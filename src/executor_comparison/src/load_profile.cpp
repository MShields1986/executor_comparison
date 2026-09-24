#include "executor_comparison/load_profile.hpp"

namespace executor_comparison
{

double topic_rate_hz(
    std::size_t         index,
    std::size_t         num_topics,
    const std::string & publish_mode,
    double              uniform_hz,
    double              min_hz,
    double              max_hz)
{
    if (publish_mode != "multi_rate") {
        return uniform_hz;
    }
    if (num_topics <= 1) {
        return max_hz;
    }
    // Linear rather than the random assignment the reference topologies use:
    // a benchmark axis should be reproducible from its parameters alone.
    const double t = static_cast<double>(index) / static_cast<double>(num_topics - 1);
    return min_hz + (max_hz - min_hz) * t;
}

}  // namespace executor_comparison
