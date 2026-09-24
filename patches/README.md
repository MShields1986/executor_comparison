# patches

## `0001-lyrical-port.patch`

Applied to [autowarefoundation/callback_isolated_executor][upstream] at commit
`a3e5102` during the Docker build. Upstream targets Humble/Jazzy and does not
compile on Lyrical without it. Two independent breakages, both mechanical:

1. **`ament_target_dependencies` was removed** in Lyrical (ament_cmake 2.8.8) —
   deprecated in earlier distros, gone in this one. 14 call sites across
   `cie_thread_configurator`, `callback_isolated_executor` and
   `cie_sample_application` are rewritten to the modern imported-target form,
   e.g. `rclcpp` → `rclcpp::rclcpp`, `cie_config_msgs` →
   `${cie_config_msgs_TARGETS}`.

2. **`rclcpp::Executor`'s virtuals now take `const &`.** In Lyrical,
   `add_callback_group`, `remove_callback_group`, both `add_node` overloads and
   both `remove_node` overloads take their smart pointers by const reference.
   Upstream passes by value, so the six `override` declarations in
   `CallbackIsolatedExecutor` no longer override anything and the build fails
   outright. The patch changes the six signatures in the header and their
   definitions in the `.cpp`.

Nothing else is touched: no behaviour, no scheduling logic, no thread
management. The point of benchmarking this executor is to measure *their*
implementation, so the patch is kept to the minimum needed to make it compile.

`cie_sample_application` is skipped with a `COLCON_IGNORE` in the Dockerfile —
it is demo code the harness does not use. Its CMake is patched anyway so the
patch stays complete if you drop the ignore.

Delete this patch once upstream supports Lyrical, and unpin `CIE_COMMIT` in
`docker/Dockerfile`.

[upstream]: https://github.com/autowarefoundation/callback_isolated_executor
