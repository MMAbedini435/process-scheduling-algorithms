import pandas as pd
import matplotlib.pyplot as plt

# Load CSV
df = pd.read_csv("input.csv")

# Sort by child_index for cleaner visualization
df = df.sort_values("child_index")

fig, ax = plt.subplots()

for _, row in df.iterrows():
    process = row["child_index"]
    start = row["start_ns"]
    end = row["end_ns"]
    arrive = row["arrive_ns"]

    # Draw Gantt bar (no specific colors)
    ax.barh(process, end - start, left=start)

    # Draw arrival arrow
    ax.annotate(
        "",
        xy=(arrive, process),
        xytext=(arrive, process + 0.3),
        arrowprops=dict(arrowstyle="->")
    )

ax.set_xlabel("Time (ns)")
ax.set_ylabel("Process (child_index)")
ax.set_title("Process Gantt Chart")

plt.show()