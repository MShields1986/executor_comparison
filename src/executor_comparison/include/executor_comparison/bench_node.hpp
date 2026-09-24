#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rclcpp/rclcpp.hpp"
#include "executor_comparison_msgs/msg/executor_probe.hpp"

#include "executor_comparison/callback_recorder.hpp"

namespace executor_comparison
{

// The system under test: a bag of subscriptions and timers whose callbacks do
// a configurable amount of work and record when the executor got round to
// them. Loaded into the component container so the executor being benchmarked
// is the only thing dispatching it.
class BenchNode : public rclcpp::Node
{
public:
    explicit BenchNode(const rclcpp::NodeOptions & options);

private:
    using Probe = executor_comparison_msgs::msg::ExecutorProbe;

    rclcpp::CallbackGroup::SharedPtr group_for_entity();

    void on_probe(std::size_t slot, const std::shared_ptr<const Probe> & msg);
    void on_timer(std::size_t slot);

    std::string callback_group_type_;
    int64_t     work_ns_{0};

    // Shared group for the "mutually_exclusive"/"reentrant" modes; unused in
    // "default" and "per_entity".
    rclcpp::CallbackGroup::SharedPtr        shared_group_;
    std::vector<rclcpp::CallbackGroup::SharedPtr> owned_groups_;

    std::vector<rclcpp::Subscription<Probe>::SharedPtr> subs_;
    std::vector<std::shared_ptr<CallbackRecorder>>      sub_recorders_;

    std::vector<rclcpp::TimerBase::SharedPtr>      timers_;
    std::vector<std::shared_ptr<CallbackRecorder>> timer_recorders_;
    std::vector<int64_t>                           timer_prev_fire_ns_;
    std::vector<uint64_t>                          timer_seq_;
    int64_t                                        timer_period_ns_{0};
};

}  // namespace executor_comparison
