#include "executor_comparison/executor_factory.hpp"

#include <sstream>
#include <stdexcept>

#include "rclcpp/executors/single_threaded_executor.hpp"
#include "rclcpp/executors/multi_threaded_executor.hpp"

// Lyrical Luth onwards. Events queue rather than a wait set, with support for
// multiple threads and multiple ROS time sources.
#if __has_include("rclcpp/executors/events_cbg_executor/events_cbg_executor.hpp")
#  include "rclcpp/executors/events_cbg_executor/events_cbg_executor.hpp"
#  define EC_HAS_EVENTS_CBG 1
#endif

// The EventsCBGExecutor's predecessor. Still shipped, still experimental,
// single threaded only.
#if __has_include("rclcpp/experimental/executors/events_executor/events_executor.hpp")
#  include "rclcpp/experimental/executors/events_executor/events_executor.hpp"
#  define EC_HAS_EVENTS 1
#endif

// Deprecated in Jazzy, removed in Lyrical. Kept behind the probe so this
// package still builds (and still benchmarks it) on older distros.
#if __has_include("rclcpp/executors/static_single_threaded_executor.hpp")
#  include "rclcpp/executors/static_single_threaded_executor.hpp"
#  define EC_HAS_STATIC_SINGLE 1
#endif

namespace executor_comparison
{

const std::vector<ExecutorSpec> & available_executors()
{
    static const std::vector<ExecutorSpec> specs = [] {
        std::vector<ExecutorSpec> s;
        s.push_back({
            "single_threaded",
            "rclcpp::executors::SingleThreadedExecutor",
            false,
            false,
            "wait set, one thread; the baseline and what plain rclcpp::spin() gives you"});
        s.push_back({
            "multi_threaded",
            "rclcpp::executors::MultiThreadedExecutor",
            true,
            false,
            "wait set, N threads; parallelism is bounded by the callback groups"});
#ifdef EC_HAS_EVENTS_CBG
        s.push_back({
            "events_cbg",
            "rclcpp::executors::EventsCBGExecutor",
            true,
            false,
            "events queue, N threads; new in Lyrical"});
#endif
#ifdef EC_HAS_EVENTS
        s.push_back({
            "events",
            "rclcpp::experimental::executors::EventsExecutor",
            false,
            false,
            "events queue, one thread; experimental, predates events_cbg"});
#endif
#ifdef EC_HAS_STATIC_SINGLE
        s.push_back({
            "static_single_threaded",
            "rclcpp::executors::StaticSingleThreadedExecutor",
            false,
            false,
            "removed in Lyrical; only present when built against an older distro"});
#endif
        s.push_back({
            "callback_isolated",
            "CallbackIsolatedExecutor (autowarefoundation/callback_isolated_executor)",
            true,
            true,
            "one thread per callback group; driven by its own component "
            "container, num_threads maps to reentrant_parallelism"});
        return s;
    }();
    return specs;
}

std::string canonical_key(const std::string & key)
{
    std::string normalised;
    normalised.reserve(key.size());
    for (char c : key) normalised.push_back(c == '-' ? '_' : c);

    for (const auto & spec : available_executors()) {
        if (spec.key == normalised) return spec.key;
    }
    return {};
}

bool is_available(const std::string & key)
{
    return !canonical_key(key).empty();
}

namespace
{
std::string key_list()
{
    std::ostringstream ss;
    bool first = true;
    for (const auto & spec : available_executors()) {
        if (!first) ss << ", ";
        ss << spec.key;
        first = false;
    }
    return ss.str();
}
}  // namespace

std::shared_ptr<rclcpp::Executor> make_executor(const std::string & key, std::size_t num_threads)
{
    const std::string k = canonical_key(key);

    for (const auto & spec : available_executors()) {
        if (spec.key == k && spec.external_container) {
            throw std::runtime_error(
                "executor '" + k + "' is not constructible here: it is driven by its own "
                "component container (callback_isolated_executor's "
                "component_container_callback_isolated). The launch file selects that "
                "container automatically for executor:=" + k + ".");
        }
    }

    if (k == "single_threaded") {
        return std::make_shared<rclcpp::executors::SingleThreadedExecutor>();
    }
    if (k == "multi_threaded") {
        return std::make_shared<rclcpp::executors::MultiThreadedExecutor>(
            rclcpp::ExecutorOptions(), num_threads);
    }
#ifdef EC_HAS_EVENTS_CBG
    if (k == "events_cbg") {
        return std::make_shared<rclcpp::executors::EventsCBGExecutor>(
            rclcpp::ExecutorOptions(), num_threads);
    }
#endif
#ifdef EC_HAS_EVENTS
    if (k == "events") {
        return std::make_shared<rclcpp::experimental::executors::EventsExecutor>();
    }
#endif
#ifdef EC_HAS_STATIC_SINGLE
    if (k == "static_single_threaded") {
        return std::make_shared<rclcpp::executors::StaticSingleThreadedExecutor>();
    }
#endif

    throw std::runtime_error(
        "unknown or unavailable executor '" + key + "'; available: " + key_list());
}

}  // namespace executor_comparison
