#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "rclcpp/rclcpp.hpp"

namespace executor_comparison
{

struct ExecutorSpec
{
    std::string key;             // canonical key, as used on the CLI and in filenames
    std::string class_name;      // rclcpp class this maps onto
    bool        honours_threads; // whether num_threads does anything
    // True for executors this container cannot construct because they are
    // driven by a component manager of their own. Listed so the key resolves
    // and the error explains where to find it, rather than "unknown".
    bool        external_container;
    std::string note;
};

// Only executors that are actually compiled in are listed — the set is probed
// with __has_include so the same source builds on distros either side of the
// StaticSingleThreadedExecutor removal and the EventsCBGExecutor addition.
const std::vector<ExecutorSpec> & available_executors();

// Accepts both spellings, so `events-cbg` (upstream component_container's
// --executor-type vocabulary) and `events_cbg` (ours, filename friendly) both
// resolve. Returns "" if the key is not available in this build.
std::string canonical_key(const std::string & key);

bool is_available(const std::string & key);

// Throws std::runtime_error, listing the available keys, if `key` is unknown.
// num_threads == 0 means "hardware concurrency" for the executors that take a
// thread count, and is ignored by the single-threaded ones.
std::shared_ptr<rclcpp::Executor> make_executor(const std::string & key, std::size_t num_threads);

}  // namespace executor_comparison
