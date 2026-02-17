import argparse
import pandas as pd
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib import transforms

# tuning params (visual)
BASE_BOTTOM = -0.03     # axes-fraction for first level bottom (just below axes)
STEP = 0.15            # how much lower each next stack goes (in axes fraction)
MIN_BOTTOM = -3      # don't go lower than this (axes fraction)
LABEL_PAD = 0.01        # extra gap below the line for the label (axes fraction)

# defaults for merging/filtering (in nanoseconds)
DEFAULT_MERGE_GAP_NS = 1_000_000    # merge microslices separated by <= this gap (1 ms)
DEFAULT_MAX_DURATION_NS = 100_000     # if set (int), ignore slices longer than this


def read_and_coerce(file_path):
    """
    Read CSV and coerce numeric time columns to integer (or Int64).
    Returns a DataFrame.
    """
    df = pd.read_csv(file_path)

    # ensure numeric types where present
    for col in ("arrive_ns", "start_ns", "end_ns", "duration_ns"):
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce")

    # if duration_ns is missing but start/end exist, compute it
    if "duration_ns" not in df.columns and ("start_ns" in df.columns and "end_ns" in df.columns):
        df["duration_ns"] = df["end_ns"] - df["start_ns"]

    # drop fully-empty rows for start/end
    df = df.dropna(subset=["start_ns", "end_ns"], how="any").reset_index(drop=True)

    # sort by start time as a basic canonical ordering
    df = df.sort_values("start_ns").reset_index(drop=True)
    return df

def consolidate_adjacent_by_pid(df, pid_col="pid"):
    """
    Consolidate consecutive rows (sorted by start_ns) that have the same PID.
    This merges runs of rows where no other PID interrupts between them,
    and ignores any merge margin (i.e. gaps are allowed).
    Returns a new DataFrame with merged intervals and recomputed duration_ns.
    """
    if df is None or df.empty:
        return df

    # Choose column to use as PID; if absent, fall back to child_index
    if pid_col not in df.columns:
        if "child_index" in df.columns:
            pid_col = "child_index"
        else:
            # nothing meaningful to consolidate by; return as-is
            return df

    # Work on a copy sorted by start time
    dfe = df.sort_values("start_ns").reset_index(drop=True)
    merged = []
    cur = None

    for _, row in dfe.iterrows():
        # make sure numeric ints for starts/ends
        s = int(row["start_ns"])
        e = int(row["end_ns"])
        pid = row[pid_col]

        if cur is None:
            cur = row.to_dict()
            cur["start_ns"] = s
            cur["end_ns"] = e
        else:
            cur_pid = cur.get(pid_col)
            if cur_pid == pid:
                # same PID and consecutive in sorted order -> merge by extending end
                cur["end_ns"] = max(int(cur["end_ns"]), e)
                # Optionally: update other fields if you want (keep the first one's metadata)
            else:
                # different PID -> flush current and start new
                merged.append(cur)
                cur = row.to_dict()
                cur["start_ns"] = s
                cur["end_ns"] = e

    if cur is not None:
        merged.append(cur)

    if not merged:
        return pd.DataFrame(columns=df.columns)

    out = pd.DataFrame(merged)

    # recompute duration
    out["start_ns"] = pd.to_numeric(out["start_ns"], errors="coerce")
    out["end_ns"] = pd.to_numeric(out["end_ns"], errors="coerce")
    out["duration_ns"] = out["end_ns"] - out["start_ns"]

    # keep sorted
    out = out.sort_values("start_ns").reset_index(drop=True)
    return out

def merge_slices(df, merge_gap_ns=DEFAULT_MERGE_GAP_NS, max_duration_ns=DEFAULT_MAX_DURATION_NS):
    """
    Merge nearby microslices that belong to the same logical group.
    Grouping keys are: pid, child_index and arrive_ns (if arrive_ns exists).
    Two consecutive microslices in the same group are merged if the gap between
    the previous end and next start is <= merge_gap_ns (this also handles overlap).
    After merging, recompute duration_ns. Optionally drop slices with duration > max_duration_ns.
    """

    # Make a working copy
    dfw = df.copy()

    # Decide grouping keys depending on available columns
    group_keys = []
    if "pid" in dfw.columns:
        group_keys.append("pid")
    if "child_index" in dfw.columns:
        group_keys.append("child_index")
    if "arrive_ns" in dfw.columns:
        # fillna with -1 so missing arrive times are treated as a single group key value
        dfw["arrive_ns"] = dfw["arrive_ns"].fillna(-1)
        group_keys.append("arrive_ns")

    if not group_keys:
        # no grouping keys available — nothing to merge by
        return dfw

    merged_rows = []

    # ensure start/end are numeric ints for comparisons
    dfw["start_ns"] = pd.to_numeric(dfw["start_ns"], errors="coerce")
    dfw["end_ns"] = pd.to_numeric(dfw["end_ns"], errors="coerce")

    # group and merge
    for _, grp in dfw.groupby(group_keys, sort=False):
        g = grp.sort_values("start_ns").reset_index(drop=True)

        cur = None  # will hold a Series-like dict for the currently building merged slice
        for idx, row in g.iterrows():
            s = int(row["start_ns"])
            e = int(row["end_ns"])
            # skip inverted intervals (s >= e)
            if e <= s:
                continue

            if cur is None:
                # start a new merged interval: copy row to dict to preserve other fields
                cur = row.to_dict()
                cur["start_ns"] = s
                cur["end_ns"] = e
            else:
                prev_end = int(cur["end_ns"])
                # if current slice starts within merge gap of previous end (or overlaps), merge
                if s <= prev_end + int(merge_gap_ns):
                    # extend the end to the max
                    cur["end_ns"] = max(prev_end, e)
                    # optional: we could keep track of microslice count etc.
                else:
                    # finalize previous merged slice
                    merged_rows.append(cur)
                    # start new merged slice
                    cur = row.to_dict()
                    cur["start_ns"] = s
                    cur["end_ns"] = e

        # finalize group's last pending slice
        if cur is not None:
            merged_rows.append(cur)

    if not merged_rows:
        return pd.DataFrame(columns=dfw.columns)

    merged_df = pd.DataFrame(merged_rows)

    # recompute duration if needed
    merged_df["start_ns"] = pd.to_numeric(merged_df["start_ns"], errors="coerce")
    merged_df["end_ns"] = pd.to_numeric(merged_df["end_ns"], errors="coerce")
    merged_df["duration_ns"] = merged_df["end_ns"] - merged_df["start_ns"]

    # restore arrive_ns NaN if original had NaNs (we filled with -1 earlier)
    if "arrive_ns" in merged_df.columns:
        merged_df["arrive_ns"] = merged_df["arrive_ns"].replace(-1, pd.NA)

    # Keep consistent ordering
    merged_df = merged_df.sort_values("start_ns").reset_index(drop=True)
    return merged_df


def prepare_data(file_path, merge_gap_ns=DEFAULT_MERGE_GAP_NS, max_duration_ns=DEFAULT_MAX_DURATION_NS):
    """
    Read CSV, coerce numeric types, REMOVE oversized raw rows,
    then merge microslices, and build a color map.
    Returns: merged_df, id_to_color
    """
    df = read_and_coerce(file_path)

    # -------------------------------------------------
    # 🔥 REMOVE large rows BEFORE merging (your request)
    # -------------------------------------------------
    if max_duration_ns is not None:
        if "duration_ns" not in df.columns:
            # compute duration if not present
            df["duration_ns"] = df["end_ns"] - df["start_ns"]

        df = df[df["duration_ns"] <= max_duration_ns].reset_index(drop=True)

    # No need to merge now that the scheduler is validated and we know exactly one process runs at a time
    # df_merged = merge_slices(
    #     df,
    #     merge_gap_ns=merge_gap_ns,
    #     max_duration_ns=None  # <-- IMPORTANT: do NOT filter again after merge
    # )
    # consolidate stuff
    df_merged = consolidate_adjacent_by_pid(df, pid_col="pid")

    # Build color map
    if "child_index" in df_merged.columns:
        unique_ids = list(df_merged["child_index"].dropna().unique())
    else:
        df_merged["child_index"] = df_merged.get("pid", pd.Series([0] * len(df_merged)))
        unique_ids = list(df_merged["child_index"].dropna().unique())

    n = len(unique_ids)
    cmap_name = "tab20" if n <= 20 else "hsv"
    cmap = plt.cm.get_cmap(cmap_name, n if n > 0 else 1)
    id_to_color = {cid: cmap(i) for i, cid in enumerate(unique_ids)}

    return df_merged, id_to_color

def plot_2d_gantt(file_path, merge_gap_ns=DEFAULT_MERGE_GAP_NS, max_duration_ns=DEFAULT_MAX_DURATION_NS):
    """
    2D Gantt: horizontal bars per pid.
    Ensures all rectangles of the same PID have the same color.
    Uses merged slices.
    Highlights the first 50 ms interval.
    """
    df_merged, _ = prepare_data(
        file_path,
        merge_gap_ns=merge_gap_ns,
        max_duration_ns=max_duration_ns
    )

    fig, ax = plt.subplots(figsize=(12, 6))

    if df_merged.empty:
        ax.text(0.5, 0.5, "No data after merging/filtering",
                ha="center", va="center")
        return fig

    # -------------------------------------------------
    # Highlight first 50 ms interval
    # -------------------------------------------------
    interval_50ms_ns = 50_000_000  # 50 ms in nanoseconds

    ax.axvspan(
        0,
        interval_50ms_ns,
        color="gray",
        alpha=0.15,
        label="First 50 ms"
    )

    ax.axvline(
        interval_50ms_ns,
        color="red",
        linestyle="--",
        linewidth=1.5
    )

    # -------------------------------------------------
    # Build PID → color map
    # -------------------------------------------------
    if "pid" not in df_merged.columns:
        raise ValueError("2D plot requires a 'pid' column.")

    unique_pids = sorted(df_merged["pid"].unique())
    n = len(unique_pids)

    cmap_name = "tab20" if n <= 20 else "hsv"
    cmap = plt.cm.get_cmap(cmap_name, n)

    pid_to_color = {pid: cmap(i) for i, pid in enumerate(unique_pids)}

    # -------------------------------------------------
    # Draw bars
    # -------------------------------------------------
    for _, row in df_merged.iterrows():
        pid = row["pid"]
        start = row["start_ns"]
        width = row["end_ns"] - row["start_ns"]

        ax.barh(
            y=str(pid),
            width=width,
            left=start,
            height=0.6,
            color=pid_to_color[pid],
            edgecolor="black",
            alpha=0.9
        )

    ax.set_xlabel("Time (ns)")
    ax.set_ylabel("Process PID")
    ax.set_title("OS Scheduling Gantt Chart (merged microslices)")
    ax.grid(True, axis='x', linestyle='--', alpha=0.4)

    # -------------------------------------------------
    # Legend
    # -------------------------------------------------
    handles = [
        plt.Line2D([0], [0], color=pid_to_color[pid], lw=6)
        for pid in unique_pids
    ]
    labels = [f"PID {pid}" for pid in unique_pids]

    # Add interval legend entry
    handles.append(plt.Line2D([0], [0], color="gray", lw=6, alpha=0.3))
    labels.append("First 50 ms")

    ax.legend(handles, labels, bbox_to_anchor=(1.02, 1.0),
              loc="upper left", fontsize=8)

    plt.tight_layout()
    return fig
def plot_1d_gantt(file_path, merge_gap_ns=DEFAULT_MERGE_GAP_NS, max_duration_ns=DEFAULT_MAX_DURATION_NS):
    """
    1D timeline with stacked arrival labels. Uses merged slices.
    """
    df, id_to_color = prepare_data(file_path, merge_gap_ns=merge_gap_ns, max_duration_ns=max_duration_ns)

    # figure a bit taller to give room for stacked arrival labels
    fig, ax = plt.subplots(figsize=(9, 1.6))

    if df.empty:
        ax.text(0.5, 0.5, "No data after merging/filtering", ha="center", va="center")
        return fig

    # compute arrival time per child: prefer arrive_ns, fall back to start_ns if arrive is missing
    if "arrive_ns" in df.columns:
        df["arrive_or_start"] = df["arrive_ns"].where(df["arrive_ns"].notna(), df["start_ns"])
    else:
        df["arrive_or_start"] = df["start_ns"]

    arrival_series = df.groupby("child_index")["arrive_or_start"].min()
    # sort children by arrival time (earliest first) and assign stack indices in that order
    arrival_order = arrival_series.sort_values().index.tolist()
    arrival_stack = {cid: idx for idx, cid in enumerate(arrival_order)}

    y_level = 0
    # track deepest bottom_y we actually use (axes fraction, can be negative)
    deepest_bottom_y = 0.0

    # determine data x-limits first (so autoscale won't change due to labels)
    x_candidates = []
    for c in ("start_ns", "arrive_ns", "end_ns"):
        if c in df.columns:
            x_candidates.append(df[c].min())
            x_candidates.append(df[c].max())
    x_candidates = [x for x in x_candidates if pd.notna(x)]
    if not x_candidates:
        xmin, xmax = 0, 1
    else:
        xmin = min(x_candidates)
        xmax = max(x_candidates)

    x_margin = max(1, (xmax - xmin) * 0.01)  # small 1% margin or 1 unit
    ax.set_xlim(xmin - x_margin, xmax + x_margin)

    # draw each merged slice as a horizontal thick line and the arrival vertical line + label
    for _, row in df.iterrows():
        cid = row["child_index"]
        color = id_to_color.get(cid, (0.5, 0.5, 0.5, 1.0))

        start = int(row["start_ns"])
        end = int(row["end_ns"])

        # execution bar (data coords)
        ax.hlines(
            y=y_level,
            xmin=start,
            xmax=end,
            linewidth=16,
            color=color,
            zorder=2
        )

        # centered child number above the bar (data coords)
        mid = start + (end - start) / 2
        ax.text(
            mid,
            0.22,
            str(cid),
            ha='center',
            va='bottom',
            fontsize=9,
            fontweight='bold',
            transform=transforms.blended_transform_factory(ax.transData, ax.transAxes),
            clip_on=False,
            zorder=3
        )

        # get stack index from precomputed arrival-sorted mapping
        stack_idx = arrival_stack.get(cid, 0)

        # compute bottom in axes fraction coordinates (can be <0 to be below the axes)
        bottom_y = BASE_BOTTOM - stack_idx * STEP
        if bottom_y < MIN_BOTTOM:
            bottom_y = MIN_BOTTOM

        # remember the deepest (most negative) bottom_y used
        deepest_bottom_y = min(deepest_bottom_y, bottom_y)

        # blended transform: x in data coords, y in axes fraction coords
        blend_trans = transforms.blended_transform_factory(ax.transData, ax.transAxes)

        # draw vertical arrival line
        arrive_x = row.get("arrive_ns", pd.NA)
        if pd.isna(arrive_x):
            arrive_x = start  # fallback
        line = Line2D(
            [arrive_x, arrive_x],   # x in data coords
            [1.0, bottom_y],        # y in axes-fraction coords
            transform=blend_trans,
            linestyle="--",
            linewidth=1,
            alpha=0.5,
            color=color,
            clip_on=False,
            zorder=1
        )
        ax.add_line(line)

        # label just below the bottom of this line (in axes fraction coords)
        ax.text(
            arrive_x,
            bottom_y - LABEL_PAD,
            str(cid),
            ha='center',
            va='top',
            fontsize=7,
            fontweight='bold',
            color=color,
            transform=blend_trans,
            clip_on=False,
            zorder=4
        )

    # --- adjust figure bottom margin so the labels/lines below the axes aren't cropped ---
    safety = 0.02
    required_bottom = max(0.08, safety + (-deepest_bottom_y))  # at least 8% bottom margin
    required_bottom = min(required_bottom, 0.45)
    fig.subplots_adjust(bottom=required_bottom, top=0.82)

    ax.set_xlabel("Time (ns)")
    ax.set_yticks([])
    ax.set_title("CPU Timeline (merged microslices)")
    ax.set_ylim(-0.3, 0.5)

    # Legend (compact)
    unique_children = sorted(df["child_index"].dropna().unique())
    handles = [
        plt.Line2D([0], [0], color=id_to_color[cid], lw=6)
        for cid in unique_children
    ]
    labels = [f"Child {cid}" for cid in unique_children]

    if handles:
        ax.legend(handles, labels, bbox_to_anchor=(1.02, 1.0), loc='upper left', fontsize=7)

    return fig


def main():
    parser = argparse.ArgumentParser(description="Plot Gantt charts from scheduling CSV data (with microslice merging).")
    parser.add_argument("--input", "-i", default="test.csv",
                        help="Input CSV file")
    parser.add_argument("--output", "-o",
                        help="Output image file (e.g. output.png). If omitted, no file is saved.")
    parser.add_argument("--no-gui", action="store_true",
                        help="Do not launch the GUI display.")
    parser.add_argument("--mode", choices=["1d", "2d"], default="1d",
                        help="Select plot type: 1d or 2d (default: 1d)")

    parser.add_argument("--merge-gap", type=int, default=DEFAULT_MERGE_GAP_NS,
                        help=f"Merge microslices separated by <= this gap (ns). Default: {DEFAULT_MERGE_GAP_NS}")
    parser.add_argument("--max-duration", type=int, default=DEFAULT_MAX_DURATION_NS,
                        help="Ignore merged slices whose duration (ns) is greater than this value. Default: None (don't drop)")

    args = parser.parse_args()

    # Generate plot
    if args.mode == "1d":
        fig = plot_1d_gantt(args.input, merge_gap_ns=args.merge_gap, max_duration_ns=args.max_duration)
    else:
        fig = plot_2d_gantt(args.input, merge_gap_ns=args.merge_gap, max_duration_ns=args.max_duration)

    # Save if requested
    if args.output:
        fig.savefig(args.output, dpi=300, bbox_inches='tight', pad_inches=0.04)
        print(f"Saved plot to {args.output}")

    # Show GUI unless disabled
    if not args.no_gui:
        plt.show()
    else:
        plt.close(fig)


if __name__ == "__main__":
    main()