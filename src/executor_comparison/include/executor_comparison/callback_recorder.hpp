#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace executor_comparison
{

enum class EntityType : uint8_t
{
    Subscription = 0,
    Timer        = 1,
};

const char * to_string(EntityType type);

struct Sample
{
    // For subscriptions this is the publisher's pre-publish stamp; for timers
    // it is the wall time the tick was *due*, so latency_ns is timer drift.
    int64_t    t_reference_ns{0};
    int64_t    t_start_ns{0};
    int64_t    t_end_ns{0};
    uint64_t   seq{0};
    uint32_t   entity_index{0};
    uint32_t   thread_slot{0};
    EntityType entity_type{EntityType::Subscription};
};

// Every timestamp in this package comes from here. CLOCK_MONOTONIC is system
// wide on Linux, so a stamp taken in the load generator process is directly
// comparable with one taken inside the executor under test.
int64_t monotonic_ns();

// Dense id for the calling thread, assigned on first use. Lets the CSV show
// how many executor threads actually served a given entity, which is the
// difference between a reentrant callback group doing its job and not.
uint32_t thread_slot();

// Busy-wait for `ns` without sleeping, to emulate callback work. Sleeping
// would hand the core back to the executor and defeat the point.
void busy_wait_ns(int64_t ns);

// Wait-free per-entity sample buffer.
//
// Callbacks only bump an atomic index and write into preallocated storage: no
// allocation, no locking, and crucially no ROS traffic on the hot path. The
// obvious alternative — publishing a record message per callback, as
// ros_latency_tests does — would push the executor under test through an
// extra publish per callback and measure that instead.
class CallbackRecorder
{
public:
    CallbackRecorder(EntityType type, uint32_t entity_index, std::size_t capacity);

    void record(uint64_t seq, int64_t t_reference_ns, int64_t t_start_ns, int64_t t_end_ns);

    EntityType  type()  const { return type_; }
    uint32_t    index() const { return entity_index_; }

    // Samples actually stored (capped at capacity).
    std::size_t recorded() const;
    // Samples that arrived after the buffer filled up and were thrown away.
    std::size_t overflowed() const;

    const std::vector<Sample> & buffer() const { return buffer_; }

private:
    EntityType                  type_;
    uint32_t                    entity_index_;
    std::vector<Sample>         buffer_;
    std::atomic<std::size_t>    write_index_{0};
};

// Process-global registry.
//
// Composable nodes are constructed independently and cannot hand each other
// pointers, but they all live in one process, so the bench nodes register
// their recorders here as they come up and the container drains the registry
// once spin() returns. That keeps a whole run in a single CSV no matter how
// many nodes the entities were spread over.
class MetricsRegistry
{
public:
    static MetricsRegistry & instance();

    void add(const std::shared_ptr<CallbackRecorder> & recorder);
    std::vector<std::shared_ptr<CallbackRecorder>> recorders() const;
    void clear();

private:
    MetricsRegistry() = default;

    mutable std::mutex                             mu_;
    std::vector<std::shared_ptr<CallbackRecorder>> recorders_;
};

}  // namespace executor_comparison
