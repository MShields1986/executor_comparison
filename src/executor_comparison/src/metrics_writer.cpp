#include "executor_comparison/metrics_writer.hpp"

#include "rclcpp_components/register_node_macro.hpp"

#include "executor_comparison/callback_recorder.hpp"

namespace executor_comparison
{

MetricsWriter::MetricsWriter(const rclcpp::NodeOptions & options)
: rclcpp::Node("metrics_writer", options),
  sampler_(10.0)
{
    meta_.run_id            = this->declare_parameter<std::string>("run_id", "run");
    meta_.executor          = this->declare_parameter<std::string>("executor", "unknown");
    meta_.num_threads       = static_cast<int>(this->declare_parameter<int>("num_threads", 0));
    meta_.num_nodes         = static_cast<int>(this->declare_parameter<int>("num_nodes", 1));
    meta_.num_subscriptions = static_cast<int>(this->declare_parameter<int>("num_subscriptions", 0));
    meta_.num_timers        = static_cast<int>(this->declare_parameter<int>("num_timers", 0));
    meta_.timer_period_ms   = this->declare_parameter<double>("timer_period_ms", 0.0);
    meta_.publish_rate_hz   = this->declare_parameter<double>("publish_rate_hz", 0.0);
    meta_.publish_mode      = this->declare_parameter<std::string>("publish_mode", "burst");
    meta_.rate_min_hz       = this->declare_parameter<double>("rate_min_hz", 0.0);
    meta_.rate_max_hz       = this->declare_parameter<double>("rate_max_hz", 0.0);
    meta_.payload_bytes     = static_cast<int>(this->declare_parameter<int>("payload_bytes", 0));
    meta_.callback_work_us  = this->declare_parameter<double>("callback_work_us", 0.0);
    meta_.callback_group    = this->declare_parameter<std::string>("callback_group", "default");
    meta_.use_intra_process_comms =
        this->declare_parameter<bool>("use_intra_process_comms", false);
    meta_.generator_mode    = this->declare_parameter<std::string>("generator_mode", "process");
    meta_.qos_depth         = static_cast<int>(this->declare_parameter<int>("qos_depth", 10));
    meta_.duration_s        = this->declare_parameter<double>("duration_s", 0.0);
    output_dir_             = this->declare_parameter<std::string>("output_dir", "/data/results");

    collect_host_info(meta_);
    sampler_.start();

    RCLCPP_INFO(this->get_logger(),
        "MetricsWriter armed for executor=%s rmw=%s -> %s",
        meta_.executor.c_str(), meta_.rmw.c_str(), output_dir_.c_str());
}

MetricsWriter::~MetricsWriter()
{
    sampler_.stop();

    const auto recorders = MetricsRegistry::instance().recorders();
    if (recorders.empty()) {
        RCLCPP_WARN(this->get_logger(), "no callback recorders registered, nothing to write");
        return;
    }

    const auto stem = write_results(meta_, output_dir_, recorders, sampler_.stats());
    RCLCPP_INFO(this->get_logger(), "wrote %s.csv (+ _summary.csv)", stem.c_str());
}

}  // namespace executor_comparison

RCLCPP_COMPONENTS_REGISTER_NODE(executor_comparison::MetricsWriter)
