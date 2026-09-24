"""Benchmark one rclcpp executor against a configurable callback load.

A load generator publishes `ExecutorProbe` messages onto `num_subscriptions`
topics. Every one of those topics is subscribed by a bench node loaded into a
component container whose executor is the thing under test, so the only work
the executor does is dispatching the callbacks being measured.

```
  load_generator                  executor_container (--executor-type X)
  +--------------+                +-----------------------------------+
  |  publish     | /probe/topic_0 |  bench_node_0                     |
  |  timer       |--------------->|    subscription 0 -> callback     |
  |              | /probe/topic_1 |    subscription 1 -> callback     |
  |              |--------------->|    ...                            |
  |              |      ...       |    timer 0..M     -> callback     |
  +--------------+                +-----------------------------------+
```

Launch arguments (all overridable from the CLI):
    executor                   single_threaded | multi_threaded | events_cbg | events
    num_threads                worker threads (0 = hw concurrency); single-threaded ignores it
    num_nodes                  how many bench nodes the entities are spread over
    num_subscriptions          total subscriptions across all bench nodes
    num_timers                 total timers across all bench nodes
    timer_period_ms            period of every bench timer
    publish_rate_hz            per-topic publish rate
    publish_mode               burst (all topics per tick) | round_robin (one per tick)
                               | multi_rate (per-topic timer, rates spread over the range)
    rate_min_hz                lowest per-topic rate, multi_rate only
    rate_max_hz                highest per-topic rate, multi_rate only
    payload_bytes              size of the probe payload field
    callback_work_us           busy-wait performed inside every callback
    callback_group             default | mutually_exclusive | reentrant | per_entity
    generator_mode             process (own process) | composed (into the container)
    use_intra_process_comms    rclcpp zero-copy IPC; only meaningful with generator_mode:=composed
    qos_depth                  KEEP_LAST depth on the probe topics
    warmup_s                   delay before the generator starts publishing
    duration_s                 seconds of publishing before shutdown
    run_id                     unique id for this run (default: timestamp)
    output_dir                 directory (inside the container) to write CSVs into
"""

import datetime as _dt
import math

from launch import LaunchDescription
from launch.actions import (
    DeclareLaunchArgument,
    LogInfo,
    OpaqueFunction,
    Shutdown,
    TimerAction,
)
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import ComposableNodeContainer, Node
from launch_ros.descriptions import ComposableNode

TOPIC_PREFIX = "/probe/topic_"

# Keys accepted here; executor_factory.cpp is the authority on what is
# actually compiled in and will list the real set if one is missing.
KNOWN_EXECUTORS = (
    "single_threaded",
    "multi_threaded",
    "events_cbg",
    "events",
    "static_single_threaded",
    "callback_isolated",
)

# Executors driven by a component container we do not own. Keyed to the
# (package, executable) providing it; everything else runs in our own
# executor_container, which picks the rclcpp executor via --executor-type.
EXTERNAL_CONTAINERS = {
    "callback_isolated": ("callback_isolated_executor",
                          "component_container_callback_isolated"),
}

CALLBACK_GROUPS = ("default", "mutually_exclusive", "reentrant", "per_entity")


def _parse_bool(s: str) -> bool:
    return s.strip().lower() in ("1", "true", "yes", "on")


def _split_evenly(total: int, buckets: int):
    """Spread `total` entities over `buckets` nodes, remainder to the front."""
    base, rem = divmod(total, buckets)
    return [base + (1 if i < rem else 0) for i in range(buckets)]


def _build(context, *args, **kwargs):
    def cfg(name):
        return LaunchConfiguration(name).perform(context)

    executor          = cfg("executor").replace("-", "_")
    num_threads       = int(cfg("num_threads"))
    num_nodes         = int(cfg("num_nodes"))
    num_subscriptions = int(cfg("num_subscriptions"))
    num_timers        = int(cfg("num_timers"))
    timer_period_ms   = float(cfg("timer_period_ms"))
    publish_rate_hz   = float(cfg("publish_rate_hz"))
    publish_mode      = cfg("publish_mode")
    rate_min_hz       = float(cfg("rate_min_hz"))
    rate_max_hz       = float(cfg("rate_max_hz"))
    payload_bytes     = int(cfg("payload_bytes"))
    callback_work_us  = float(cfg("callback_work_us"))
    callback_group    = cfg("callback_group")
    generator_mode    = cfg("generator_mode")
    use_ipc           = _parse_bool(cfg("use_intra_process_comms"))
    qos_depth         = int(cfg("qos_depth"))
    warmup_s          = float(cfg("warmup_s"))
    duration_s        = float(cfg("duration_s"))
    run_id            = cfg("run_id")
    output_dir        = cfg("output_dir")

    if executor not in KNOWN_EXECUTORS:
        raise RuntimeError(
            f"Unsupported executor '{executor}'. Supported: {sorted(KNOWN_EXECUTORS)}"
        )
    if callback_group not in CALLBACK_GROUPS:
        raise RuntimeError(
            f"Unsupported callback_group '{callback_group}'. "
            f"Supported: {sorted(CALLBACK_GROUPS)}"
        )
    if num_nodes < 1:
        raise RuntimeError("num_nodes must be >= 1")
    if num_subscriptions < 1 and num_timers < 1:
        raise RuntimeError("need at least one subscription or timer to measure")
    if publish_mode not in ("burst", "round_robin", "multi_rate"):
        raise RuntimeError(
            f"Unsupported publish_mode '{publish_mode}'. "
            "Supported: ['burst', 'multi_rate', 'round_robin']")
    if publish_mode == "multi_rate" and rate_min_hz > rate_max_hz:
        raise RuntimeError("rate_min_hz must not exceed rate_max_hz")
    if generator_mode not in ("process", "composed"):
        raise RuntimeError("generator_mode must be 'process' or 'composed'")
    if use_ipc and generator_mode != "composed":
        raise RuntimeError(
            "use_intra_process_comms:=true only does anything with "
            "generator_mode:=composed — otherwise the publisher is in another "
            "process and the messages go through the middleware regardless"
        )

    if not run_id:
        run_id = _dt.datetime.now().strftime("run_%Y%m%d_%H%M%S")

    # Per-entity sample buffers are preallocated so recording never allocates
    # on the hot path. Size them for the whole run plus headroom for the
    # overshoot between the shutdown timer firing and spin() returning.
    # multi_rate gives each topic its own frequency, so the busiest entity —
    # not the nominal rate — sets how big the buffers have to be.
    peak_rate_hz = max(publish_rate_hz, rate_max_hz) if publish_mode == "multi_rate" \
        else publish_rate_hz
    ticks = peak_rate_hz * (duration_s + warmup_s)
    if num_timers > 0 and timer_period_ms > 0:
        ticks = max(ticks, (duration_s + warmup_s) * 1000.0 / timer_period_ms)
    sample_capacity = int(math.ceil(ticks * 1.2)) + 1000

    sub_split   = _split_evenly(num_subscriptions, num_nodes)
    timer_split = _split_evenly(num_timers, num_nodes)

    chain_extra = [{"use_intra_process_comms": use_ipc}]

    nodes = []
    topic_cursor = 0
    timer_cursor = 0
    for i in range(num_nodes):
        nodes.append(ComposableNode(
            package="executor_comparison",
            plugin="executor_comparison::BenchNode",
            name=f"bench_node_{i}",
            parameters=[{
                "node_index": i,
                "topic_prefix": TOPIC_PREFIX,
                "first_topic_index": topic_cursor,
                "num_subscriptions": sub_split[i],
                "first_timer_index": timer_cursor,
                "num_timers": timer_split[i],
                "timer_period_ms": timer_period_ms,
                "callback_work_us": callback_work_us,
                "callback_group": callback_group,
                "qos_depth": qos_depth,
                "sample_capacity": sample_capacity,
            }],
            extra_arguments=chain_extra,
        ))
        topic_cursor += sub_split[i]
        timer_cursor += timer_split[i]

    generator_params = {
        "run_id": run_id,
        "topic_prefix": TOPIC_PREFIX,
        "num_topics": num_subscriptions,
        "first_topic_index": 0,
        "publish_rate_hz": publish_rate_hz,
        "publish_mode": publish_mode,
        "rate_min_hz": rate_min_hz,
        "rate_max_hz": rate_max_hz,
        "payload_bytes": payload_bytes,
        "qos_depth": qos_depth,
        "warmup_s": warmup_s,
    }

    extra_actions = []
    if num_subscriptions > 0:
        if generator_mode == "composed":
            nodes.append(ComposableNode(
                package="executor_comparison",
                plugin="executor_comparison::LoadGenerator",
                name="load_generator",
                parameters=[generator_params],
                extra_arguments=chain_extra,
            ))
        else:
            extra_actions.append(Node(
                package="executor_comparison",
                executable="load_generator",
                name="load_generator",
                parameters=[generator_params],
                output="screen",
                emulate_tty=True,
            ))

    # The writer owns the CSVs, so it carries the whole run config for the
    # metadata header. It lives in the container rather than in the container
    # executable so that the external containers work too.
    nodes.append(ComposableNode(
        package="executor_comparison",
        plugin="executor_comparison::MetricsWriter",
        name="metrics_writer",
        parameters=[{
            "run_id": run_id,
            "executor": executor,
            "num_threads": num_threads,
            "num_nodes": num_nodes,
            "num_subscriptions": num_subscriptions,
            "num_timers": num_timers,
            "timer_period_ms": timer_period_ms,
            "publish_rate_hz": publish_rate_hz,
            "publish_mode": publish_mode,
            "rate_min_hz": rate_min_hz,
            "rate_max_hz": rate_max_hz,
            "payload_bytes": payload_bytes,
            "callback_work_us": callback_work_us,
            "callback_group": callback_group,
            "use_intra_process_comms": use_ipc,
            "generator_mode": generator_mode,
            "qos_depth": qos_depth,
            "duration_s": duration_s,
            "output_dir": output_dir,
        }],
    ))

    if executor in EXTERNAL_CONTAINERS:
        pkg, exe = EXTERNAL_CONTAINERS[executor]
        # This container derives its thread count from the callback groups, so
        # num_threads only has a meaning for reentrant groups, where it sets
        # how many threads that one group gets.
        container_args = []
        container_params = [{"reentrant_parallelism": max(num_threads, 1)}]
    else:
        pkg, exe = "executor_comparison", "executor_container"
        container_args = ["--executor-type", executor]
        container_params = [{"num_threads": num_threads}]

    container = ComposableNodeContainer(
        name="executor_container",
        namespace="",
        package=pkg,
        executable=exe,
        arguments=container_args,
        composable_node_descriptions=nodes,
        parameters=container_params,
        output="screen",
        emulate_tty=True,
    )

    shutdown = TimerAction(
        period=duration_s + warmup_s,
        actions=[
            LogInfo(msg="[executor_bench] duration elapsed, shutting down"),
            Shutdown(reason="run duration complete"),
        ],
    )

    return [
        LogInfo(msg=(
            f"[executor_bench] executor={executor} threads={num_threads} "
            f"nodes={num_nodes} subs={num_subscriptions} timers={num_timers} "
            f"rate={publish_rate_hz}Hz mode={publish_mode} "
            + (f"spread={rate_min_hz}-{rate_max_hz}Hz " if publish_mode == "multi_rate" else "")
            + f"payload={payload_bytes}B "
            f"work={callback_work_us}us cbg={callback_group} ipc={use_ipc} "
            f"generator={generator_mode} run_id={run_id}"
        )),
        container,
        *extra_actions,
        shutdown,
    ]


def generate_launch_description():
    return LaunchDescription([
        DeclareLaunchArgument("executor",                default_value="single_threaded"),
        DeclareLaunchArgument("num_threads",             default_value="0"),
        DeclareLaunchArgument("num_nodes",               default_value="1"),
        DeclareLaunchArgument("num_subscriptions",       default_value="10"),
        DeclareLaunchArgument("num_timers",              default_value="0"),
        DeclareLaunchArgument("timer_period_ms",         default_value="10.0"),
        DeclareLaunchArgument("publish_rate_hz",         default_value="100.0"),
        DeclareLaunchArgument("publish_mode",            default_value="burst"),
        DeclareLaunchArgument("rate_min_hz",             default_value="63.0"),
        DeclareLaunchArgument("rate_max_hz",             default_value="2000.0"),
        DeclareLaunchArgument("payload_bytes",           default_value="1024"),
        DeclareLaunchArgument("callback_work_us",        default_value="0.0"),
        DeclareLaunchArgument("callback_group",          default_value="mutually_exclusive"),
        DeclareLaunchArgument("generator_mode",          default_value="process"),
        DeclareLaunchArgument("use_intra_process_comms", default_value="false"),
        DeclareLaunchArgument("qos_depth",               default_value="10"),
        DeclareLaunchArgument("warmup_s",                default_value="5.0"),
        DeclareLaunchArgument("duration_s",              default_value="60.0"),
        DeclareLaunchArgument("run_id",                  default_value=""),
        DeclareLaunchArgument("output_dir",              default_value="/data/results"),
        OpaqueFunction(function=_build),
    ])
