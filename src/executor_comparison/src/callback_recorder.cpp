#include "executor_comparison/callback_recorder.hpp"

#include <algorithm>
#include <ctime>

namespace executor_comparison
{

const char * to_string(EntityType type)
{
    switch (type) {
        case EntityType::Subscription: return "subscription";
        case EntityType::Timer:        return "timer";
    }
    return "unknown";
}

int64_t monotonic_ns()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + static_cast<int64_t>(ts.tv_nsec);
}

uint32_t thread_slot()
{
    static std::atomic<uint32_t> next{0};
    thread_local const uint32_t slot = next.fetch_add(1, std::memory_order_relaxed);
    return slot;
}

void busy_wait_ns(int64_t ns)
{
    if (ns <= 0) return;
    const int64_t deadline = monotonic_ns() + ns;
    while (monotonic_ns() < deadline) {
        // Spin rather than sleep: yielding the core would hand time back to
        // the executor and stop this looking like a busy callback.
    }
}

CallbackRecorder::CallbackRecorder(EntityType type, uint32_t entity_index, std::size_t capacity)
: type_(type),
  entity_index_(entity_index),
  buffer_(std::max<std::size_t>(capacity, 1))
{
}

void CallbackRecorder::record(
    uint64_t seq, int64_t t_reference_ns, int64_t t_start_ns, int64_t t_end_ns)
{
    const std::size_t i = write_index_.fetch_add(1, std::memory_order_relaxed);
    if (i >= buffer_.size()) return;  // buffer full; counted by overflowed()

    Sample & s = buffer_[i];
    s.t_reference_ns = t_reference_ns;
    s.t_start_ns     = t_start_ns;
    s.t_end_ns       = t_end_ns;
    s.seq            = seq;
    s.entity_index   = entity_index_;
    s.thread_slot    = thread_slot();
    s.entity_type    = type_;
}

std::size_t CallbackRecorder::recorded() const
{
    return std::min(write_index_.load(std::memory_order_relaxed), buffer_.size());
}

std::size_t CallbackRecorder::overflowed() const
{
    const std::size_t written = write_index_.load(std::memory_order_relaxed);
    return written > buffer_.size() ? written - buffer_.size() : 0;
}

MetricsRegistry & MetricsRegistry::instance()
{
    static MetricsRegistry registry;
    return registry;
}

void MetricsRegistry::add(const std::shared_ptr<CallbackRecorder> & recorder)
{
    std::lock_guard<std::mutex> lk(mu_);
    recorders_.push_back(recorder);
}

std::vector<std::shared_ptr<CallbackRecorder>> MetricsRegistry::recorders() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return recorders_;
}

void MetricsRegistry::clear()
{
    std::lock_guard<std::mutex> lk(mu_);
    recorders_.clear();
}

}  // namespace executor_comparison
