# Probe message fired at the executor under test by the load generator.
#
# Latency is measured as (callback entry - t_published_ns), so the stamp that
# matters is t_published_ns, captured from CLOCK_MONOTONIC immediately before
# publish(). CLOCK_MONOTONIC is system wide on Linux, so the value stays
# comparable when the generator runs in its own process.

std_msgs/Header header       # conventional stamp, unused by the benchmark maths
uint64  seq                  # per-topic monotonic sequence number, for drop detection
uint32  topic_index          # which fan-out topic this went out on
int64   t_published_ns       # CLOCK_MONOTONIC ns, captured just before publish()
uint8[] payload              # padded to the run's payload_bytes
