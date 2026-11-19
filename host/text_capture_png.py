#!/usr/bin/env python3
"""Render a text console capture to a terminal-style PNG (no editing).

    python3 host/text_capture_png.py docs/results/captures/pump-C-alarm.txt
"""
import sys


def render(path, out=None):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    lines = open(path, errors="replace").read().rstrip("\n").split("\n")
    width = max(len(l) for l in lines)
    fig = plt.figure(figsize=(max(6, width * 0.075), 0.19 * len(lines) + 0.4))
    fig.patch.set_facecolor("#111111")
    for i, l in enumerate(lines):
        col = "#7f7f7f" if l.startswith("#") else (
            "#ff6b6b" if ("SAFE STATE" in l or "ALARM" in l) else
            "#ffd166" if ("THROTTLED" in l or "ESCALATED" in l) else "#e6e6e6")
        fig.text(0.01, 1 - (i + 1) / (len(lines) + 1), l, family="monospace",
                 fontsize=8.5, color=col, va="center")
    out = out or path[:-4] + ".png"
    fig.savefig(out, dpi=110, facecolor=fig.get_facecolor())
    print("wrote", out)


if __name__ == "__main__":
    for p in sys.argv[1:]:
        render(p)
