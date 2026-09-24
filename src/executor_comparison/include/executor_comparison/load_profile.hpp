#pragma once

#include <cstddef>
#include <string>

namespace executor_comparison
{

// Rate assigned to topic `index` of `num_topics`.
//
// For "burst" and "round_robin" every topic runs at `uniform_hz`. For
// "multi_rate" the rates are spread linearly across [min_hz, max_hz], which
// mirrors the mixed-frequency topologies used in the ros2-performance derived
// benchmarks (50 topics spanning roughly 63 Hz to 2 kHz).
//
// Deliberately a pure function of the run's parameters: the load generator
// uses it to set up its timers and the metrics writer uses it to work out how
// many messages each subscription *should* have received, and the two must
// agree without talking to each other.
double topic_rate_hz(
    std::size_t         index,
    std::size_t         num_topics,
    const std::string & publish_mode,
    double              uniform_hz,
    double              min_hz,
    double              max_hz);

}  // namespace executor_comparison
