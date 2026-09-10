#!/usr/bin/env python3
"""
Measure trigger-to-response latency from a Wokwi logic analyzer VCD capture.

Pairs each rising edge on the trigger channel with the next rising edge on the
response channel, and reports the latency distribution.

Usage:
    python vcd_latency.py wokwi-logic.vcd
    python vcd_latency.py wokwi-logic.vcd --trigger D0 --response D1 --csv out.csv
"""

import argparse
import re
import statistics
import sys

# VCD timescale units expressed in nanoseconds.
UNIT_NS = {"s": 1e9, "ms": 1e6, "us": 1e3, "ns": 1.0, "ps": 1e-3, "fs": 1e-6}


def parse_vcd(path):
    """Return (timescale_ns, {signal_name: [(time_ns, value), ...]})."""
    timescale_ns = 1.0
    ids = {}          # short id -> signal name
    changes = {}      # signal name -> list of (time, value)
    now = 0
    in_header = True

    with open(path, "r", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue

            if in_header:
                if line.startswith("$timescale"):
                    m = re.search(r"(\d+)\s*([munpf]?s)", line)
                    if m:
                        timescale_ns = int(m.group(1)) * UNIT_NS[m.group(2)]
                elif line.startswith("$var"):
                    # $var wire 1 ! D0 $end
                    parts = line.split()
                    if len(parts) >= 5:
                        short_id, name = parts[3], parts[4]
                        ids[short_id] = name
                        changes[name] = []
                elif line.startswith("$enddefinitions"):
                    in_header = False
                continue

            if line.startswith("#"):
                now = int(line[1:])
            elif line[0] in "01xzXZ" and len(line) > 1:
                value, short_id = line[0], line[1:]
                name = ids.get(short_id)
                if name:
                    changes[name].append((now, value))

    return timescale_ns, changes


def rising_edges(series):
    """Times at which a signal transitions from 0 to 1."""
    edges = []
    previous = "0"
    for time, value in series:
        if previous == "0" and value == "1":
            edges.append(time)
        previous = value
    return edges


def summarize(label, values, unit):
    if not values:
        print(f"{label}: no samples")
        return
    ordered = sorted(values)
    p99 = ordered[min(len(ordered) - 1, int(round(0.99 * (len(ordered) - 1))))]
    print(f"{label}  (n = {len(values)})")
    print(f"  min    {min(ordered):10.3f} {unit}")
    print(f"  mean   {statistics.fmean(ordered):10.3f} {unit}")
    print(f"  median {statistics.median(ordered):10.3f} {unit}")
    print(f"  p99    {p99:10.3f} {unit}")
    print(f"  max    {max(ordered):10.3f} {unit}")
    if len(ordered) > 1:
        print(f"  stdev  {statistics.stdev(ordered):10.3f} {unit}")
    print()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("vcd")
    ap.add_argument("--trigger", default="D0")
    ap.add_argument("--response", default="D1")
    ap.add_argument("--csv", help="write per-event latencies here")
    args = ap.parse_args()

    timescale_ns, changes = parse_vcd(args.vcd)

    for name in (args.trigger, args.response):
        if name not in changes:
            print(f"signal '{name}' not in capture. found: {sorted(changes)}")
            return 1

    trig = rising_edges(changes[args.trigger])
    resp = rising_edges(changes[args.response])

    if not trig:
        print(f"no rising edges on {args.trigger} -- check the wiring")
        return 1

    # Trigger period, as a check that the reference clock is what you think.
    periods_us = [(b - a) * timescale_ns / 1000.0 for a, b in zip(trig, trig[1:])]

    # Pair each trigger with the next response that follows it.
    latencies_us = []
    missed = 0
    r = 0
    for i, t in enumerate(trig):
        limit = trig[i + 1] if i + 1 < len(trig) else float("inf")
        while r < len(resp) and resp[r] < t:
            r += 1
        if r < len(resp) and resp[r] < limit:
            latencies_us.append((resp[r] - t) * timescale_ns / 1000.0)
            r += 1
        else:
            missed += 1

    print(f"\nfile: {args.vcd}   timescale: {timescale_ns:g} ns\n")
    summarize("trigger period", periods_us, "us")
    summarize("latency", latencies_us, "us")

    if missed:
        pct = 100.0 * missed / len(trig)
        print(f"unanswered triggers: {missed} of {len(trig)}  ({pct:.3f}%)\n")
    else:
        print(f"every one of {len(trig)} triggers got a response\n")

    if args.csv:
        with open(args.csv, "w") as fh:
            fh.write("event,latency_us\n")
            for i, value in enumerate(latencies_us):
                fh.write(f"{i},{value:.4f}\n")
        print(f"wrote {args.csv}")

    return 0


if __name__ == "__main__":
    sys.exit(main())