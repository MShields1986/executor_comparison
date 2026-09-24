#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

namespace executor_comparison
{

// Background sampler for process-level cost.
//
// Dispatch latency alone does not separate the executors: the headline claim
// for the events-based ones is 10-15% less CPU and less context switching for
// the same work, so the run is only interesting alongside these numbers.
class ResourceSampler
{
public:
    struct Stats
    {
        double   cpu_percent_mean{0.0};
        double   cpu_percent_max{0.0};
        double   rss_mb_mean{0.0};
        double   rss_mb_max{0.0};
        double   wall_s{0.0};
        double   cpu_s_total{0.0};
        uint64_t voluntary_ctx_switches{0};
        uint64_t involuntary_ctx_switches{0};
        uint64_t samples{0};
    };

    explicit ResourceSampler(double sample_hz = 10.0);
    ~ResourceSampler();

    ResourceSampler(const ResourceSampler &) = delete;
    ResourceSampler & operator=(const ResourceSampler &) = delete;

    // Baseline is taken at start(), so warmup is included unless start() is
    // deferred. The container starts it right before spin().
    void start();
    void stop();

    Stats stats() const { return stats_; }

private:
    void run();

    double            sample_hz_;
    std::atomic<bool> running_{false};
    std::thread       thread_;
    Stats             stats_;
};

}  // namespace executor_comparison
