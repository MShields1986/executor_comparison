#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rclcpp/rclcpp.hpp"
#include "executor_comparison_msgs/msg/executor_probe.hpp"

namespace executor_comparison
{

// Drives the executor under test. Runs in its own process by default so its
// own publish timer does not consume the executor being measured; load it as
// a composable node instead (generator_mode:=composed) when you want the
// intra-process path exercised.
class LoadGenerator : public rclcpp::Node
{
public:
    explicit LoadGenerator(const rclcpp::NodeOptions & options);

private:
    using Probe = executor_comparison_msgs::msg::ExecutorProbe;

    void start(std::chrono::duration<double> tick_period);
    void tick();
    void publish_one(std::size_t slot);

    std::string              publish_mode_{"burst"};
    std::size_t              num_topics_{1};
    std::size_t              round_robin_cursor_{0};
    std::vector<uint64_t>    seq_;
    std::vector<double>      topic_rates_hz_;
    std::string              run_id_;
    std::size_t              payload_bytes_{0};

    std::vector<rclcpp::Publisher<Probe>::SharedPtr> pubs_;
    rclcpp::TimerBase::SharedPtr                     start_timer_;
    // One shared tick timer for burst/round_robin; one timer per topic for
    // multi_rate, since each topic then runs at its own frequency.
    rclcpp::TimerBase::SharedPtr                     timer_;
    std::vector<rclcpp::TimerBase::SharedPtr>        topic_timers_;
};

}  // namespace executor_comparison
