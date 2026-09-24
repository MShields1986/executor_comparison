#include "executor_comparison/run_metadata.hpp"

#include "executor_comparison/load_profile.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <thread>

#include <sys/utsname.h>
#include <unistd.h>

#include "rmw/rmw.h"

namespace executor_comparison
{

namespace
{

std::string env_or(const char * name, const char * fallback)
{
    const char * v = std::getenv(name);
    return v ? std::string(v) : std::string(fallback);
}

std::string timestamp_now()
{
    const auto now = std::chrono::system_clock::now();
    const auto t   = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return ss.str();
}

// First "key: value" match in a /proc-style file.
std::string read_first(const std::string & path, const std::string & key)
{
    std::ifstream f(path);
    if (!f) return "unknown";
    std::string line;
    while (std::getline(f, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = line.substr(0, colon);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        if (k != key) continue;
        std::string v = line.substr(colon + 1);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
        return v;
    }
    return "unknown";
}

std::string read_os_pretty_name()
{
    std::ifstream f("/etc/os-release");
    if (!f) return "unknown";
    std::string line;
    const std::string key = "PRETTY_NAME=";
    while (std::getline(f, line)) {
        if (line.rfind(key, 0) != 0) continue;
        std::string v = line.substr(key.size());
        if (!v.empty() && v.front() == '"') v.erase(v.begin());
        if (!v.empty() && v.back()  == '"') v.pop_back();
        return v;
    }
    return "unknown";
}

std::string host_name()
{
    char buf[256] = {0};
    if (gethostname(buf, sizeof(buf) - 1) == 0) return std::string(buf);
    return "unknown";
}

std::string sanitise(const std::string & s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back((c == '/' || c == ':' || c == ' ') ? '_' : c);
    }
    return out;
}

double percentile(std::vector<int64_t> & sorted, double p)
{
    if (sorted.empty()) return 0.0;
    const double pos = p * static_cast<double>(sorted.size() - 1);
    const auto idx = static_cast<std::size_t>(std::llround(pos));
    return static_cast<double>(sorted[std::min(idx, sorted.size() - 1)]);
}

double mean_of(const std::vector<int64_t> & v)
{
    if (v.empty()) return 0.0;
    long double s = 0.0L;
    for (auto x : v) s += static_cast<long double>(x);
    return static_cast<double>(s / static_cast<long double>(v.size()));
}

double stddev_of(const std::vector<int64_t> & v, double m)
{
    if (v.size() < 2) return 0.0;
    long double s = 0.0L;
    for (auto x : v) {
        const long double d = static_cast<long double>(x) - static_cast<long double>(m);
        s += d * d;
    }
    return static_cast<double>(std::sqrt(static_cast<double>(s / (v.size() - 1))));
}

}  // namespace


void collect_host_info(RunMetadata & meta)
{
    struct utsname uts{};
    (void)uname(&uts);

    // Ask the middleware directly: RMW_IMPLEMENTATION is often unset even
    // when a non-default RMW is in use.
    const char * rmw_id = rmw_get_implementation_identifier();
    meta.rmw        = rmw_id ? std::string(rmw_id) : env_or("RMW_IMPLEMENTATION", "unknown_rmw");
    meta.ros_distro = env_or("ROS_DISTRO", "?");
    meta.host       = host_name();
    meta.cpu_model  = read_first("/proc/cpuinfo", "model name");
    meta.cpu_cores  = static_cast<int>(std::thread::hardware_concurrency());
    meta.os_pretty  = read_os_pretty_name();
    meta.kernel     = std::string(uts.sysname) + " " + uts.release + " " + uts.machine;
    meta.start_time = timestamp_now();
}

std::string build_filename_stem(const RunMetadata & meta, const std::string & output_dir)
{
    std::ostringstream ss;
    ss << output_dir << "/executor_"
       << sanitise(meta.executor) << "_"
       << meta.num_threads        << "t_"
       << meta.num_nodes          << "n_"
       << meta.num_subscriptions  << "s_"
       << meta.num_timers         << "tm_"
       << meta.payload_bytes      << "B_"
       << static_cast<int>(meta.publish_rate_hz) << "hz_"
       << static_cast<int>(meta.callback_work_us) << "us_"
       << sanitise(meta.callback_group) << "_"
       << (meta.use_intra_process_comms ? "ipc_" : "noipc_")
       << sanitise(meta.rmw) << "_"
       << timestamp_now();
    return ss.str();
}

std::string write_results(
    const RunMetadata &                                    meta,
    const std::string &                                    output_dir,
    const std::vector<std::shared_ptr<CallbackRecorder>> & recorders,
    const ResourceSampler::Stats &                         resources)
{
    std::error_code ec;
    std::filesystem::create_directories(output_dir, ec);

    const std::string stem = build_filename_stem(meta, output_dir);

    // Walk the samples once up front: the drop/overflow counts belong in the
    // metadata header, which has to be written before the rows.
    std::size_t total_samples = 0;
    std::size_t overflowed    = 0;
    int64_t     dropped       = 0;

    // Throughput, in the sense the ros2-performance derived benchmarks use:
    // callbacks actually serviced over what the configured rate should have
    // delivered. Expected comes from the rate rather than from the observed
    // sequence range, because under back-pressure a reliable publisher can
    // block and never emit the messages a range would imply.
    // Measured against the window the generator was actually publishing over,
    // not the nominal duration_s: the run overruns it slightly (launch shutdown
    // is not instant), which otherwise shows up as a steady ~102% throughput.
    // Taken globally rather than per topic so that a topic being starved does
    // not shrink its own denominator and hide the starvation.
    int64_t window_lo = std::numeric_limits<int64_t>::max();
    int64_t window_hi = std::numeric_limits<int64_t>::min();
    for (const auto & rec : recorders) {
        if (rec->type() != EntityType::Subscription) continue;
        const std::size_t n = rec->recorded();
        if (n == 0) continue;
        window_lo = std::min(window_lo, rec->buffer()[0].t_reference_ns);
        window_hi = std::max(window_hi, rec->buffer()[n - 1].t_reference_ns);
    }
    const double window_s = (window_hi > window_lo)
        ? static_cast<double>(window_hi - window_lo) / 1e9
        : meta.duration_s;

    const auto expected_for = [&](uint32_t topic_index) {
        const double hz = topic_rate_hz(
            topic_index, static_cast<std::size_t>(std::max(meta.num_subscriptions, 1)),
            meta.publish_mode, meta.publish_rate_hz, meta.rate_min_hz, meta.rate_max_hz);
        return hz * window_s + 1.0;  // +1: n ticks span (n-1) periods
    };
    double total_expected = 0.0;
    double total_received = 0.0;

    for (const auto & rec : recorders) {
        const std::size_t n = rec->recorded();
        total_samples += n;
        overflowed    += rec->overflowed();
        if (rec->type() != EntityType::Subscription) continue;
        total_expected += expected_for(rec->index());
        total_received += static_cast<double>(n);
        if (n == 0) continue;

        // Sequence numbers are per-topic and monotonic, so anything missing
        // between the first and last one we saw was dropped somewhere between
        // the publisher and the callback.
        uint64_t lo = std::numeric_limits<uint64_t>::max();
        uint64_t hi = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const uint64_t s = rec->buffer()[i].seq;
            lo = std::min(lo, s);
            hi = std::max(hi, s);
        }
        dropped += static_cast<int64_t>(hi - lo + 1) - static_cast<int64_t>(n);
    }

    const double throughput_pct = (total_expected > 0.0)
        ? 100.0 * total_received / total_expected
        : 0.0;

    // Mean dispatch latency over every subscription callback. The reference
    // benchmarks report a mean rather than percentiles, so it belongs in the
    // header where the plotter can reach it without loading the sample rows.
    long double latency_sum = 0.0L;
    std::size_t latency_n   = 0;
    for (const auto & rec : recorders) {
        if (rec->type() != EntityType::Subscription) continue;
        const std::size_t n = rec->recorded();
        for (std::size_t i = 0; i < n; ++i) {
            const Sample & s = rec->buffer()[i];
            latency_sum += static_cast<long double>(s.t_start_ns - s.t_reference_ns);
            ++latency_n;
        }
    }
    const double latency_mean_ns = latency_n
        ? static_cast<double>(latency_sum / static_cast<long double>(latency_n))
        : 0.0;

    auto write_meta = [&](std::ofstream & f) {
        f << "# run_id="                  << meta.run_id                 << "\n"
          << "# executor="                << meta.executor               << "\n"
          << "# num_threads="             << meta.num_threads            << "\n"
          << "# num_nodes="               << meta.num_nodes              << "\n"
          << "# num_subscriptions="       << meta.num_subscriptions      << "\n"
          << "# num_timers="              << meta.num_timers             << "\n"
          << "# timer_period_ms="         << meta.timer_period_ms        << "\n"
          << "# publish_rate_hz="         << meta.publish_rate_hz        << "\n"
          << "# publish_mode="            << meta.publish_mode           << "\n"
          << "# payload_bytes="           << meta.payload_bytes          << "\n"
          << "# callback_work_us="        << meta.callback_work_us       << "\n"
          << "# callback_group="          << meta.callback_group         << "\n"
          << "# use_intra_process_comms=" << (meta.use_intra_process_comms ? "true" : "false") << "\n"
          << "# generator_mode="          << meta.generator_mode         << "\n"
          << "# qos_depth="               << meta.qos_depth              << "\n"
          << "# duration_s="              << meta.duration_s             << "\n"
          << "# rmw="                     << meta.rmw                    << "\n"
          << "# ros_distro="              << meta.ros_distro             << "\n"
          << "# start_time="              << meta.start_time             << "\n"
          << "# host="                    << meta.host                   << "\n"
          << "# cpu_model="               << meta.cpu_model              << "\n"
          << "# cpu_cores="               << meta.cpu_cores              << "\n"
          << "# os="                      << meta.os_pretty              << "\n"
          << "# kernel="                  << meta.kernel                 << "\n"
          << "# samples="                 << total_samples               << "\n"
          << "# samples_expected="         << static_cast<int64_t>(total_expected) << "\n"
          << "# publish_window_s="         << window_s                    << "\n"
          << "# throughput_pct="           << throughput_pct              << "\n"
          << "# latency_mean_ns="          << latency_mean_ns             << "\n"
          << "# samples_dropped="         << dropped                     << "\n"
          << "# samples_overflowed="      << overflowed                  << "\n"
          << "# cpu_percent_mean="        << resources.cpu_percent_mean  << "\n"
          << "# cpu_percent_max="         << resources.cpu_percent_max   << "\n"
          << "# cpu_s_total="             << resources.cpu_s_total       << "\n"
          << "# rss_mb_mean="             << resources.rss_mb_mean       << "\n"
          << "# rss_mb_max="              << resources.rss_mb_max        << "\n"
          << "# voluntary_ctx_switches="  << resources.voluntary_ctx_switches   << "\n"
          << "# involuntary_ctx_switches=" << resources.involuntary_ctx_switches << "\n"
          << "# sampled_wall_s="          << resources.wall_s            << "\n";
    };

    std::ofstream csv(stem + ".csv");
    std::ofstream summary(stem + "_summary.csv");
    write_meta(csv);
    write_meta(summary);

    csv << "entity_type,entity_index,seq,t_reference_ns,t_start_ns,t_end_ns,"
           "latency_ns,exec_ns,thread_slot\n";

    // Collected per entity as we stream the rows out, then reduced into the
    // summary so the samples are only walked once more.
    std::map<std::pair<std::string, uint32_t>, std::vector<int64_t>> latency_by_entity;
    std::vector<int64_t> sub_latency, sub_exec, timer_latency, timer_exec;

    for (const auto & rec : recorders) {
        const char * type_name = to_string(rec->type());
        const std::size_t n = rec->recorded();
        auto & bucket = latency_by_entity[{type_name, rec->index()}];
        bucket.reserve(n);

        for (std::size_t i = 0; i < n; ++i) {
            const Sample & s = rec->buffer()[i];
            const int64_t latency = s.t_start_ns - s.t_reference_ns;
            const int64_t exec    = s.t_end_ns   - s.t_start_ns;

            csv << type_name          << ','
                << s.entity_index     << ','
                << s.seq              << ','
                << s.t_reference_ns   << ','
                << s.t_start_ns       << ','
                << s.t_end_ns         << ','
                << latency            << ','
                << exec               << ','
                << s.thread_slot      << '\n';

            bucket.push_back(latency);
            if (rec->type() == EntityType::Subscription) {
                sub_latency.push_back(latency);
                sub_exec.push_back(exec);
            } else {
                timer_latency.push_back(latency);
                timer_exec.push_back(exec);
            }
        }
    }

    summary << "metric,entity_type,entity_index,count,min_ns,mean_ns,p50_ns,"
               "p90_ns,p95_ns,p99_ns,max_ns,stddev_ns,expected,throughput_pct\n";

    // expected/throughput only mean anything for subscription latency rows;
    // left blank elsewhere so the table stays one shape.
    auto dump = [&](const std::string & metric, const std::string & type_name,
                    const std::string & index, std::vector<int64_t> v,
                    double expected = -1.0) {
        if (v.empty()) return;
        std::sort(v.begin(), v.end());
        const double m  = mean_of(v);
        const double sd = stddev_of(v, m);
        summary << metric << ',' << type_name << ',' << index << ','
                << v.size()             << ','
                << v.front()            << ','
                << m                    << ','
                << percentile(v, 0.50)  << ','
                << percentile(v, 0.90)  << ','
                << percentile(v, 0.95)  << ','
                << percentile(v, 0.99)  << ','
                << v.back()             << ','
                << sd                   << ',';
        if (expected >= 0.0) {
            summary << static_cast<int64_t>(expected) << ','
                    << (expected > 0.0 ? 100.0 * static_cast<double>(v.size()) / expected : 0.0);
        } else {
            summary << ',';
        }
        summary << '\n';
    };

    for (auto & [key, samples] : latency_by_entity) {
        const double expected = (key.first == "subscription")
            ? expected_for(key.second) : -1.0;
        dump("latency", key.first, std::to_string(key.second), std::move(samples), expected);
    }
    dump("latency", "subscription", "all", sub_latency, total_expected);
    dump("exec",    "subscription", "all", sub_exec);
    dump("latency", "timer",        "all", timer_latency);
    dump("exec",    "timer",        "all", timer_exec);

    csv.close();
    summary.close();
    return stem;
}

}  // namespace executor_comparison
