// Component container whose executor is chosen at run time.
//
// Mirrors upstream `component_container --executor-type <type> --ros-args -p
// thread_num:=N`, extended with the experimental EventsExecutor and using
// `num_threads` in place of `thread_num` (ComponentManager declares that one
// itself, with an integer range that rejects 0).
//
// The run's CSVs are written by the MetricsWriter component, not here, so
// that the same bench nodes also work under a container we do not own —
// specifically callback_isolated_executor's.

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp_components/component_manager.hpp"

#include "executor_comparison/executor_factory.hpp"

namespace
{

std::string parse_executor_type(const std::vector<std::string> & args)
{
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--executor-type" && i + 1 < args.size()) {
            return args[i + 1];
        }
        const std::string prefix = "--executor-type=";
        if (args[i].rfind(prefix, 0) == 0) {
            return args[i].substr(prefix.size());
        }
    }
    return "single_threaded";
}

}  // namespace

int main(int argc, char ** argv)
{
    rclcpp::init(argc, argv);

    const auto args = rclcpp::remove_ros_arguments(argc, argv);
    const std::string requested = parse_executor_type(args);
    const std::string executor_key = executor_comparison::canonical_key(requested);

    auto logger = rclcpp::get_logger("executor_container");

    if (executor_key.empty()) {
        RCLCPP_ERROR(logger, "unknown --executor-type '%s'", requested.c_str());
        RCLCPP_ERROR(logger, "available executors on this build:");
        for (const auto & spec : executor_comparison::available_executors()) {
            RCLCPP_ERROR(logger, "  %-24s %s", spec.key.c_str(), spec.class_name.c_str());
        }
        rclcpp::shutdown();
        return 2;
    }

    // Parameter services and the parameter event publisher would add entities
    // to the very executor under test, so they stay off.
    const auto node_options = rclcpp::NodeOptions()
        .start_parameter_services(false)
        .start_parameter_event_publisher(false);

    std::weak_ptr<rclcpp::Executor> no_executor_yet;
    auto manager = std::make_shared<rclcpp_components::ComponentManager>(
        no_executor_yet, "executor_container", node_options);

    const int thread_num = manager->has_parameter("num_threads")
        ? static_cast<int>(manager->get_parameter("num_threads").as_int())
        : static_cast<int>(manager->declare_parameter<int>("num_threads", 0));
    const auto num_threads = static_cast<std::size_t>(std::max(thread_num, 0));

    std::shared_ptr<rclcpp::Executor> executor;
    try {
        executor = executor_comparison::make_executor(executor_key, num_threads);
    } catch (const std::exception & e) {
        RCLCPP_ERROR(logger, "%s", e.what());
        rclcpp::shutdown();
        return 2;
    }

    RCLCPP_INFO(logger, "executor=%s threads=%d", executor_key.c_str(), thread_num);

    manager->set_executor(executor);
    executor->add_node(manager);
    executor->spin();

    rclcpp::shutdown();
    return 0;
}
