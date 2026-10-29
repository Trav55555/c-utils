"""Prints where a dcheck run spent its time, for the CI job log, which outlives
the uploaded artifacts: each check's total from timing.tsv, and the slowest
semantics files from semantics.log with any at half their limit or more.

Usage: ci/timings.py LOGS_DIR"""
import re
import sys
from pathlib import Path

FILE_TIME = re.compile(r"^DCHECK: (\S+) took (\d+) s of (\d+) s$")
SLOWEST = 10


def check_totals(text):
    """(check, seconds) for each check's total row, in run order."""
    totals = []
    for line in text.splitlines():
        cols = line.split("\t")
        if len(cols) == 4 and cols[1] == "total" and cols[2].isdigit():
            totals.append((cols[0], int(cols[2]) / 1e6))
    return totals


def file_times(text):
    """(file, seconds, limit) for each semantics file, slowest first."""
    times = []
    for line in text.splitlines():
        m = FILE_TIME.match(line)
        if m:
            times.append((m.group(1), int(m.group(2)), int(m.group(3))))
    return sorted(times, key=lambda t: -t[1])


def report(logs):
    lines = []
    timing = logs / "timing.tsv"
    if timing.is_file():
        totals = check_totals(timing.read_text(errors="replace"))
        if totals:
            lines.append("check times: " + ", ".join(f"{name} {secs:.0f} s" for name, secs in totals))
    semantics = logs / "semantics.log"
    if semantics.is_file():
        times = file_times(semantics.read_text(errors="replace"))
        if times:
            lines.append(f"slowest semantics files ({len(times)} timed):")
            lines += [f"  {secs} s of {limit} s  {name}" for name, secs, limit in times[:SLOWEST]]
            near = [name for name, secs, limit in times if 2 * secs >= limit]
            if near:
                lines.append("at half their limit or more: " + ", ".join(near))
    return lines


def main(argv):
    if len(argv) != 2:
        sys.exit(__doc__.split("Usage: ")[1])
    for line in report(Path(argv[1])):
        print(line)


if __name__ == "__main__":
    main(sys.argv)
