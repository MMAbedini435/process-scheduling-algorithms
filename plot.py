import argparse
import pandas as pd
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib import transforms

# tuning params
BASE_BOTTOM = -0.03     # axes-fraction for first level bottom (just below axes)
STEP = 0.15            # how much lower each next stack goes (in axes fraction)
MIN_BOTTOM = -3      # don't go lower than this (axes fraction)
LABEL_PAD = 0.01        # extra gap below the line for the label (axes fraction)

def prepare_data(file_path):
    """
    Read CSV, coerce numeric types, sort and build a color map for child_index.
    Returns: df, id_to_color
    """
    df = pd.read_csv(file_path)

    # ensure numeric types and reset index
    for col in ("arrive_ns", "start_ns", "end_ns", "duration_ns"):
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce").astype("Int64")

    df = df.sort_values("start_ns").reset_index(drop=True)

    # color map choice (tab20 for <=20, otherwise hsv)
    unique_ids = list(df["child_index"].unique())
    n = len(unique_ids)
    cmap_name = "tab20" if n <= 20 else "hsv"
    cmap = plt.cm.get_cmap(cmap_name, n)

    id_to_color = {cid: cmap(i) for i, cid in enumerate(unique_ids)}
    return df, id_to_color


def plot_2d_gantt(file_path):
    df = pd.read_csv(file_path)
    df = df.sort_values(by="start_ns")

    fig, ax = plt.subplots(figsize=(12, 6))

    for _, row in df.iterrows():
        ax.barh(
            y=str(row["pid"]),
            width=row["end_ns"] - row["start_ns"],
            left=row["start_ns"]
        )

    ax.set_xlabel("Time (ns)")
    ax.set_ylabel("Process PID")
    ax.set_title("OS Scheduling Gantt Chart")
    ax.grid(True)

    plt.tight_layout()
    return fig


def plot_1d_gantt(file_path):
    df, id_to_color = prepare_data(file_path)

    # figure a bit taller to give room for stacked arrival labels
    fig, ax = plt.subplots(figsize=(9, 1.6))

    # compute arrival time per child: prefer arrive_ns, fall back to start_ns if arrive is missing
    df["arrive_or_start"] = df["arrive_ns"].where(df["arrive_ns"].notna(), df["start_ns"])
    arrival_series = df.groupby("child_index")["arrive_or_start"].min()
    # sort children by arrival time (earliest first) and assign stack indices in that order
    arrival_order = arrival_series.sort_values().index.tolist()
    arrival_stack = {cid: idx for idx, cid in enumerate(arrival_order)}

    y_level = 0
    # track deepest bottom_y we actually use (axes fraction, can be negative)
    deepest_bottom_y = 0.0

    # determine data x-limits first (so autoscale won't change due to labels)
    x_min = df[["start_ns", "arrive_ns"]].min().min()
    x_max = df[["end_ns", "arrive_ns"]].max().max()
    x_margin = max(1, (x_max - x_min) * 0.01)  # small 1% margin or 1 unit
    ax.set_xlim(x_min - x_margin, x_max + x_margin)

    for _, row in df.iterrows():
        cid = row["child_index"]
        color = id_to_color[cid]

        start = row["start_ns"]
        end = row["end_ns"]

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
        line = Line2D(
            [row["arrive_ns"], row["arrive_ns"]],   # x in data coords
            [1.0, bottom_y],                        # y in axes-fraction coords
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
            row["arrive_ns"],
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
    ax.set_title("CPU Timeline")
    ax.set_ylim(-0.3, 0.5)

    handles = [
        plt.Line2D([0], [0], color=id_to_color[cid], lw=6)
        for cid in sorted(df["child_index"].unique())
    ]
    labels = [f"Child {cid}" for cid in sorted(df["child_index"].unique())]

    return fig

def main():
    parser = argparse.ArgumentParser(description="Plot Gantt charts from scheduling CSV data.")
    parser.add_argument("--input", "-i", default="test.csv",
                        help="Input CSV file")
    parser.add_argument("--output", "-o",
                        help="Output image file (e.g. output.png). If omitted, no file is saved.")
    parser.add_argument("--no-gui", action="store_true",
                        help="Do not launch the GUI display.")
    parser.add_argument("--mode", choices=["1d", "2d"], default="1d",
                        help="Select plot type: 1d or 2d (default: 1d)")

    args = parser.parse_args()

    # Generate plot
    if args.mode == "1d":
        fig = plot_1d_gantt(args.input)
    else:
        fig = plot_2d_gantt(args.input)

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