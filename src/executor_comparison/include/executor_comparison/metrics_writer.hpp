#pragma once

#include <string>

#include "rclcpp/rclcpp.hpp"

#include "executor_comparison/resource_sampler.hpp"
#include "executor_comparison/run_metadata.hpp"

namespace executor_comparison
{

// Owns the run's output. Loaded into the container alongside the bench nodes,
// it samples process-level resource use for the lifetime of the run and, on
// destruction, drains the MetricsRegistry and writes the CSVs.
//
// This lives in a component rather than in the container executable so that
// the harness works with *any* component container — including
// callback_isolated_executor's, which is a separate upstream binary we cannot
// add code to. The samples are held by shared_ptr in the process-global
// registry, so they survive regardless of which component is destroyed first.
class MetricsWriter : public rclcpp::Node
{
public:
    explicit MetricsWriter(const rclcpp::NodeOptions & options);
    ~MetricsWriter() override;

private:
    RunMetadata     meta_;
    std::string     output_dir_;
    ResourceSampler sampler_;
};

}  // namespace executor_comparison
