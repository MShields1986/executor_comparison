# executor_comparison

Performance comparison of the callback executors available in ROS 2 Lyrical Luth.

A load generator publishes stamped probe messages onto `N` topics, subscribed by
bench nodes inside a composable container whose executor is the thing under
test. Each callback records when the executor got round to it, how long it ran
and on which thread; a writer component emits per-callback + summary CSVs to
`data/results/`.

```mermaid
flowchart LR
    G([load_generator<br/>own process]):::gen
    subgraph C["executor_container — executor under test"]
        direction TB
        S0([subscription 0]):::sub
        S1([subscription 1]):::sub
        SN([... subscription N-1]):::sub
        T0([timer 0 .. M-1]):::tmr
    end
    W[[CSV + summary<br/>written on shutdown]]:::col

    G -- "/probe/topic_0" --> S0
    G -- "/probe/topic_1" --> S1
    G -- "/probe/topic_N-1" --> SN

    S0 -. "wait-free sample buffer" .-> W
    S1 -.-> W
    SN -.-> W
    T0 -.-> W

    classDef gen fill:#cfe8ff,stroke:#1f6feb,color:#0a2540;
    classDef sub fill:#d4f5d4,stroke:#1a7f1a,color:#0b3f0b;
    classDef tmr fill:#fff3b0,stroke:#a07f00,color:#402c00;
    classDef col fill:#eeeeee,stroke:#555,color:#222;
```

Recording is wait-free — callbacks bump an atomic index into a preallocated
buffer, with no allocation, no locking and no ROS traffic on the hot path.
Publishing a record per callback (as
[ros_latency_tests](https://github.com/MShields1986/ros_latency_tests) does)
would push the executor through an extra publish and measure that instead.

## Results

50 topics at 63–2000 Hz, 1 MB messages, single process, 20 s, CycloneDDS.
Matches the topology in the [Polymath write-up][polymath].

<img src="https://github.com/MShields1986/executor_comparison/blob/main/img/executors_50topics_1MB_lyrical.png" width=80% height=80%>

| executor | threads | ipc | throughput | mean latency | CPU |
|---|---|---|---|---|---|
| **events_cbg** | 1 | on | **100.0%** | ~0 ms | 64% |
| events_cbg | 8 | on | 97.8% | ~0 ms | 110% |
| single_threaded | 8 | on | 43.6% | 0.79 ms | 80% |
| single_threaded | 1 | on | 40.3% | 0.85 ms | 80% |
| multi_threaded | 1 | on | 37.4% | 0.94 ms | 80% |
| multi_threaded | 8 | on | 12.1% | 0.18 ms | 98% |
| events | 1 | on | 8.0% | 31.94 ms | 95% |
| events_cbg | 1 | off | 34.2% | 0.04 ms | 84% |
| events_cbg | 8 | off | 24.7% | 24.69 ms | 163% |
| single_threaded | 8 | off | 13.5% | 10.57 ms | 80% |
| single_threaded | 1 | off | 13.0% | 10.97 ms | 80% |
| multi_threaded | 1 | off | 12.9% | 10.99 ms | 80% |
| events | 1 | off | 4.5% | 42.90 ms | 96% |
| multi_threaded | 8 | off | 4.0% | 93.02 ms | 108% |

- `events_cbg` at one thread is the only configuration that keeps up, and it
  does so at the *lowest* CPU of the set.
- Eight threads makes everything worse except `single_threaded`, which ignores
  the setting. `multi_threaded` drops 12.9% → 4.0% and mean latency goes
  11 ms → 93 ms: contention, not parallelism.
- The experimental `events` is the worst of the four. `events_cbg` is not an
  incremental improvement on it.

An unsaturated sweep tells you much less — below roughly 210 µs everything ties
on the middleware floor, and with one shared callback group `callback_isolated`
and `events` are indistinguishable.

## Quick start

```shell
git clone https://github.com/MShields1986/executor_comparison.git
cd executor_comparison

./run_single.sh events_cbg                   # one executor, no plot
./run_all.sh                                 # full matrix, then plot
./run_all.sh "single_threaded events_cbg"    # override the list
```

Results land in `data/results/` as
`executor_<exec>_<T>t_<N>n_<S>s_<M>tm_<payload>B_<rate>hz_<work>us_<cbg>_<ipc|noipc>_<rmw>_<ts>.csv`,
each with a `#`-prefixed metadata header (run config, resource usage, host, CPU,
OS, kernel). `run_all.sh` also emits an `executors_*.png`.

To reproduce the table above:

```shell
MATRIX_EXECUTORS="single_threaded multi_threaded events events_cbg" \
MATRIX_THREADS="1 8" MATRIX_SUBS="50" MATRIX_MODES="multi_rate" \
MATRIX_RATE_MIN=63.0 MATRIX_RATE_MAX=2000.0 \
MATRIX_PAYLOADS="1048576" MATRIX_GENERATOR="composed" \
MATRIX_IPCS="false true" MATRIX_DURATION=20.0 \
MATRIX_PLOT_GROUPBY="threads_ipc" ./run_all.sh
```

[polymath]: https://www.polymathrobotics.com/blog/execution-management-in-rclcpp

## Metrics

| metric | meaning |
|---|---|
| `latency_ns` (subscription) | callback entry minus the publisher's pre-publish stamp — dispatch latency |
| `latency_ns` (timer) | actual interval minus nominal period; first tick records 0 |
| `exec_ns` | time spent inside the callback |
| `thread_slot` | which executor thread ran it — shows whether the callback groups bought any parallelism |
| `# throughput_pct` | callbacks serviced over what the rate should have delivered |
| `# latency_mean_ns` | mean dispatch latency |
| `# cpu_percent_mean` | process CPU, sampled at 10 Hz |
| `# voluntary_ctx_switches` | summed across all threads, not just the main one |
| `# samples_dropped` | gaps in the per-topic sequence numbers |
| `# samples_overflowed` | samples lost to a full recording buffer (should be 0) |

`throughput_pct` and `samples_dropped` catch different failures: under reliable
back-pressure the publisher never emits, so sequence numbers stay contiguous and
only throughput sees it. Throughput is measured against the observed publish
window rather than `duration_s`, which the run overruns by a couple of percent.

Timestamps come from `CLOCK_MONOTONIC`, which is system-wide on Linux, so the
generator's stamps stay comparable across a process boundary.

## Knobs

```shell
docker compose -f docker/docker-compose.yaml run --rm events_cbg \
  ros2 launch executor_comparison executor_bench.launch.py \
    executor:=events_cbg num_subscriptions:=100 num_threads:=4 \
    callback_group:=reentrant callback_work_us:=500 duration_s:=30.0
```

| launch arg | default | notes |
|---|---|---|
| `executor` | `single_threaded` | see below |
| `num_threads` | `0` | 0 = hw concurrency; ignored by single-threaded executors; means `reentrant_parallelism` for `callback_isolated` |
| `num_nodes` | `1` | bench nodes to spread the entities over |
| `num_subscriptions` | `10` | total across all bench nodes |
| `num_timers` | `0` | total across all bench nodes |
| `timer_period_ms` | `10.0` | |
| `publish_rate_hz` | `100.0` | per topic |
| `publish_mode` | `burst` | `burst` = all topics per tick; `round_robin` = one per tick; `multi_rate` = per-topic timer, rates spread over `rate_min_hz`..`rate_max_hz` |
| `rate_min_hz` / `rate_max_hz` | `63.0` / `2000.0` | `multi_rate` only |
| `payload_bytes` | `1024` | size of the probe payload field |
| `callback_work_us` | `0.0` | busy-wait inside every callback |
| `callback_group` | `mutually_exclusive` | see below |
| `generator_mode` | `process` | or `composed`, into the container under test |
| `use_intra_process_comms` | `false` | requires `generator_mode:=composed` |
| `qos_depth` | `10` | KEEP_LAST depth, reliable |
| `warmup_s` / `duration_s` | `5.0` / `60.0` | |
| `output_dir` | `/data/results` | inside the container |

`run_all.sh` sweeps the cartesian product of `MATRIX_EXECUTORS`, `MATRIX_SUBS`,
`MATRIX_TIMERS`, `MATRIX_NODES`, `MATRIX_THREADS`, `MATRIX_CBGS`,
`MATRIX_WORK_US`, `MATRIX_PAYLOADS`, `MATRIX_MODES` and `MATRIX_IPCS`, with
`MATRIX_RATE`, `MATRIX_DURATION`, `MATRIX_WARMUP`, `MATRIX_GENERATOR`,
`MATRIX_RATE_MIN`, `MATRIX_RATE_MAX` and `MATRIX_PLOT_GROUPBY` as scalars.

## Executors

| key | class | threads | |
|---|---|---|---|
| `single_threaded` | `rclcpp::executors::SingleThreadedExecutor` | 1 | baseline |
| `multi_threaded` | `rclcpp::executors::MultiThreadedExecutor` | N | parallelism bounded by callback groups |
| `events_cbg` | `rclcpp::executors::EventsCBGExecutor` | N | new in Lyrical; events queue, not a wait set |
| `events` | `rclcpp::experimental::executors::EventsExecutor` | 1 | experimental, predates `events_cbg` |
| `callback_isolated` | [`CallbackIsolatedExecutor`][cie] (TIER IV / Autoware) | one per callback group | see below |
| `static_single_threaded` | `rclcpp::executors::StaticSingleThreadedExecutor` | 1 | removed in Lyrical; only built against older distros |

[cie]: https://github.com/autowarefoundation/callback_isolated_executor

The set is probed with `__has_include`, so the same source builds either side of
the `StaticSingleThreadedExecutor` removal and the `EventsCBGExecutor` addition.
Hyphenated spellings (`events-cbg`) are accepted too.

### `callback_isolated`

Gives every callback group its own thread, so the OS scheduler becomes the
arbiter — you can set policy, priority and affinity per group via its companion
`cie_thread_configurator`. Three differences:

- **Not selectable via `--executor-type`.** Its `spin()` snapshots callback
  groups once, so composable nodes loaded after spin are never picked up.
  Upstream ships `component_container_callback_isolated` instead, which the
  launch file selects automatically.
- **`num_threads` means `reentrant_parallelism`.** With
  `callback_group:=per_entity`, `num_subscriptions:=100` means 100 threads.
- **Needs a patch on Lyrical.** Cloned at a pinned commit during the Docker
  build and patched by `patches/0001-lyrical-port.patch` — see
  [`patches/README.md`](patches/README.md). Nothing behavioural is touched.

## Callback groups

| value | behaviour |
|---|---|
| `default` | the node's default (mutually exclusive) group |
| `mutually_exclusive` | one shared group — nothing runs in parallel |
| `reentrant` | one shared group — anything may run in parallel, including with itself |
| `per_entity` | one mutually exclusive group each — entities parallel with each other, never with themselves |

`multi_threaded` with `mutually_exclusive` serialises everything and performs
like `single_threaded` while still paying for the threads. `per_entity` is where
the multi-threaded executors earn their keep.

## Notes

- All three of `rmw_cyclonedds_cpp`, `rmw_fastrtps_cpp` and
  `rmw_fastrtps_dynamic_cpp` are in the one image, selected at run time via
  `RMW_IMPLEMENTATION` (default CycloneDDS) and recorded in every CSV. Every
  executor service therefore shares a single build.
- At payloads above ~128 KB the generator raises glibc's `mmap` threshold.
  Without it every message body is a fresh `mmap`/`munmap` plus page faults,
  which measures the allocator rather than the executor — worth 4.4% vs 41.9%
  throughput on the 1 MB runs.
- CPU is process-wide, so it includes the component manager, and the generator
  too under `generator_mode:=composed`. Only compare within one mode.
- Sample buffers are preallocated from the peak rate and duration. A non-zero
  `# samples_overflowed` means the run outgrew them.
