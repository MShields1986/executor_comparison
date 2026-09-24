#include "executor_comparison/resource_sampler.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <dirent.h>
#include <unistd.h>

#include "executor_comparison/callback_recorder.hpp"  // monotonic_ns()

namespace executor_comparison
{

namespace
{

// utime + stime in clock ticks. /proc/self/stat's comm field can contain
// spaces and parentheses, so parsing has to start after the *last* ')'.
bool read_cpu_ticks(uint64_t & utime, uint64_t & stime)
{
    std::ifstream f("/proc/self/stat");
    if (!f) return false;
    std::string line;
    std::getline(f, line);

    const auto close = line.rfind(')');
    if (close == std::string::npos) return false;

    std::istringstream ss(line.substr(close + 1));
    std::vector<std::string> fields;
    std::string tok;
    while (ss >> tok) fields.push_back(tok);

    // fields[0] is `state` (stat field 3), so utime (14) and stime (15) sit
    // at offsets 11 and 12.
    if (fields.size() < 13) return false;
    try {
        utime = std::stoull(fields[11]);
        stime = std::stoull(fields[12]);
    } catch (...) {
        return false;
    }
    return true;
}

double read_rss_mb()
{
    std::ifstream f("/proc/self/statm");
    if (!f) return 0.0;
    unsigned long size = 0, resident = 0;
    f >> size >> resident;
    const long page = sysconf(_SC_PAGESIZE);
    return static_cast<double>(resident) * static_cast<double>(page) / (1024.0 * 1024.0);
}

// Context-switch counters live in each thread's own status file;
// /proc/self/status describes the *main thread* only, not the process. An
// executor that does its work on spawned threads — callback_isolated gives
// every callback group its own — would otherwise report almost zero, since
// its main thread just parks in the component manager. Sum across
// /proc/self/task/<tid>/status instead.
void read_ctx_switches(uint64_t & voluntary, uint64_t & involuntary)
{
    static const std::string kVol   = "voluntary_ctxt_switches:";
    static const std::string kInvol = "nonvoluntary_ctxt_switches:";

    voluntary = 0;
    involuntary = 0;

    DIR * dir = opendir("/proc/self/task");
    if (!dir) return;

    while (const dirent * ent = readdir(dir)) {
        if (ent->d_name[0] == '.') continue;
        std::ifstream f(std::string("/proc/self/task/") + ent->d_name + "/status");
        if (!f) continue;  // thread exited between readdir and open

        std::string line;
        while (std::getline(f, line)) {
            // "nonvoluntary_..." does not share a prefix with "voluntary_...",
            // so the order of these tests does not matter.
            if (line.rfind(kVol, 0) == 0) {
                voluntary += std::strtoull(line.c_str() + kVol.size(), nullptr, 10);
            } else if (line.rfind(kInvol, 0) == 0) {
                involuntary += std::strtoull(line.c_str() + kInvol.size(), nullptr, 10);
            }
        }
    }
    closedir(dir);
}

}  // namespace

ResourceSampler::ResourceSampler(double sample_hz)
: sample_hz_(sample_hz > 0.0 ? sample_hz : 10.0)
{
}

ResourceSampler::~ResourceSampler()
{
    stop();
}

void ResourceSampler::start()
{
    if (running_.exchange(true)) return;
    thread_ = std::thread(&ResourceSampler::run, this);
}

void ResourceSampler::stop()
{
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

void ResourceSampler::run()
{
    const double ticks_per_s = static_cast<double>(sysconf(_SC_CLK_TCK));
    const auto   period      = std::chrono::duration<double>(1.0 / sample_hz_);

    uint64_t prev_u = 0, prev_s = 0;
    read_cpu_ticks(prev_u, prev_s);
    const uint64_t base_u = prev_u, base_s = prev_s;

    uint64_t base_vol = 0, base_invol = 0;
    read_ctx_switches(base_vol, base_invol);
    uint64_t last_vol = base_vol, last_invol = base_invol;

    const int64_t t0 = monotonic_ns();
    int64_t prev_t   = t0;

    double   cpu_sum = 0.0, rss_sum = 0.0;
    uint64_t n = 0;

    while (running_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(period);

        uint64_t u = 0, s = 0;
        if (!read_cpu_ticks(u, s)) continue;

        const int64_t now    = monotonic_ns();
        const double  dt_s   = static_cast<double>(now - prev_t) / 1e9;
        if (dt_s <= 0.0) continue;

        const double cpu_s  = static_cast<double>((u - prev_u) + (s - prev_s)) / ticks_per_s;
        const double cpu_pc = 100.0 * cpu_s / dt_s;
        const double rss    = read_rss_mb();

        cpu_sum += cpu_pc;
        rss_sum += rss;
        ++n;
        stats_.cpu_percent_max = std::max(stats_.cpu_percent_max, cpu_pc);
        stats_.rss_mb_max      = std::max(stats_.rss_mb_max, rss);

        // Walking every thread's status file is the expensive part of a
        // tick, and these are monotonic counters rather than averages, so
        // once a second is plenty. Sampled during the run because the worker
        // threads are joined before this sampler is stopped — reading at the
        // end would find them already gone.
        if (n % 10 == 0) {
            read_ctx_switches(last_vol, last_invol);
        }

        prev_u = u;
        prev_s = s;
        prev_t = now;
    }

    uint64_t end_u = prev_u, end_s = prev_s;
    read_cpu_ticks(end_u, end_s);

    stats_.samples          = n;
    stats_.wall_s           = static_cast<double>(monotonic_ns() - t0) / 1e9;
    stats_.cpu_s_total      = static_cast<double>((end_u - base_u) + (end_s - base_s)) / ticks_per_s;
    stats_.cpu_percent_mean = n ? cpu_sum / static_cast<double>(n) : 0.0;
    stats_.rss_mb_mean      = n ? rss_sum / static_cast<double>(n) : 0.0;
    stats_.voluntary_ctx_switches   = last_vol   > base_vol   ? last_vol   - base_vol   : 0;
    stats_.involuntary_ctx_switches = last_invol > base_invol ? last_invol - base_invol : 0;
}

}  // namespace executor_comparison
