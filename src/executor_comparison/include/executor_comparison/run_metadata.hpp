#pragma once

#include <memory>
#include <string>
#include <vector>

#include "executor_comparison/callback_recorder.hpp"
#include "executor_comparison/resource_sampler.hpp"

namespace executor_comparison
{

// Everything that goes into the `#`-prefixed CSV header. The host fields are
// filled in by collect_host_info(); the rest are read off the container's
// parameters so the plot can describe the machine that *produced* the data,
// not the one rendering it.
struct RunMetadata
{
    // Run configuration
    std::string run_id;
    std::string executor{"unknown"};
    int         num_threads{0};
    int         num_nodes{1};
    int         num_subscriptions{0};
    int         num_timers{0};
    double      timer_period_ms{0.0};
    double      publish_rate_hz{0.0};
    std::string publish_mode{"burst"};
    double      rate_min_hz{0.0};
    double      rate_max_hz{0.0};
    int         payload_bytes{0};
    double      callback_work_us{0.0};
    std::string callback_group{"mutually_exclusive"};
    bool        use_intra_process_comms{false};
    std::string generator_mode{"process"};
    int         qos_depth{10};
    double      duration_s{0.0};

    // Host / environment, filled by collect_host_info()
    std::string rmw{"unknown_rmw"};
    std::string ros_distro{"?"};
    std::string host{"unknown"};
    std::string cpu_model{"unknown"};
    int         cpu_cores{0};
    std::string os_pretty{"unknown"};
    std::string kernel{"unknown"};
    std::string start_time;
};

void collect_host_info(RunMetadata & meta);

// data/results/executor_<exec>_<threads>t_<nodes>n_<subs>s_<timers>tm_..._<ts>
std::string build_filename_stem(const RunMetadata & meta, const std::string & output_dir);

// Writes <stem>.csv (one row per callback) and <stem>_summary.csv
// (percentiles per entity plus the process-level resource stats). Returns the
// stem so the caller can log where the run landed.
std::string write_results(
    const RunMetadata &                                    meta,
    const std::string &                                    output_dir,
    const std::vector<std::shared_ptr<CallbackRecorder>> & recorders,
    const ResourceSampler::Stats &                         resources);

}  // namespace executor_comparison
