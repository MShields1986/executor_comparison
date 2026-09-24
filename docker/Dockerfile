ARG ROS_DISTRO=lyrical

FROM ros:${ROS_DISTRO}-ros-base

SHELL ["/bin/bash", "-c"]
ENV DEBIAN_FRONTEND=noninteractive

# Unlike ros_latency_tests, the RMW is not the axis under test here, so every
# middleware goes into the one image and is selected at run time via
# RMW_IMPLEMENTATION. That means all the executor services share a single
# build instead of one image per variant.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      git \
      "ros-${ROS_DISTRO}-rmw-cyclonedds-cpp" \
      "ros-${ROS_DISTRO}-rmw-fastrtps-cpp" \
      "ros-${ROS_DISTRO}-rmw-fastrtps-dynamic-cpp" \
 && rm -rf /var/lib/apt/lists/*

ENV RMW_IMPLEMENTATION=rmw_cyclonedds_cpp

# Increase kernel network buffers (helps loopback DDS transports)
RUN echo 'net.core.rmem_max=2147483647'     >> /etc/sysctl.conf \
 && echo 'net.core.rmem_default=8388608'    >> /etc/sysctl.conf

# ---------------------------------------------------------------------------
# TIER IV / Autoware callback_isolated_executor, pinned.
#
# Cloned rather than vendored, and carried forward with a patch, so it stays
# visibly upstream's code. The patch is a Lyrical port: upstream targets
# Humble/Jazzy and does not build here as-is. See patches/README.md.
# ---------------------------------------------------------------------------
ARG CIE_COMMIT=a3e5102ec8a1d318e0f00b4e06af849be67f0f76
RUN git clone https://github.com/autowarefoundation/callback_isolated_executor.git \
        /ros2_ws/src/callback_isolated_executor \
 && cd /ros2_ws/src/callback_isolated_executor \
 && git checkout --quiet "${CIE_COMMIT}" \
 && touch cie_sample_application/COLCON_IGNORE

COPY patches /tmp/patches
RUN cd /ros2_ws/src/callback_isolated_executor \
 && git apply --verbose /tmp/patches/0001-lyrical-port.patch

# Build the workspace
COPY src /ros2_ws/src
WORKDIR /ros2_ws
RUN apt-get update \
 && source /opt/ros/${ROS_DISTRO}/setup.bash \
 && rosdep update \
 && rosdep install -i --from-path src --rosdistro ${ROS_DISTRO} -y \
 && rm -rf /var/lib/apt/lists/* \
 && JOBS=$(( $(nproc) / 2 > 0 ? $(nproc) / 2 : 1 )) \
 && MAKEFLAGS="-j${JOBS}" colcon build --symlink-install \
        --parallel-workers "${JOBS}" \
        --cmake-args -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
 && echo 'source /opt/ros/${ROS_DISTRO}/setup.bash' >> /root/.bashrc \
 && echo 'source /ros2_ws/install/setup.bash'       >> /root/.bashrc \
 && sed -i '/exec "$@"/d' /ros_entrypoint.sh \
 && echo 'source /ros2_ws/install/setup.bash'       >> /ros_entrypoint.sh \
 && echo 'exec "$@"'                                >> /ros_entrypoint.sh

ENTRYPOINT [ "/ros_entrypoint.sh" ]

ENV DEBIAN_FRONTEND=dialog
