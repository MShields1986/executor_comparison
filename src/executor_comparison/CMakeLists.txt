cmake_minimum_required(VERSION 3.8)
project(executor_comparison)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rmw REQUIRED)
find_package(rclcpp_components REQUIRED)
find_package(executor_comparison_msgs REQUIRED)
find_package(std_msgs REQUIRED)

# -----------------------------------------------------------------------------
# Core library: sample recording, executor factory, resource sampling, CSV I/O.
#
# Deliberately SHARED. The component nodes and the container executable both
# link it, and MetricsRegistry is a process-global singleton — two copies would
# mean the container drained an empty registry.
# -----------------------------------------------------------------------------
add_library(executor_comparison_core SHARED
  src/callback_recorder.cpp
  src/executor_factory.cpp
  src/load_profile.cpp
  src/resource_sampler.cpp
  src/run_metadata.cpp
)
target_include_directories(executor_comparison_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(executor_comparison_core PUBLIC rclcpp::rclcpp rmw::rmw)

# -----------------------------------------------------------------------------
# Components loaded into the container under test.
# -----------------------------------------------------------------------------
add_library(executor_comparison_components SHARED
  src/bench_node.cpp
  src/load_generator.cpp
  src/metrics_writer.cpp
)
target_include_directories(executor_comparison_components PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(executor_comparison_components PUBLIC
  executor_comparison_core
  rclcpp::rclcpp
  rclcpp_components::component_manager
  ${std_msgs_TARGETS}
  ${executor_comparison_msgs_TARGETS}
)
rclcpp_components_register_nodes(executor_comparison_components
  "executor_comparison::BenchNode"
  "executor_comparison::LoadGenerator"
  "executor_comparison::MetricsWriter"
)

# -----------------------------------------------------------------------------
# Executables
# -----------------------------------------------------------------------------
add_executable(executor_container src/executor_container.cpp)
target_link_libraries(executor_container
  executor_comparison_core
  rclcpp::rclcpp
  rclcpp_components::component_manager
)

add_executable(load_generator src/load_generator_main.cpp)
target_link_libraries(load_generator
  executor_comparison_components
  executor_comparison_core
  rclcpp::rclcpp
)

# -----------------------------------------------------------------------------
# Install
# -----------------------------------------------------------------------------
install(TARGETS executor_comparison_core executor_comparison_components
  EXPORT export_${PROJECT_NAME}
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

install(TARGETS executor_container load_generator
  DESTINATION lib/${PROJECT_NAME}
)

install(DIRECTORY launch
  DESTINATION share/${PROJECT_NAME}
)

install(DIRECTORY include/
  DESTINATION include
)

ament_export_targets(export_${PROJECT_NAME} HAS_LIBRARY_TARGET)
ament_export_dependencies(rclcpp rclcpp_components executor_comparison_msgs std_msgs)

ament_package()
