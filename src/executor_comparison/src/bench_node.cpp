#include "executor_comparison/bench_node.hpp"

#include <algorithm>
#include <chrono>
#include <functional>

#include "rclcpp_components/register_node_macro.hpp"

namespace executor_comparison
{

BenchNode::BenchNode(const rclcpp::NodeOptions & options)
: rclcpp::Node("bench_node", options)
{
    const int  node_index        = static_cast<int>(this->declare_parameter<int>("node_index", 0));
    const auto topic_prefix      = this->declare_parameter<std::string>("topic_prefix", "/probe/topic_");
    const int  first_topic_index = static_cast<int>(this->declare_parameter<int>("first_topic_index", 0));
    const int  num_subscriptions = static_cast<int>(this->declare_parameter<int>("num_subscriptions", 1));
    const int  num_timers        = static_cast<int>(this->declare_parameter<int>("num_timers", 0));
    const int  first_timer_index = static_cast<int>(this->declare_parameter<int>("first_timer_index", 0));
    const double timer_period_ms = this->declare_parameter<double>("timer_period_ms", 10.0);
    const double callback_work_us = this->declare_parameter<double>("callback_work_us", 0.0);
    const int  qos_depth         = static_cast<int>(this->declare_parameter<int>("qos_depth", 10));
    const int  sample_capacity   = static_cast<int>(this->declare_parameter<int>("sample_capacity", 100000));

    callback_group_type_ = this->declare_parameter<std::string>(
        "callback_group", "mutually_exclusive");
    work_ns_ = static_cast<int64_t>(callback_work_us * 1000.0);

    if (callback_group_type_ == "mutually_exclusive") {
        shared_group_ = this->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
    } else if (callback_group_type_ == "reentrant") {
        shared_group_ = this->create_callback_group(rclcpp::CallbackGroupType::Reentrant);
    } else if (callback_group_type_ != "default" && callback_group_type_ != "per_entity") {
        RCLCPP_WARN(this->get_logger(),
            "unknown callback_group '%s', falling back to 'default'",
            callback_group_type_.c_str());
        callback_group_type_ = "default";
    }

    const auto capacity = static_cast<std::size_t>(std::max(sample_capacity, 1));
    auto qos = rclcpp::QoS(rclcpp::KeepLast(static_cast<std::size_t>(std::max(qos_depth, 1))));
    qos.reliable();

    for (int i = 0; i < num_subscriptions; ++i) {
        const auto topic_index = static_cast<uint32_t>(first_topic_index + i);
        const std::string topic = topic_prefix + std::to_string(topic_index);
        const auto slot = static_cast<std::size_t>(i);

        auto recorder = std::make_shared<CallbackRecorder>(
            EntityType::Subscription, topic_index, capacity);
        sub_recorders_.push_back(recorder);
        MetricsRegistry::instance().add(recorder);

        rclcpp::SubscriptionOptions sub_options;
        sub_options.callback_group = group_for_entity();

        subs_.push_back(this->create_subscription<Probe>(
            topic, qos,
            [this, slot](std::shared_ptr<const Probe> msg) { this->on_probe(slot, msg); },
            sub_options));
    }

    timer_period_ns_ = static_cast<int64_t>(timer_period_ms * 1e6);
    timer_prev_fire_ns_.assign(static_cast<std::size_t>(std::max(num_timers, 0)), 0);
    timer_seq_.assign(static_cast<std::size_t>(std::max(num_timers, 0)), 0);

    for (int i = 0; i < num_timers; ++i) {
        const auto timer_index = static_cast<uint32_t>(first_timer_index + i);
        const auto slot = static_cast<std::size_t>(i);

        auto recorder = std::make_shared<CallbackRecorder>(
            EntityType::Timer, timer_index, capacity);
        timer_recorders_.push_back(recorder);
        MetricsRegistry::instance().add(recorder);

        timers_.push_back(this->create_wall_timer(
            std::chrono::nanoseconds(timer_period_ns_),
            [this, slot]() { this->on_timer(slot); },
            group_for_entity()));
    }

    RCLCPP_INFO(this->get_logger(),
        "BenchNode[%d] %d subscriptions from %s%d, %d timers @ %.1f ms, "
        "work=%.1f us, callback_group=%s",
        node_index, num_subscriptions, topic_prefix.c_str(), first_topic_index,
        num_timers, timer_period_ms, callback_work_us, callback_group_type_.c_str());
}

rclcpp::CallbackGroup::SharedPtr BenchNode::group_for_entity()
{
    if (callback_group_type_ == "per_entity") {
        // One mutually exclusive group each: every entity can run in parallel
        // with every other, but never with itself. This is the layout that
        // actually lets MultiThreadedExecutor use all of its threads.
        owned_groups_.push_back(
            this->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive));
        return owned_groups_.back();
    }
    // "default" leaves it null so the node's own default group is used.
    return shared_group_;
}

void BenchNode::on_probe(std::size_t slot, const std::shared_ptr<const Probe> & msg)
{
    const int64_t t_start = monotonic_ns();
    busy_wait_ns(work_ns_);
    const int64_t t_end = monotonic_ns();

    // Recorded after the work so the bookkeeping is outside the measured span.
    sub_recorders_[slot]->record(msg->seq, msg->t_published_ns, t_start, t_end);
}

void BenchNode::on_timer(std::size_t slot)
{
    const int64_t t_start = monotonic_ns();

    // Measured against where the previous tick actually landed, not against a
    // grid anchored on the first one: the first tick is cold and late, and
    // anchoring on it biases every later sample early by that amount. So
    // latency_ns here is period error, (actual interval - nominal period),
    // and the first tick records 0.
    int64_t & prev = timer_prev_fire_ns_[slot];
    const int64_t due = (prev == 0) ? t_start : prev + timer_period_ns_;
    prev = t_start;

    busy_wait_ns(work_ns_);
    const int64_t t_end = monotonic_ns();

    timer_recorders_[slot]->record(timer_seq_[slot]++, due, t_start, t_end);
}

}  // namespace executor_comparison

RCLCPP_COMPONENTS_REGISTER_NODE(executor_comparison::BenchNode)
