#!/usr/bin/env python3
"""Draws the README charts from the committed result files.

    python3 tools/make_charts.py [--results bench/results] [--out docs/img] [--label ubuntu-i5-1135G7]

Standard library only. It reads
    <label>-{o1,o2,o3,main}-w1.csv          latency and throughput, one session
    <label>-before-o1-allocs.csv            allocations, baseline engine
    <label>-after-{o1,o2,o3}-allocs.csv     allocations after each step
and writes docs/img/latency.svg and docs/img/campaign.svg. Nothing is typed in
by hand: change a CSV and rerun, and the chart follows. A missing file stops
the script with its name, never a chart with a silent gap.
"""
import argparse
import math
import os
import sys

VERSIONS = [
    ("o1", "O1  reused result, std::map levels, std::unordered_map ids", "#64748b"),
    ("o2", "O2  + flat id map", "#d97706"),
    ("o3", "O3  + sorted-array levels", "#2563eb"),
    ("main", "main  (O3 + ADR-009 changes)", "#059669"),
]
INK = "#111827"
MUTED = "#6b7280"
GRID = "#e5e7eb"
FONT = "font-family=\"-apple-system, 'Segoe UI', Helvetica, Arial, sans-serif\""


def need(path):
    if not os.path.exists(path):
        sys.exit("missing result file: " + path)
    return path


def read_w1(path):
    out = {"throughput": None, "rows": {}}
    with open(need(path)) as f:
        for line in f:
            line = line.strip()
            if line.startswith("# throughput_ops_per_sec:"):
                out["throughput"] = float(line.split(":")[1])
            elif line.startswith(("limit,", "cancel,")):
                c = line.split(",")
                out["rows"][c[0]] = {"p50": float(c[2]), "p99": float(c[3]), "p999": float(c[4])}
    if out["throughput"] is None or set(out["rows"]) != {"limit", "cancel"}:
        sys.exit("unexpected layout in " + path)
    return out


def read_allocs(path):
    with open(need(path)) as f:
        for line in f:
            if line.startswith("all,"):
                return int(line.split(",")[2])
    sys.exit("no 'all' row in " + path)


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def text(x, y, s, size=12, anchor="start", fill=INK, weight="normal"):
    return ('<text x="%.1f" y="%.1f" font-size="%d" text-anchor="%s" fill="%s" font-weight="%s" %s>%s</text>'
            % (x, y, size, anchor, fill, weight, FONT, esc(s)))


def fmt_int(v):
    return "{:,}".format(int(round(v)))


def svg_open(w, h, title, desc):
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d" role="img" '
            'aria-labelledby="t d"><title id="t">%s</title><desc id="d">%s</desc>'
            '<rect x="0.5" y="0.5" width="%d" height="%d" rx="8" fill="#ffffff" stroke="#d1d5db"/>'
            % (w, h, w, h, esc(title), esc(desc), w - 1, h - 1))


def latency_chart(w1):
    W, H = 980, 600
    parts = [svg_open(W, H, "Latency by version on workload W1",
                      "Grouped bars of p50, p99 and p99.9 latency in nanoseconds, log scale, "
                      "for limit orders and cancels, for the O1, O2, O3 and main builds.")]
    parts.append(text(W / 2, 32, "W1 latency by build (lower is better), log scale", 18, "middle", weight="bold"))
    parts.append(text(W / 2, 52, "Pinned core 3, i5-1135G7, turbo off, GCC 15.2. One run per build, one session. "
                      "Run-to-run spread about 1.5% at p50 and p99, 8% at p99.9.", 11, "middle", MUTED))

    lo, hi = 100.0, 5000.0
    ticks = [100, 200, 500, 1000, 2000, 5000]
    top, bottom = 100, 430
    panels = [("limit", "Limit orders", 70, 480), ("cancel", "Cancels", 530, 940)]

    def ypos(v):
        return bottom - (math.log10(v) - math.log10(lo)) / (math.log10(hi) - math.log10(lo)) * (bottom - top)

    for key, title, x0, x1 in panels:
        parts.append(text((x0 + x1) / 2, 84, title, 14, "middle", weight="bold"))
        for t in ticks:
            y = ypos(t)
            parts.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="%s"/>' % (x0, y, x1, y, GRID))
            if key == "limit":
                parts.append(text(x0 - 8, y + 4, fmt_int(t), 10, "end", MUTED))
        parts.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s"/>' % (x0, bottom, x1, bottom, MUTED))
        groups = [("p50", "p50"), ("p99", "p99"), ("p999", "p99.9")]
        gw = (x1 - x0) / len(groups)
        bw = 24
        gap = 3
        used = len(VERSIONS) * bw + (len(VERSIONS) - 1) * gap
        for gi, (mk, ml) in enumerate(groups):
            gx = x0 + gi * gw + (gw - used) / 2
            for vi, (vk, _, color) in enumerate(VERSIONS):
                v = w1[vk]["rows"][key][mk]
                x = gx + vi * (bw + gap)
                y = ypos(v)
                parts.append('<rect x="%.1f" y="%.1f" width="%d" height="%.1f" fill="%s"/>'
                             % (x, y, bw, bottom - y, color))
                parts.append(text(x + bw / 2, y - 4, "%d" % round(v), 9, "middle", INK))
            parts.append(text(x0 + gi * gw + gw / 2, bottom + 18, ml, 12, "middle", INK, "bold"))
    parts.append(text(28, (top + bottom) / 2, "nanoseconds (log scale)", 11, "middle", MUTED).replace(
        'x="28.0"', 'x="22.0" transform="rotate(-90 22 %d)"' % ((top + bottom) / 2)))

    ly = 478
    for i, (vk, label, color) in enumerate(VERSIONS):
        col, row = i % 2, i // 2
        x = 70 + col * 440
        y = ly + row * 24
        parts.append('<rect x="%d" y="%d" width="14" height="14" fill="%s"/>' % (x, y, color))
        parts.append(text(x + 22, y + 12, label, 12))
    parts.append(text(W / 2, 574, "The p99.9 jump of about 4x arrives with O2 (flat id map) and is unchanged by O3. "
                      "Cause not yet established.", 12, "middle", INK))
    parts.append("</svg>")
    return "\n".join(parts)


def campaign_chart(w1, allocs):
    W, H = 980, 470
    parts = [svg_open(W, H, "Allocations and throughput across the optimization campaign",
                      "Left: heap allocations per million commands for the baseline, O1, O2 and O3. "
                      "Right: batched throughput in operations per second for O1, O2, O3 and main.")]
    parts.append(text(W / 2, 32, "Optimization campaign on W1: allocations and throughput", 18, "middle", weight="bold"))

    # left: allocations
    x0, x1, top, bottom = 80, 450, 110, 360
    parts.append(text((x0 + x1) / 2, 84, "Heap allocations per 1,000,000 commands (lower is better)", 13, "middle", weight="bold"))
    names = [("Baseline", allocs["base"]), ("O1", allocs["o1"]), ("O2", allocs["o2"]), ("O3", allocs["o3"])]
    peak = max(v for _, v in names)
    scale_max = 1400000
    for t in (0, 400000, 800000, 1200000):
        y = bottom - t / scale_max * (bottom - top)
        parts.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="%s"/>' % (x0, y, x1, y, GRID))
        parts.append(text(x0 - 8, y + 4, "0" if t == 0 else "%dk" % (t // 1000), 10, "end", MUTED))
    bw = 56
    step = (x1 - x0) / len(names)
    for i, (n, v) in enumerate(names):
        x = x0 + i * step + (step - bw) / 2
        h = v / scale_max * (bottom - top)
        parts.append('<rect x="%.1f" y="%.1f" width="%d" height="%.1f" fill="#2563eb"/>' % (x, bottom - h, bw, h))
        parts.append(text(x + bw / 2, bottom - h - 6, fmt_int(v), 11, "middle", INK, "bold"))
        parts.append(text(x + bw / 2, bottom + 18, n, 12, "middle", INK, "bold"))
    cuts = ["", "trades vector reused", "flat id map", "sorted-array levels"]
    for i, c in enumerate(cuts):
        if c:
            x = x0 + i * step + step / 2
            parts.append(text(x, bottom + 34, c, 10, "middle", MUTED))
    parts.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s"/>' % (x0, bottom, x1, bottom, MUTED))

    # right: throughput
    x0, x1 = 560, 930
    parts.append(text((x0 + x1) / 2, 84, "Batched throughput, million ops/s (higher is better)", 13, "middle", weight="bold"))
    tmax = 7.0
    for t in (0, 2, 4, 6):
        y = bottom - t / tmax * (bottom - top)
        parts.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="%s"/>' % (x0, y, x1, y, GRID))
        parts.append(text(x0 - 8, y + 4, "%d" % t, 10, "end", MUTED))
    step = (x1 - x0) / len(VERSIONS)
    for i, (vk, _, color) in enumerate(VERSIONS):
        v = w1[vk]["throughput"] / 1e6
        x = x0 + i * step + (step - bw) / 2
        h = v / tmax * (bottom - top)
        parts.append('<rect x="%.1f" y="%.1f" width="%d" height="%.1f" fill="%s"/>' % (x, bottom - h, bw, h, color))
        parts.append(text(x + bw / 2, bottom - h - 6, "%.2f" % v, 11, "middle", INK, "bold"))
        parts.append(text(x + bw / 2, bottom + 18, vk.upper() if vk != "main" else "main", 12, "middle", INK, "bold"))
    parts.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s"/>' % (x0, bottom, x1, bottom, MUTED))

    parts.append(text(W / 2, 428, "Allocation counts are exact and machine independent. Throughput is one pinned run per build in one session, "
                      "and a control run in that session was disturbed, so a few percent is indicative only.", 11, "middle", MUTED))
    parts.append(text(W / 2, 448, "Per-command latency is in latency.svg, where O3 improves median and p99 while throughput dips.", 11, "middle", MUTED))
    parts.append("</svg>")
    return "\n".join(parts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default="bench/results")
    ap.add_argument("--out", default="docs/img")
    ap.add_argument("--label", default="ubuntu-i5-1135G7")
    a = ap.parse_args()

    def p(suffix):
        return os.path.join(a.results, "%s-%s" % (a.label, suffix))

    w1 = {k: read_w1(p("%s-w1.csv" % k)) for k, _, _ in VERSIONS}
    allocs = {"base": read_allocs(p("before-o1-allocs.csv")), "o1": read_allocs(p("after-o1-allocs.csv")),
              "o2": read_allocs(p("after-o2-allocs.csv")), "o3": read_allocs(p("after-o3-allocs.csv"))}
    os.makedirs(a.out, exist_ok=True)
    for name, body in (("latency.svg", latency_chart(w1)), ("campaign.svg", campaign_chart(w1, allocs))):
        with open(os.path.join(a.out, name), "w") as f:
            f.write(body + "\n")
        print("wrote", os.path.join(a.out, name))


if __name__ == "__main__":
    main()
