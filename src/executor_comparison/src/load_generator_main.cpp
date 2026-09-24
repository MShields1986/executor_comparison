// Standalone entry point for the load generator.
//
// This is the default (generator_mode:=process): the generator gets its own
// process and its own executor, so its publish timer never competes for the
// executor being benchmarked. Load it as a composable node instead
// (generator_mode:=composed) when the intra-process path is what you want to
// measure.

#include <memory>

#include "rclcpp/rclcpp.hpp"
#include "executor_comparison/load_generator.hpp"

int main(int argc, char ** argv)
{
    rclcpp::init(argc, argv);

    rclcpp::NodeOptions options;
    auto node = std::make_shared<executor_comparison::LoadGenerator>(options);

    rclcpp::executors::SingleThreadedExecutor executor;
    executor.add_node(node);
    executor.spin();

    rclcpp::shutdown();
    return 0;
}
