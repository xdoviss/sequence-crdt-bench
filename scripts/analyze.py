#!/usr/bin/env python3

import json
import os
import sys
import warnings
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd
from matplotlib.ticker import NullFormatter

warnings.simplefilter("ignore", pd.errors.PerformanceWarning)

# graph styling

ALGOS = ["WOOT", "Logoot", "RGA", "LSEQ", "Treedoc"]  # fixed order everywhere

# every algorithm keeps one colour, marker and line style in every figure
STYLE = {
    "WOOT":    dict(color="#2a78d6", marker="o", linestyle="-"),
    "Logoot":  dict(color="#eb6834", marker="s", linestyle="--"),
    "RGA":     dict(color="#1baf7a", marker="^", linestyle="-."),
    "LSEQ":    dict(color="#eda100", marker="D", linestyle=(0, (5, 1, 1, 1))),
    "Treedoc": dict(color="#e87ba4", marker="v", linestyle=":"),
}

HATCH = {"Logoot": "//", "LSEQ": "\\\\", "Treedoc": ".."}  # bar patterns
INK, MUTED, GRID = "#1f1f1e", "#6b6a64", "#e6e5df"

plt.rcParams.update({
    "figure.dpi": 130, "savefig.dpi": 160, "font.size": 9.5, "axes.titlesize": 10.5,
    "axes.edgecolor": MUTED, "axes.labelcolor": INK, "xtick.color": MUTED, "ytick.color": MUTED,
    "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.8, "axes.axisbelow": True,
    "axes.spines.top": False, "axes.spines.right": False, "legend.frameon": False,
    "lines.linewidth": 2.0, "lines.markersize": 6,
})

OUT = sys.argv[1] if len(sys.argv) > 1 else "results/quick"
FIG = os.path.join(OUT, "figures")

tables = []
findings = {} # criterion -> {x value: {best, value, ranking}}, written to findings.json
figures = []

# data helpers

def load(name):
    """loads result file, returns `None` if not found"""

    path = os.path.join(OUT, name)
    return pd.read_csv(path) if os.path.exists(path) else None


def present(frame):
    """algorithms that appear in `frame`, in fixed order"""
    return [algo for algo in ALGOS if algo in set(frame.algorithm)]


def ordered(frame, keys):
    """sorts by `keys`, then by algorithm order"""
    frame = frame.copy()
    frame["algo_order"] = frame.algorithm.map(ALGOS.index)

    return frame.sort_values(keys + ["algo_order"])


def average_runs(frame, keys):
    """ran simulations repeat with different seeds -> average the repetitive"""
    return frame.groupby(["algorithm"] + keys, as_index=False).mean(numeric_only=True)


def best(frame, key, value, lower=True):
    """for every key returns the best algorithm by `value` and the full ranking"""
    result = {}
    for key_value, group in frame.groupby(key):
        group = group.dropna(subset=[value])
        if group.empty:
            continue

        winner = group.loc[group[value].idxmin() if lower else group[value].idxmax()]
        result[str(key_value)] = {"best": winner.algorithm, "value": float(winner[value]),
                                  "ranking": list(group.sort_values(value, ascending=lower).algorithm)}

    return result


# table helpers

def fmt(value, digits=3):
    """formats numbers -> 12,345 / 12.3 / 0.0123."""
    if pd.isna(value):
        return "-"
    
    if abs(value) >= 1000:
        return f"{value:,.0f}"
    
    if abs(value) < 1:
        return f"{value:.{digits}g}"
    
    return f"{value:.{digits}f}".rstrip("0").rstrip(".")


def md_table(frame, columns, headers=None):
    """returns markdown table of `columns` where floats are shortened with `fmt()`"""
    headers = headers or columns
    text = "| " + " | ".join(headers) + " |\n|" + "|".join("---" for _ in columns) + "|\n"
    
    for _, row in frame.iterrows():
        cells = [fmt(row[column]) if isinstance(row[column], (int, float)) else str(row[column]) for column in columns]
        text += "| " + " | ".join(cells) + " |\n"
        
    return text


def add_table(title, frame, columns, headers=None, note=""):
    tables.append(f"## {title}\n\n{note}" + md_table(frame, columns, headers) + "\n")


def with_ci(frame):
    """adds a 'mean ± ci95' column"""
    frame["mean ± ci95"] = frame.apply(lambda row: f"{fmt(row['mean'])} ± {fmt(row['ci95'])}", axis=1)
    return frame


# matplotlib helpers

def grid(rows, columns, width, height):
    """returns figure with rows x columns panels (axes[row][column])"""
    return plt.subplots(rows, columns, figsize=(width, height), squeeze=False)


def lines(axis, frame, x, y, title, x_label, y_label, log_x=True, log_y=False):
    """single panel where a line per algorithm, y against x"""
    for algo in present(frame):
        subset = frame[frame.algorithm == algo].sort_values(x)
        axis.plot(subset[x], subset[y], label=algo, **STYLE[algo])
        
    if log_x:
        # log scale, labelled only at the measured x values
        ticks = sorted(set(frame[x]))
        axis.set_xscale("log")
        axis.set_xticks(ticks)
        axis.set_xticklabels([f"{tick:,.0f}" if tick >= 1 else f"{tick:g}" for tick in ticks])
        axis.xaxis.set_minor_formatter(NullFormatter())
        
    if log_y:
        axis.set_yscale("log")
        
    axis.set_title(title, loc="left")
    axis.set_xlabel(x_label)
    axis.set_ylabel(y_label)


def bars(axis, frame, category, value, title, y_label, log=False):
    """single panel where groups of bars (one bar per algorithm) for every category"""
    categories = list(dict.fromkeys(frame[category]))
    algos = present(frame)
    width = 0.8 / max(1, len(algos))
    for slot, algo in enumerate(algos):
        heights = frame[frame.algorithm == algo].set_index(category).reindex(categories)[value]
        positions = [index + (slot - (len(algos) - 1) / 2) * width for index in range(len(categories))]
        axis.bar(positions, heights, width=width * 0.92, color=STYLE[algo]["color"], label=algo,
                 edgecolor="white", linewidth=0.6, hatch=HATCH.get(algo, ""))
        
    axis.set_xticks(range(len(categories)))
    axis.set_xticklabels(categories, rotation=25, ha="right")
    
    if log:
        axis.set_yscale("log")
        
    axis.set_title(title, loc="left")
    axis.set_ylabel(y_label)


def save(figure, axes, name, title):
    """adds title and shared legend at the bottom, then writes figures/<name>"""
    figure.suptitle(title, x=0.01, ha="left", fontsize=11.5, color=INK)
    handles, labels = axes[0][0].get_legend_handles_labels()
    figure.legend(handles, labels, loc="lower center", ncol=len(labels), bbox_to_anchor=(0.5, -0.01))
    figure.tight_layout(rect=(0, 0.06, 1, 0.95))
    figure.savefig(os.path.join(FIG, name), bbox_inches="tight", facecolor="white")
    plt.close(figure)
    figures.append(name)


# benchmarks - micro

def micro_benchmarks():
    micro = load("micro.csv")
    if micro is not None:
        # each figure contains one row per metric, left = median, right = p99
        for name, title, y_label, metrics in [
            ("fig01_insert_latency_vs_size.png", "Insertion latency vs document size", "µs per operation",
             [("insert_latency_us", "Local insertion")]),
            ("fig02_delete_latency_vs_size.png", "Deletion latency vs document size", "µs per operation",
             [("delete_latency_us", "Local deletion")]),
            ("fig03_lookup_latency_vs_size.png", "Lookup latency vs document size", "µs per lookup",
             [("lookup_pos_us", "Lookup by position"), ("lookup_id_us", "Lookup by identifier")]),
            ("fig04_traversal_latency_vs_size.png", "Traversal latency vs document size", "ms per traversal",
             [("traversal_ms", "Full traversal")]),
            ("fig04b_merge_and_serialization_vs_size.png", "Merge and serialization vs document size",
             "µs (merge) / ms (state)",
             [("merge_remote_op_us", "Remote operation (merge)"), ("state_serialize_ms", "State serialization")]),
        ]:
            figure, axes = grid(len(metrics), 2, 10, 3.6 * len(metrics))
            for row, (metric, metric_title) in enumerate(metrics):
                subset = micro[micro.metric == metric]
                lines(axes[row][0], subset, "doc_size", "median", f"{metric_title} — median (p50)",
                      "document size (elements)", y_label, log_y=True)
                lines(axes[row][1], subset, "doc_size", "p99", f"{metric_title} — p99",
                      "document size (elements)", y_label, log_y=True)
                
            save(figure, axes, name, title)

        tables.append("## Operation latency vs document size (micro-benchmarks)\n\n"
                      "Every operation timed individually; warm-up excluded; mean ± 95% CI half-width.\n")
        
        for metric in ["insert_latency_us", "delete_latency_us", "lookup_pos_us", "lookup_id_us", "traversal_ms",
                       "merge_remote_op_us", "merge_batch_ms", "op_encode_us", "op_decode_us",
                       "state_serialize_ms", "state_deserialize_ms"]:
            subset = micro[micro.metric == metric]
            
            if subset.empty:
                continue
            
            subset = with_ci(ordered(subset, ["doc_size"]))
            tables.append(f"### {metric} ({subset.unit.iloc[0]})\n\n" + md_table(
                subset, ["doc_size", "algorithm", "n", "mean ± ci95", "median", "p95", "p99", "min", "max",
                         "stddev"]) + "\n")
            
            findings[f"micro.{metric}.median"] = best(subset, "doc_size", "median")

    sizes = load("micro_sizes.csv")
    
    if sizes is not None:
        figure, axes = grid(1, 3, 13, 3.8)
        x_label = "document size (elements)"
        lines(axes[0][0], sizes, "doc_size", "state_metadata_bytes_per_element", "Snapshot metadata per element",
              x_label, "bytes / element (excl. 1-byte value)")
        
        lines(axes[0][1], sizes, "doc_size", "insert_op_body_bytes", "Insert operation payload",
              x_label, "bytes / operation")
        
        lines(axes[0][2], sizes, "doc_size", "id_bytes_avg", "Encoded identifier", x_label, "bytes / identifier")
        save(figure, axes, "fig10_serialized_metadata_vs_doc_size.png", "Serialized metadata size vs document size")

        sizes = ordered(sizes, ["doc_size"])
        add_table("Serialization sizes, identifiers and throughput (micro)", sizes,
                  ["doc_size", "algorithm", "state_bytes", "state_bytes_per_element", "insert_op_body_bytes",
                   "delete_op_body_bytes", "insert_op_wire_bytes", "id_bytes_avg",
                   "avg_id_depth", "insert_ops_per_sec", "merge_ops_per_sec"],
                  ["n", "algorithm", "state B", "state B/elem", "insert op B", "delete op B", "insert wire B",
                   "id B", "avg id depth", "insert ops/s", "merge ops/s"])
        # heap per element - see the memory experiment (micro runs without the heap hook, so it has no heap numbers)
        
        findings["sizes.state_bytes_per_element"] = best(sizes, "doc_size", "state_bytes_per_element")
        findings["sizes.insert_op_body_bytes"] = best(sizes, "doc_size", "insert_op_body_bytes")


# merging concurrent edits

def merge_experiment():
    merge = load("merge.csv")
    if merge is not None:
        latency = merge[merge.metric == "merge_latency_us"]
        patterns = list(dict.fromkeys(merge.pattern))
        figure, axes = grid(1, len(patterns), 5.2 * len(patterns), 3.8)
        
        for index, pattern in enumerate(patterns):
            lines(axes[0][index], latency[latency.pattern == pattern], "replicas", "mean",
                  f"pattern: {pattern} (mean per remote op)", "concurrent replicas", "µs per remote operation",
                  log_y=True)
            
        save(figure, axes, "fig05_merge_latency_vs_replicas.png", "Merge latency vs number of concurrent replicas")

        latency = with_ci(ordered(latency, ["pattern", "replicas"]))
        add_table("Merge latency vs concurrent replicas (µs per remote operation)", latency,
                  ["pattern", "replicas", "algorithm", "n", "mean ± ci95", "median", "p95", "p99", "max"])
        
        for pattern in patterns:
            findings[f"merge.{pattern}.mean"] = best(latency[latency.pattern == pattern], "replicas", "mean")

    merge_sync = load("merge_sync.csv")
    if merge_sync is not None:
        sync = average_runs(merge_sync, ["pattern", "replicas"])
        patterns = list(dict.fromkeys(sync.pattern))
        figure, axes = grid(1, len(patterns), 5.2 * len(patterns), 3.8)
        
        for index, pattern in enumerate(patterns):
            lines(axes[0][index], sync[sync.pattern == pattern], "replicas", "sync_total_ms", f"pattern: {pattern}",
                  "concurrent replicas", "CPU ms until all converge", log_y=True)
            
        save(figure, axes, "fig08b_sync_cpu_time_vs_replicas.png",
             "Synchronization processing time vs replica count (encode + decode + apply, all replicas)")

        sync = ordered(sync, ["pattern", "replicas"])
        add_table("Synchronization (CPU) until convergence vs replicas", sync,
                  ["pattern", "replicas", "algorithm", "sync_total_ms", "sync_encode_ms", "sync_decode_ms",
                   "sync_apply_ms", "sync_per_replica_ms", "bytes_per_op", "converged"])
        
        for pattern in patterns:
            findings[f"sync_cpu.{pattern}"] = best(sync[sync.pattern == pattern], "replicas", "sync_total_ms")


# memory

def memory_experiment():
    memory = load("memory.csv")
    if memory is None:
        return
    
    x = "axis_value" if "axis_value" in memory.columns else "x"  # older result files call it "x"
    bulk, growth = memory[memory.experiment == "bulk"], memory[memory.experiment == "growth"]
    cycles, deletions = memory[memory.experiment == "cycles"], memory[memory.experiment == "deletion"]

    figure, axes = grid(1, 2, 10, 3.8)
    lines(axes[0][0], bulk, x, "heap_bytes", "Total heap", "document size (elements)", "bytes", log_y=True)
    lines(axes[0][1], bulk, x, "heap_bytes_per_logical", "Heap per element", "document size (elements)",
          "bytes / element")
    
    save(figure, axes, "fig06_memory_vs_doc_size.png", "Memory consumption vs document size (initial document)")

    # growth from an empty document
    patterns = list(dict.fromkeys(growth.pattern))
    for metric, name, title, y_label in [
        ("heap_bytes", "fig07_memory_vs_op_count.png",
         "Memory consumption vs operation count (single replica, from empty)", "heap bytes"),
        ("avg_id_depth", "fig07b_identifier_depth_vs_op_count.png",
         "Identifier / tree depth growth (WOOT and RGA ids are constant-size: depth 1)", "avg identifier depth"),
    ]:
        rows = (len(patterns) + 2) // 3
        figure, axes = grid(rows, 3, 13, 3.4 * rows)
        for index in range(len(patterns), rows * 3):
            axes[index // 3][index % 3].axis("off")
            
        for index, pattern in enumerate(patterns):
            axis, subset = axes[index // 3][index % 3], growth[growth.pattern == pattern]
            lines(axis, subset, "ops_applied", metric, pattern, "operations applied", y_label, log_x=False,
                  log_y=True)
            
            if metric == "heap_bytes":  # mark runs that were stopped at the memory budget
                for _, stopped in subset[subset.truncated == 1].groupby("algorithm"):
                    axis.annotate("budget hit", (stopped.ops_applied.iloc[-1], stopped.heap_bytes.iloc[-1]),
                                  fontsize=7.5, color=MUTED, xytext=(-40, 6), textcoords="offset points")
                    
        save(figure, axes, name, title)

    figure, axes = grid(1, 2, 10, 3.8)
    lines(axes[0][0], cycles, x, "heap_bytes", "Repeated insert/delete cycles (constant logical size)", "cycle",
          "heap bytes", log_x=False)
    
    lines(axes[0][1], deletions, x, "heap_bytes", "Memory retained after deleting a fraction",
          "fraction of document deleted", "heap bytes", log_x=False)
    
    save(figure, axes, "fig06b_memory_after_deletions.png", "Tombstones: memory after deletions")

    for experiment, title in [("bulk", "initial document"), ("cycles", "insert/delete cycles"),
                              ("deletion", "after deletions")]:
        add_table(f"Memory — {title}", ordered(memory[memory.experiment == experiment], [x]),
                  [x, "algorithm", "logical_size", "stored_elements", "tombstones", "placeholders", "heap_bytes",
                   "peak_heap_bytes", "est_bytes", "heap_bytes_per_logical", "avg_id_depth", "serialized_bytes"],
                  ["x", "algorithm", "logical", "stored", "tombstones", "placeholders", "heap B", "peak B",
                   "estimate B", "heap B/elem", "avg id depth", "snapshot B"])

    final = growth.sort_values("ops_applied").groupby(["algorithm", "pattern"], as_index=False).last()
    final = ordered(final, ["pattern"])
    add_table("Memory — growth (state after the last operation)", final,
              ["pattern", "algorithm", "ops_applied", "logical_size", "stored_elements", "tombstones", "heap_bytes",
               "heap_bytes_per_op", "avg_id_depth", "max_id_depth", "truncated"])
    findings["memory.bulk.heap_per_element"] = best(bulk, x, "heap_bytes_per_logical")
    findings["memory.growth.heap_per_op"] = best(final, "pattern", "heap_bytes_per_op")
    findings["memory.deletion.heap"] = best(deletions, x, "heap_bytes")
    findings["memory.cycles.heap"] = best(cycles[cycles[x] == cycles[x].max()], x, "heap_bytes")


# network simulations

def workload_simulations():
    runs = load("workloads.csv")
    if runs is None:
        return
    
    data = average_runs(runs, ["pattern"])
    data["cpu_us_per_op"] = data.cpu_processing_ms * 1000 / (data.ops_total + data.remote_applications)
    data = data.set_index("pattern").loc[list(dict.fromkeys(runs.pattern))].reset_index()  # keep the run order

    figure, axes = grid(2, 2, 13, 8)
    bars(axes[0][0], data, "pattern", "cpu_us_per_op", "CPU per operation (local + remote)", "µs", log=True)
    bars(axes[0][1], data, "pattern", "mem_bytes_per_element", "Memory per visible element (per replica)", "bytes",
         log=True)
    bars(axes[1][0], data, "pattern", "avg_id_depth", "Average identifier / tree depth", "levels", log=True)
    bars(axes[1][1], data, "pattern", "payload_bytes_per_op", "CRDT payload per delivered operation", "bytes",
         log=True)
    
    save(figure, axes, "fig11_workload_characteristics.png", "Workload characteristics (5 replicas, default network)")

    data = ordered(data, ["pattern"])
    add_table("Workload patterns (network simulation)", data,
              ["pattern", "algorithm", "converged", "cpu_us_per_op", "remote_apply_us_mean", "remote_apply_us_p99",
               "mem_bytes_per_element", "tombstones", "avg_id_depth", "max_id_depth", "payload_bytes_per_op",
               "state_bytes", "processing_ops_per_sec"],
              ["pattern", "algorithm", "conv", "CPU µs/op", "apply µs mean", "apply µs p99", "mem B/elem",
               "tombstones", "avg depth", "max depth", "payload B/op", "snapshot B", "ops/s"])
    
    for value in ["cpu_us_per_op", "mem_bytes_per_element", "payload_bytes_per_op"]:
        findings[f"workloads.{value}"] = best(data, "pattern", value)


def network_simulations():
    runs = load("network.csv")
    if runs is None:
        return
    
    data = average_runs(runs, ["scenario"])
    data = data.set_index("scenario").loc[list(dict.fromkeys(runs.scenario))].reset_index()  # keep the run order

    figure, axes = grid(1, 3, 15, 4.2)
    bars(axes[0][0], data, "scenario", "propagation_ms_median", "Propagation delay p50 (generation → remote apply)",
         "ms (virtual time)")
    bars(axes[0][1], data, "scenario", "propagation_ms_p99", "Propagation delay p99", "ms (virtual time)")
    bars(axes[0][2], data, "scenario", "causal_wait_ms_mean", "Mean causal-buffer wait", "ms (virtual time)")
    
    save(figure, axes, "fig14_network_scenarios.png", "Network scenarios: synchronization delay")

    figure, axes = grid(1, 2, 11, 4)
    bars(axes[0][0], data, "scenario", "bytes_per_op_delivery",
         "Bytes transmitted per delivered operation (incl. retransmissions, acks)", "bytes")
    bars(axes[0][1], data, "scenario", "cpu_processing_ms", "Total CPU processing (all replicas)", "ms (wall clock)")
    
    save(figure, axes, "fig19_network_bytes_and_cpu.png", "Network scenarios: transmission and processing overhead")

    data = ordered(data, ["scenario"])
    
    add_table("Network scenarios", data,
              ["scenario", "algorithm", "converged", "propagation_ms_mean", "propagation_ms_median",
               "propagation_ms_p95", "propagation_ms_p99", "network_delay_ms_mean", "causal_wait_ms_mean",
               "settle_time_ms", "messages", "retransmissions", "bytes_per_op_delivery", "avg_message_bytes",
               "cpu_processing_ms"],
              ["scenario", "algorithm", "conv", "prop mean", "prop p50", "prop p95", "prop p99", "net delay",
               "causal wait", "settle ms", "messages", "retx", "B/op", "avg msg B", "CPU ms"],
              note="Synchronization delay (virtual ms) is reported separately from network transmission delay and "
                   "from local processing (wall-clock CPU).\n\n")
    
    findings["network.bytes_per_op"] = best(data, "scenario", "bytes_per_op_delivery")
    findings["network.cpu_processing_ms"] = best(data, "scenario", "cpu_processing_ms")


def sync_interval_simulations():
    runs = load("sync_intervals.csv")
    if runs is None:
        return
    
    data = average_runs(runs, ["sync_interval_ms"])
    figure, axes = grid(1, 3, 14, 3.9)
    x_label = "synchronization interval (ms, 0 = immediate)"
    lines(axes[0][0], data, "sync_interval_ms", "messages", "Messages sent", x_label, "messages", log_x=False)
    lines(axes[0][1], data, "sync_interval_ms", "propagation_ms_mean", "Mean propagation delay", x_label,
          "ms (virtual)", log_x=False)
    lines(axes[0][2], data, "sync_interval_ms", "bytes_per_op_delivery", "Bytes per delivered operation", x_label,
          "bytes", log_x=False)
    
    for axis in axes[0]:
        axis.set_xscale("symlog", linthresh=50)  # 0 cannot be shown on a log axis
        
    axes[0][0].set_yscale("log")  # only after the x scale or the x axis grows into negative values
    
    save(figure, axes, "fig15_sync_intervals.png", "Synchronization interval / batching")

    add_table("Synchronization intervals (batching)", ordered(data, ["sync_interval_ms"]),
              ["sync_interval_ms", "algorithm", "messages", "avg_message_bytes", "bytes_per_op_delivery",
               "envelope_bytes_per_op", "payload_bytes_per_op", "batching_wait_ms_mean", "propagation_ms_mean",
               "propagation_ms_p99", "cpu_processing_ms"])


def replica_count_simulations():
    runs = load("scale_replicas.csv")
    if runs is None:
        return
    
    data = average_runs(runs, ["pattern", "replicas"])
    patterns = list(dict.fromkeys(runs.pattern))
    figure, axes = grid(len(patterns), 3, 15, 3.7 * len(patterns))
    
    for row, pattern in enumerate(patterns):
        subset = data[data.pattern == pattern]
        lines(axes[row][0], subset, "replicas", "settle_time_ms", f"Time to converge after last edit* — {pattern}",
              "replicas", "ms (virtual)")
        lines(axes[row][1], subset, "replicas", "propagation_ms_p99", f"Propagation delay p99* — {pattern}",
              "replicas", "ms (virtual)")
        lines(axes[row][2], subset, "replicas", "cpu_processing_ms", f"CPU processing (all replicas) — {pattern}",
              "replicas", "ms (wall clock)", log_y=True)
        
    save(figure, axes, "fig08_sync_time_vs_replicas.png",
         "Synchronization time vs replica count  (* identical for all algorithms: set by network + causal delivery)")

    data = ordered(data, ["pattern", "replicas"])
    add_table("Scalability: replica count (network simulation)", data,
              ["pattern", "replicas", "algorithm", "converged", "settle_time_ms", "propagation_ms_mean",
               "propagation_ms_p99", "causal_wait_ms_mean", "remote_apply_us_mean", "remote_apply_us_p99",
               "cpu_processing_ms", "bytes_per_op_delivery", "envelope_bytes_per_op", "mem_per_replica_bytes"])
    
    for pattern in patterns:
        rows = data[data.pattern == pattern]
        findings[f"scale_replicas.{pattern}.cpu"] = best(rows, "replicas", "cpu_processing_ms")
        findings[f"scale_replicas.{pattern}.apply_p99"] = best(rows, "replicas", "remote_apply_us_p99")


def operation_count_simulations():
    runs = load("scale_ops.csv")
    if runs is None:
        return
    
    data = average_runs(runs, ["ops_per_replica"])
    figure, axes = grid(1, 3, 14, 3.9)
    x_label = "operations per replica"
    lines(axes[0][0], data, "ops_per_replica", "processing_ops_per_sec", "Processing throughput", x_label,
          "operations / s", log_y=True)
    lines(axes[0][1], data, "ops_per_replica", "remote_apply_us_p99", "Remote apply latency p99", x_label, "µs",
          log_y=True)
    lines(axes[0][2], data, "ops_per_replica", "mem_per_replica_bytes", "Memory per replica", x_label, "bytes",
          log_y=True)
    
    save(figure, axes, "fig18_scale_ops.png", "Scalability: operations per user (3 replicas)")

    data = ordered(data, ["ops_per_replica"])
    add_table("Scalability: operations per replica", data,
              ["ops_per_replica", "algorithm", "converged", "processing_ops_per_sec", "remote_apply_us_mean",
               "remote_apply_us_p99", "local_op_us_p99", "mem_per_replica_bytes", "tombstones", "avg_id_depth",
               "state_bytes"])
    findings["scale_ops.throughput"] = best(data, "ops_per_replica", "processing_ops_per_sec", lower=False)


def intensity_simulations():
    runs = load("intensity.csv")
    if runs is None:
        return
    
    data = average_runs(runs, ["offered_ops_per_sec", "burst_size"])
    bursts = sorted(set(data.burst_size))
    figure, axes = grid(1, len(bursts), 5.4 * len(bursts), 3.9)
    for index, burst in enumerate(bursts):
        lines(axes[0][index], data[data.burst_size == burst], "offered_ops_per_sec", "processing_ops_per_sec",
              f"burst size {burst}", "offered load (edits / s, all replicas)", "processed operations / CPU-second",
              log_y=True)
        
    save(figure, axes, "fig09_throughput_vs_intensity.png", "Throughput vs workload intensity (hotspot editing)")

    data = ordered(data, ["burst_size", "offered_ops_per_sec"])
    add_table("Throughput vs workload intensity", data,
              ["burst_size", "offered_ops_per_sec", "algorithm", "processing_ops_per_sec", "remote_apply_us_mean",
               "remote_apply_us_p99", "causally_buffered", "propagation_ms_p99", "cpu_utilization"])
    
    largest_burst = data[data.burst_size == data.burst_size.max()]
    findings["intensity.throughput"] = best(largest_burst, "offered_ops_per_sec", "processing_ops_per_sec",
                                            lower=False)


# tests, environment, output files

def test_summary():
    path = os.path.join(OUT, "tests_summary.json")
    if not os.path.exists(path):
        return
    
    with open(path, encoding="utf-8") as handle:
        tests = json.load(handle)
        
    keys = ["tests_total", "tests_passed", "tests_failed", "assertions", "property_cases", "convergence_checks",
            "convergence_violations", "invariant_checks", "invariant_violations"]
    
    tables.insert(0, "## Correctness test summary\n\n| tests | passed | failed | assertions | property cases | "
                     "convergence checks | convergence violations | invariant checks | invariant violations |\n"
                     "|---|---|---|---|---|---|---|---|---|\n| " + " | ".join(str(tests[key]) for key in keys) + " |\n")
    
    findings["tests"] = tests


def environment_line():
    """describes environment, taken from the first metadata file found"""
    for name in ["micro.meta.json", "network.meta.json"]:
        path = os.path.join(OUT, name)
        
        if os.path.exists(path):
            with open(path, encoding="utf-8") as handle:
                env = json.load(handle)["environment"]
                
            return (f"Environment: {env['cpu']} ({env['logical_cores']} logical cores), "
                    f"{env['total_ram_bytes'] / 2**30:.1f} GiB RAM, {env['os']}; {env['compiler']}, "
                    f"{env['build_type']} `{env['cxx_flags']}`; measured {env['timestamp_utc']}.\n\n")
            
    return ""


def main():
    os.makedirs(FIG, exist_ok=True)
    micro_benchmarks()
    merge_experiment()
    memory_experiment()
    workload_simulations()
    network_simulations()
    sync_interval_simulations()
    replica_count_simulations()
    operation_count_simulations()
    intensity_simulations()
    test_summary()

    with open(os.path.join(OUT, "tables.md"), "w", encoding="utf-8") as handle:
        handle.write(f"# Results tables ({os.path.basename(os.path.normpath(OUT))})\n\n" + environment_line())
        handle.write("Figures: " + ", ".join(f"[{name}](figures/{name})" for name in figures) + "\n\n")
        handle.write("\n".join(tables))
        
    with open(os.path.join(OUT, "findings.json"), "w", encoding="utf-8") as handle:
        json.dump(findings, handle, indent=1)
        
    print(f"wrote {len(figures)} figures to {FIG}, tables.md and findings.json")


if __name__ == "__main__":
    main()
