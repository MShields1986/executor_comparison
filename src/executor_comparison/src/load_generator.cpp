#include "executor_comparison/load_generator.hpp"

#include <algorithm>
#include <chrono>

#include <malloc.h>

#include "rclcpp_components/register_node_macro.hpp"

#include "executor_comparison/callback_recorder.hpp"  // monotonic_ns()
#include "executor_comparison/load_profile.hpp"

namespace executor_comparison
{

LoadGenerator::LoadGenerator(const rclcpp::NodeOptions & options)
: rclcpp::Node("load_generator", options)
{
    const auto topic_prefix      = this->declare_parameter<std::string>("topic_prefix", "/probe/topic_");
    const int  num_topics        = static_cast<int>(this->declare_parameter<int>("num_topics", 1));
    const int  first_topic_index = static_cast<int>(this->declare_parameter<int>("first_topic_index", 0));
    const double publish_rate_hz = this->declare_parameter<double>("publish_rate_hz", 100.0);
    const int  payload_bytes     = static_cast<int>(this->declare_parameter<int>("payload_bytes", 1024));
    const int  qos_depth         = static_cast<int>(this->declare_parameter<int>("qos_depth", 10));
    const double warmup_s        = this->declare_parameter<double>("warmup_s", 5.0);
    const double rate_min_hz     = this->declare_parameter<double>("rate_min_hz", 63.0);
    const double rate_max_hz     = this->declare_parameter<double>("rate_max_hz", 2000.0);
    publish_mode_ = this->declare_parameter<std::string>("publish_mode", "burst");

    if (publish_mode_ != "burst" && publish_mode_ != "round_robin" &&
        publish_mode_ != "multi_rate") {
        RCLCPP_WARN(this->get_logger(),
            "unknown publish_mode '%s', falling back to 'burst'", publish_mode_.c_str());
        publish_mode_ = "burst";
    }

    // Above glibc's default 128 KB threshold every message body is a fresh
    // mmap and munmap: a syscall pair plus a page fault per 4 KB on first
    // touch. At megabyte payloads that bounds the run on the allocator rather
    // than on the executor, so keep large blocks on the heap where freed ones
    // get reused.
    if (payload_bytes > 128 * 1024) {
        mallopt(M_MMAP_THRESHOLD, 512 * 1024 * 1024);
        mallopt(M_TRIM_THRESHOLD, 512 * 1024 * 1024);
    }

    num_topics_ = static_cast<std::size_t>(std::max(num_topics, 1));
    seq_.assign(num_topics_, 0);

    topic_rates_hz_.resize(num_topics_);
    for (std::size_t i = 0; i < num_topics_; ++i) {
        topic_rates_hz_[i] = topic_rate_hz(
            i, num_topics_, publish_mode_, publish_rate_hz, rate_min_hz, rate_max_hz);
    }

    auto qos = rclcpp::QoS(rclcpp::KeepLast(static_cast<std::size_t>(std::max(qos_depth, 1))));
    qos.reliable();

    for (std::size_t i = 0; i < num_topics_; ++i) {
        const std::string topic =
            topic_prefix + std::to_string(first_topic_index + static_cast<int>(i));
        pubs_.push_back(this->create_publisher<Probe>(topic, qos));
    }

    run_id_        = this->declare_parameter<std::string>("run_id", "run");
    payload_bytes_ = static_cast<std::size_t>(std::max(payload_bytes, 0));

    // In "burst" every topic fires on the same tick, which is the harsher
    // case: one wakeup with N entities ready at once. "round_robin" spreads
    // the same aggregate rate evenly, one topic per tick. "multi_rate" gives
    // every topic its own timer at its own frequency.
    const double tick_hz = (publish_mode_ == "round_robin")
        ? publish_rate_hz * static_cast<double>(num_topics_)
        : publish_rate_hz;
    const auto tick_period = std::chrono::duration<double>(1.0 / tick_hz);

    start_timer_ = this->create_wall_timer(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<double>(warmup_s)),
        [this, tick_period]() {
            start_timer_->cancel();
            this->start(tick_period);
        });

    if (publish_mode_ == "multi_rate") {
        double aggregate = 0.0;
        for (double r : topic_rates_hz_) aggregate += r;
        RCLCPP_INFO(this->get_logger(),
            "LoadGenerator %zu topics from %s%d @ %.1f-%.1f Hz (multi_rate, "
            "%.0f msg/s aggregate), payload=%d B, starting in %.1f s",
            num_topics_, topic_prefix.c_str(), first_topic_index,
            topic_rates_hz_.front(), topic_rates_hz_.back(), aggregate,
            payload_bytes, warmup_s);
    } else {
        RCLCPP_INFO(this->get_logger(),
            "LoadGenerator %zu topics from %s%d @ %.1f Hz each (%s), payload=%d B, "
            "starting in %.1f s",
            num_topics_, topic_prefix.c_str(), first_topic_index, publish_rate_hz,
            publish_mode_.c_str(), payload_bytes, warmup_s);
    }
}

void LoadGenerator::start(std::chrono::duration<double> tick_period)
{
    if (publish_mode_ == "multi_rate") {
        topic_timers_.reserve(num_topics_);
        for (std::size_t i = 0; i < num_topics_; ++i) {
            const auto period = std::chrono::duration<double>(1.0 / topic_rates_hz_[i]);
            topic_timers_.push_back(this->create_wall_timer(
                std::chrono::duration_cast<std::chrono::nanoseconds>(period),
                [this, i]() { this->publish_one(i); }));
        }
        return;
    }

    timer_ = this->create_wall_timer(
        std::chrono::duration_cast<std::chrono::nanoseconds>(tick_period),
        std::bind(&LoadGenerator::tick, this));
}

void LoadGenerator::tick()
{
    if (publish_mode_ == "round_robin") {
        publish_one(round_robin_cursor_);
        round_robin_cursor_ = (round_robin_cursor_ + 1) % num_topics_;
        return;
    }
    for (std::size_t i = 0; i < num_topics_; ++i) {
        publish_one(i);
    }
}

void LoadGenerator::publish_one(std::size_t slot)
{
    // Built fresh rather than copy-constructed from a template. Ownership has
    // to transfer for intra-process publishing to move rather than copy, so a
    // new message per publish is unavoidable — but copying a template reads
    // the old payload as well as writing the new one, which at megabyte sizes
    // costs more than everything the executor does and buries the signal.
    auto msg = std::make_unique<Probe>();
    msg->payload.resize(payload_bytes_);
    msg->header.frame_id = run_id_;
    msg->seq         = seq_[slot]++;
    msg->topic_index = static_cast<uint32_t>(slot);
    msg->header.stamp = this->now();

    // Last thing before the handoff, so the copy above is not counted as
    // executor latency.
    msg->t_published_ns = monotonic_ns();
    pubs_[slot]->publish(std::move(msg));
}

}  // namespace executor_comparison

RCLCPP_COMPONENTS_REGISTER_NODE(executor_comparison::LoadGenerator)
