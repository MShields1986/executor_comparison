#!/usr/bin/env python3
"""Compare rclcpp executors from the CSVs written by executor_bench.launch.py.

Reads every `executor_*.csv` (NOT `*_summary.csv`) under a results directory,
parses the `#`-prefixed metadata header for run config, and draws two stacked
panels:

  1. violins of callback dispatch latency, coloured by executor
  2. bars of mean process CPU% for the same runs

Dispatch latency alone does not settle the executor question — the events
based executors trade a little latency for noticeably less CPU — so both
panels come from the same runs and share an x ordering.

Usage:
    ./scripts/plot_executors.py                              # plot every CSV in data/results
    ./scripts/plot_executors.py --results-dir data/results   # explicit dir
    ./scripts/plot_executors.py --filter 100s                # only files matching substring
    ./scripts/plot_executors.py --entity timer               # timer drift instead of dispatch
    ./scripts/plot_executors.py --group-by subs_work         # see --help
    ./scripts/plot_executors.py --out plot.png               # override output path

Requires: pandas, matplotlib (seaborn optional, nicer styling if present).
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import glob
import os
import sys
from typing import Dict, List, Optional

import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

try:
    import seaborn as sns
    _HAS_SNS = True
except ImportError:
    _HAS_SNS = False


# Fixed so a given executor keeps its colour across every plot in a report.
EXECUTOR_ORDER = [
    "single_threaded",
    "multi_threaded",
    "events",
    "events_cbg",
    "callback_isolated",
    "static_single_threaded",
]


@dataclasses.dataclass
class RunCsv:
    path: str
    meta: Dict[str, str]
    latency_ns: pd.Series

    def _m(self, key: str, default: str = "?") -> str:
        return self.meta.get(key, default)

    @property
    def executor(self) -> str:
        return self._m("executor", "unknown")

    @property
    def threads(self) -> str:
        return self._m("num_threads")

    @property
    def nodes(self) -> str:
        return self._m("num_nodes")

    @property
    def subs(self) -> str:
        return self._m("num_subscriptions")

    @property
    def timers(self) -> str:
        return self._m("num_timers")

    @property
    def rate(self) -> str:
        return self._m("publish_rate_hz")

    @property
    def payload(self) -> str:
        return self._m("payload_bytes", "0")

    @property
    def work(self) -> str:
        return self._m("callback_work_us", "0")

    @property
    def cbg(self) -> str:
        return self._m("callback_group", "?")

    @property
    def rmw(self) -> str:
        return self._m("rmw", "unknown")

    @property
    def ipc(self) -> bool:
        return self._m("use_intra_process_comms", "false").lower() == "true"

    @property
    def cpu_percent(self) -> float:
        return _as_float(self._m("cpu_percent_mean", "0"))

    @property
    def ctx_switches(self) -> float:
        return (_as_float(self._m("voluntary_ctx_switches", "0"))
                + _as_float(self._m("involuntary_ctx_switches", "0")))

    @property
    def dropped(self) -> int:
        return int(_as_float(self._m("samples_dropped", "0")))

    @property
    def throughput_pct(self) -> float:
        return _as_float(self._m("throughput_pct", "0"))

    @property
    def latency_mean_us(self) -> float:
        return _as_float(self._m("latency_mean_ns", "0")) / 1000.0

    @property
    def cpu_model(self) -> str:
        return self._m("cpu_model", "unknown CPU")

    @property
    def kernel(self) -> str:
        return self._m("kernel", "unknown kernel")

    @property
    def os_pretty(self) -> str:
        return self._m("os", "unknown OS")

    def label(self, scheme: str) -> str:
        parts: List[str] = []
        if "executor" in scheme:
            parts.append(self.executor)
        if "threads" in scheme:
            parts.append(f"{self.threads}t")
        if "nodes" in scheme:
            parts.append(f"{self.nodes}n")
        if "subs" in scheme:
            parts.append(f"{self.subs} subs")
        if "timers" in scheme:
            parts.append(f"{self.timers} timers")
        if "rate" in scheme:
            parts.append(f"{_as_float(self.rate):g}Hz")
        if "payload" in scheme:
            parts.append(_human_bytes(int(_as_float(self.payload))))
        if "work" in scheme:
            parts.append(f"{_as_float(self.work):g}us work")
        if "cbg" in scheme:
            parts.append(self.cbg)
        if "rmw" in scheme:
            parts.append(_short_rmw(self.rmw))
        if "ipc" in scheme:
            parts.append("ipc" if self.ipc else "no-ipc")
        return "\n".join(parts) if parts else os.path.basename(self.path)


def _as_float(s: str) -> float:
    try:
        return float(s)
    except (TypeError, ValueError):
        return 0.0


def _short_rmw(rmw: str) -> str:
    return rmw.replace("rmw_", "").replace("_cpp", "").replace("rtps", "dds")


def _human_bytes(n: int) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024:
            return f"{n}{unit}"
        n //= 1024
    return f"{n}TB"


def _parse_meta(path: str) -> Dict[str, str]:
    meta: Dict[str, str] = {}
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            if not line.startswith("#"):
                break
            kv = line[1:].strip()
            if "=" not in kv:
                continue
            k, v = kv.split("=", 1)
            meta[k.strip()] = v.strip()
    return meta


def _load_csv(path: str, entity: str) -> Optional[RunCsv]:
    meta = _parse_meta(path)
    if not meta:
        print(f"[warn] no metadata header in {path}, skipping", file=sys.stderr)
        return None
    try:
        df = pd.read_csv(path, comment="#",
                         usecols=["entity_type", "latency_ns"])
    except Exception as e:
        print(f"[warn] failed to read {path}: {e}", file=sys.stderr)
        return None
    if df.empty:
        print(f"[warn] {path} has no samples, skipping", file=sys.stderr)
        return None

    latency = df.loc[df["entity_type"] == entity, "latency_ns"].dropna()
    if latency.empty:
        print(f"[warn] {path} has no '{entity}' samples, skipping", file=sys.stderr)
        return None
    return RunCsv(path=path, meta=meta, latency_ns=latency)


def _find_csvs(results_dir: str, filt: Optional[str]) -> List[str]:
    pattern = os.path.join(results_dir, "executor_*.csv")
    paths = sorted(p for p in glob.glob(pattern) if not p.endswith("_summary.csv"))
    if filt:
        paths = [p for p in paths if filt in os.path.basename(p)]
    return paths


def _short_cpu(cpu: str) -> str:
    return " ".join(cpu.replace("(R)", "").replace("(TM)", "").split())


def _short_kernel(kernel: str) -> str:
    parts = kernel.split()
    return " ".join(parts[:2]) if len(parts) >= 2 else kernel


def _host_subtitle(runs: List[RunCsv]) -> str:
    """One-line host summary taken from the CSV metadata (the machine that
    produced the data, which need not be the one plotting it)."""
    combos, seen = [], set()
    for r in runs:
        key = (_short_cpu(r.cpu_model), r.os_pretty, _short_kernel(r.kernel))
        if key in seen:
            continue
        seen.add(key)
        combos.append(key)
    if len(combos) == 1:
        cpu, os_name, kern = combos[0]
        return f"{cpu}  •  {os_name}  •  {kern}"
    return " | ".join(f"{cpu} / {os_name} / {kern}" for cpu, os_name, kern in combos)


def _executor_order(present: List[str]) -> List[str]:
    known = [e for e in EXECUTOR_ORDER if e in present]
    return known + sorted(e for e in present if e not in EXECUTOR_ORDER)


def _build_frames(runs: List[RunCsv], scheme: str):
    samples, per_run = [], []
    for r in runs:
        label = r.label(scheme)
        samples.append(pd.DataFrame({
            "latency_us": r.latency_ns.values / 1000.0,
            "run": label,
            "executor": r.executor,
        }))
        per_run.append({
            "run": label,
            "executor": r.executor,
            "cpu_percent": r.cpu_percent,
            "ctx_switches": r.ctx_switches,
            "dropped": r.dropped,
            "throughput_pct": r.throughput_pct,
            "latency_mean_us": r.latency_mean_us,
        })
    return pd.concat(samples, ignore_index=True), pd.DataFrame(per_run)


def _bar_panel(ax, per_run, row_order, hue_order, column, ylabel, log=False):
    """One short bar row under the violins, sharing the violins' x ordering."""
    agg = per_run.groupby(["run", "executor"], as_index=False)[column].mean()
    if _HAS_SNS:
        sns.barplot(data=agg, x="run", y=column, hue="executor",
                    order=row_order, hue_order=hue_order,
                    ax=ax, legend=False, alpha=0.85)
    else:
        ax.bar(range(len(row_order)),
               [agg.loc[agg["run"] == r, column].mean() for r in row_order])
        ax.set_xticks(range(len(row_order)))
        ax.set_xticklabels(row_order)
    ax.set_xlabel("")
    ax.set_ylabel(ylabel)
    if log:
        ax.set_yscale("log")
    ax.tick_params(axis="x", labelsize=7)
    ax.grid(True, axis="y", linestyle=":", alpha=0.6)


def _plot(samples: pd.DataFrame, per_run: pd.DataFrame, entity: str,
          title: str, subtitle: str, out_path: str, log: bool) -> None:
    if samples.empty:
        print("[error] no samples to plot", file=sys.stderr)
        sys.exit(2)

    row_order = list(dict.fromkeys(samples["run"]))
    hue_order = _executor_order(sorted(samples["executor"].unique()))

    fig_h = max(9.0, 0.9 * len(row_order) + 6.0)
    fig, (ax_lat, ax_mean, ax_tput, ax_cpu) = plt.subplots(
        4, 1, figsize=(11.0, fig_h), height_ratios=[3.2, 1, 1, 1])

    if _HAS_SNS:
        sns.violinplot(
            data=samples, y="run", x="latency_us", hue="executor",
            order=row_order, hue_order=hue_order,
            cut=0, inner="quartile", linewidth=1.0,
            density_norm="width", ax=ax_lat, legend="brief",
            alpha=0.7, orient="h", width=0.9, gap=0.25,
        )
    else:
        groups = [samples.loc[samples["run"] == r, "latency_us"].values for r in row_order]
        parts = ax_lat.violinplot(groups, showmedians=True, widths=0.55, vert=False)
        for body in parts["bodies"]:
            body.set_alpha(0.7)
        ax_lat.set_yticks(range(1, len(row_order) + 1))
        ax_lat.set_yticklabels(row_order)

    xlabel = ("timer drift (\u00b5s)" if entity == "timer"
              else "callback dispatch latency (\u00b5s)")
    ax_lat.set_xlabel(xlabel)
    ax_lat.set_ylabel("")
    ax_lat.set_title(f"{title}\n{subtitle}" if subtitle else title, fontsize=11)
    if log:
        ax_lat.set_xscale("log")
        ax_lat.set_xlim(left=1.0)  # 1 microsecond
    else:
        ax_lat.set_xlim(left=0)
    ax_lat.grid(True, axis="x", which="both", linestyle=":", alpha=0.6)

    # Mean is what the ros2-performance derived benchmarks report, so it gets
    # its own row rather than being left implicit in the distribution.
    _bar_panel(ax_mean, per_run, row_order, hue_order,
               "latency_mean_us", "mean latency (\u00b5s)", log=log)
    _bar_panel(ax_tput, per_run, row_order, hue_order,
               "throughput_pct", "throughput (%)")
    ax_tput.axhline(100.0, color="#444", linewidth=0.8, linestyle="--", alpha=0.7)
    _bar_panel(ax_cpu, per_run, row_order, hue_order,
               "cpu_percent", "process CPU (%)")

    total_dropped = int(per_run["dropped"].sum())
    if total_dropped > 0:
        fig.text(0.01, 0.005,
                 f"note: {total_dropped} message(s) dropped across these runs",
                 fontsize=8, color="#a04000")

    fig.tight_layout()
    fig.savefig(out_path, dpi=140)
    print(f"[ok] wrote {out_path}", file=sys.stderr)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--results-dir", default="/data/results",
                    help="directory of executor_*.csv files (container path)")
    ap.add_argument("--filter", default=None,
                    help="only include CSVs whose filename contains this substring")
    ap.add_argument("--entity", default="subscription",
                    choices=["subscription", "timer"],
                    help="which callbacks to plot (default: subscription)")
    ap.add_argument("--group-by", default="subs_threads_cbg",
                    help="underscore-separated subset of {executor,threads,nodes,"
                         "subs,timers,rate,payload,work,cbg,rmw,ipc} used as the "
                         "run-label axis (executor is encoded by violin colour "
                         "via the hue, so it is omitted by default)")
    ap.add_argument("--out", default=None,
                    help="output PNG path (default: <results-dir>/executors_<ts>.png)")
    ap.add_argument("--title", default=None, help="override plot title")
    ap.add_argument("--linear", dest="log", action="store_false",
                    help="use a linear x axis (min=0) instead of the default log scale")
    ap.set_defaults(log=True)
    args = ap.parse_args()

    paths = _find_csvs(args.results_dir, args.filter)
    if not paths:
        print(f"[error] no matching CSVs in {args.results_dir}", file=sys.stderr)
        sys.exit(1)

    runs = [r for r in (_load_csv(p, args.entity) for p in paths) if r is not None]
    if not runs:
        print("[error] no loadable CSVs", file=sys.stderr)
        sys.exit(1)

    samples, per_run = _build_frames(runs, args.group_by)

    what = "Timer drift" if args.entity == "timer" else "Callback dispatch latency"
    title = args.title or f"{what} by executor ({len(runs)} runs)"
    subtitle = _host_subtitle(runs)

    if args.out:
        out_path = args.out
    else:
        ts = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
        suffix = args.filter or "all"
        out_path = os.path.join(args.results_dir, f"executors_{suffix}_{ts}.png")

    _plot(samples, per_run, args.entity, title, subtitle, out_path, args.log)


if __name__ == "__main__":
    main()
