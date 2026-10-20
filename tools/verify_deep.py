#!/usr/bin/env python3
"""Run a checker in a bounded Linux user scope. Never fall back to process groups."""
import argparse
from pathlib import Path, PurePosixPath
import signal
import subprocess
import sys
import time
import uuid


def scope_command(unit, seconds, command):
    return ["systemd-run", "--user", "--scope", "--quiet", "--collect",
            "--unit=" + unit, "-p", "MemoryMax=3G", "-p", "MemorySwapMax=0",
            "-p", "RuntimeMaxSec=" + str(seconds), "-p", "KillSignal=SIGKILL",
            "--", *command]


def populated(unit):
    result = subprocess.run(
        ["systemctl", "--user", "show", unit, "-p", "LoadState", "-p", "ControlGroup"],
        capture_output=True, text=True, timeout=10)
    properties = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
    if properties.get("LoadState") == "not-found":
        return False
    if result.returncode or "ControlGroup" not in properties:
        raise RuntimeError("cannot inspect verification scope")
    group = properties["ControlGroup"]
    if not group:
        return False
    path = PurePosixPath(group)
    if not path.is_absolute() or ".." in path.parts:
        raise RuntimeError("invalid verification cgroup path")
    try:
        events = (Path("/sys/fs/cgroup") / group.lstrip("/") / "cgroup.events").read_text()
    except FileNotFoundError:
        return False  # A populated cgroup cannot be removed.
    values = dict(line.split() for line in events.splitlines())
    if values.get("populated") not in {"0", "1"}:
        raise RuntimeError("cannot read verification cgroup population")
    return values["populated"] == "1"


def cleanup_scope(unit):
    try:
        leftover = populated(unit)
    finally:
        # Stop even when inspection failed. An already-collected empty scope
        # may reject stop; only a verified empty cgroup permits success.
        stopped = subprocess.run(["systemctl", "--user", "stop", unit],
                                 capture_output=True, text=True, timeout=10)
        if populated(unit):
            stopped.check_returncode()
            raise RuntimeError("verification scope still has processes after stop")
    return leftover


def run(command, seconds):
    unit = "c-utils-deep-" + uuid.uuid4().hex + ".scope"
    deadline = time.monotonic() + seconds
    result = 1
    child = None
    cancelled = 0
    handlers = {}
    def interrupt(signum, _frame):
        nonlocal cancelled
        if not cancelled:
            cancelled = signum
    try:
        # Recording signals, rather than raising inside Popen or cleanup, keeps
        # ownership publication and scope teardown uninterrupted. Child signal
        # masks remain unchanged; caught dispositions reset normally on exec.
        for sig in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
            # Preserve nohup's inherited ignore; INT/TERM retain the runner's
            # existing explicit-cancellation policy.
            if sig == signal.SIGHUP and signal.getsignal(sig) == signal.SIG_IGN:
                continue
            handlers[sig] = signal.signal(sig, interrupt)
        try:
            # Preserve caller-owned inheritable descriptors, including CI's
            # flock; CLOEXEC descriptors still close.
            child = subprocess.Popen(scope_command(unit, seconds, command), close_fds=False)
            while not cancelled:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    print("verification reached its aggregate deadline", file=sys.stderr)
                    result = 124
                    break
                try:
                    result = child.wait(timeout=min(remaining, 0.1))
                    break
                except subprocess.TimeoutExpired:
                    pass
        finally:
            if child is not None:
                try:
                    if child.poll() is None:
                        child.kill()
                    child.wait(timeout=10)
                finally:
                    if cleanup_scope(unit) and result == 0:
                        print("verification left processes running; scope stopped", file=sys.stderr)
                        result = 1
    finally:
        for sig, handler in handlers.items():
            signal.signal(sig, handler)
    if cancelled:
        return 128 + cancelled
    return result if result >= 0 else 128 - result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=int, default=1800)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.command[:1] == ["--"]:
        args.command = args.command[1:]
    if args.seconds <= 0 or not args.command:
        parser.error("positive --seconds and a command are required")
    try:
        return run(args.command, args.seconds)
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print("verification runner: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
