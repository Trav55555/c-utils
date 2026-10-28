#!/usr/bin/env python3
"""Fault controls: show that the checks for a lifetime or failure rule notice
when the rule is broken.

Each control copies the sources to a scratch directory, applies one known
fault there (never to the checkout), and runs the focused test, fuzz input or
CBMC proof that should notice it. A control passes only when that check fails
with exactly the expected contract message; a build error, a timeout, another
failure or no failure fails the run. A control can also record a check that
cannot reach its fault, which must then still pass. The unmodified copy must
pass every check first. docs/FAULT-CONTROLS.md records what each one shows.

The fuzz and proof checks build as dcheck does (check_fuzz, check_cbmc), with
dcheck.conf's PROJECT_DEFINES, CBMC_UNWIND and CBMC_OBJECT_BITS.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
COPIED = ["include", "src", "tests", "fuzz", "proofs", "Makefile", "dcheck.conf"]
BUILD_SECONDS = 300
TEST_SECONDS = 30      # dcheck's TEST_TIMEOUT
CBMC_SECONDS = 600     # dcheck's CBMC_TIMEOUT default

# A check is (kind, target, argument): a test binary built by the Makefile, a
# fuzz harness run once on fixed input bytes, or a CBMC proof harness.
SCOPES = ("test", "test_scopes", None)
ARENA = ("test", "test_arena", None)
POOL = ("test", "test_pool", None)
PLEX = ("test", "test_plex", None)
# max_gen 2; add, remove it, add again: the slot is reused under generation 2.
FUZZ_POOL = ("fuzz", "fuzz_pool", b"\x02\x01\x00\x01")
GROW = ("cbmc", "proof_arena_grow", None)

FRESH_BASE = """  /* A new base scope: open temp scopes, and containers made before now,
   * no longer match, so ending or growing one aborts. */
  a->scope = next_scope_id(a);
"""
# Returning to init's base scope id, 1, is the rule 63f0bb1 added, undone.
KEEP_BASE = "  a->scope = 1;\n"
IDS_ASSERT = "    assert(cu_arena_scope(&a) != base && cu_arena_scope(&a) != outer.id);\n"
INJECTED = """  extra = size - a->head->cap;
  if (a->attempts++ == a->fail_at) {
    a->failed = 1;
"""
LIMIT = """    a->refused = extra;
    a->failed = 1;
"""
LIMIT_ASSERT = "cu_arena_failed(&a) && a.refused == 200 - 64 && a.held == held && has_pattern(p, 64)"
REALLOC = """  b = (CuBlock *)realloc(a->head, total);
  if (b == NULL) {
    a->failed = 1;
    return NULL;
  }
  a->held += extra;
"""


def realloc_fault(new):
    return [("src/cu_arena.c", REALLOC, new)]


def failure_branch(statement):
    return realloc_fault(REALLOC.replace("    a->failed = 1;\n", "    a->failed = 1;\n" + statement))


def assertion(test, function, expression):
    return ("tests/%s.c:" % test, "%s: Assertion `%s' failed." % (function, expression))


# name -> (what the fault does, [(file, old text, new text)], [(check, expected)]).
# A test's or fuzz input's expected failure is text that must all appear in its
# output, the first piece on the line shown; a proof's is exactly the set of
# property descriptions that fail. None means the check must still pass: it
# cannot reach the fault, and the control records that.
CONTROLS = {
    "release-keeps-base-scope": (
        "cu_arena_release returns to init's base scope instead of a fresh one",
        [("src/cu_arena.c", FRESH_BASE, KEEP_BASE)],
        [(SCOPES, assertion("test_scopes", "test_scope_ids_nest",
                            "cu_arena_scope(&a) != base && cu_arena_scope(&a) != outer.id"))]),
    "release-keeps-base-scope-stale-vector": (
        "the same fault, with test_scope_ids_nest's id check removed, so a stale container must be caught",
        [("src/cu_arena.c", FRESH_BASE, KEEP_BASE), ("tests/test_scopes.c", IDS_ASSERT, "")],
        [(SCOPES, ("expect_abort: vec_push_after_release exited with status 0",
                   "expect_abort_named: Assertion `WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT' failed."))]),
    "pool-ignores-generation": (
        "a pool lookup accepts a live slot whatever the handle's generation",
        [("src/cu_pool.c", "if (s->next_free != CU_LIVE || s->gen != h.gen)", "if (s->next_free != CU_LIVE)")],
        [(POOL, assertion("test_pool", "test_reuse_bumps_generation", "CU_POOL_GET(&p, long, old) == NULL")),
         (PLEX, assertion("test_plex", "test_removed_node_links_go_stale",
                          "CU_PLEX_GET(&px, Person, person, y) == NULL")),
         (FUZZ_POOL, ("ERROR: libFuzzer: deadly signal",))]),
    "grow-failure-unmarked": (
        "a failed realloc in cu_arena_grow does not mark the arena failed",
        realloc_fault(REALLOC.replace("    a->failed = 1;\n", "")),
        [(GROW, {"a failed grow marks the arena"}), (ARENA, None)]),
    "grow-failure-counts-held": (
        "held grows by the added bytes before realloc, so also when it fails",
        realloc_fault("  a->held += extra;\n" + REALLOC.replace("  a->held += extra;\n", "")),
        [(GROW, {"a failed grow holds nothing more", "release returns every held byte"}), (ARENA, None)]),
    "grow-failure-clears-bytes": (
        "a failed realloc clears the old block's bytes",
        failure_branch("    memset(block_data(a->head), 0, a->head->used);\n"),
        [(GROW, {"a failed grow keeps the old bytes"}), (ARENA, None)]),
    "grow-failure-clears-one-byte": (
        "a failed realloc clears byte 1 only, which a check of the first and last bytes misses",
        failure_branch("    block_data(a->head)[1] = 0;\n"),
        [(GROW, {"a failed grow keeps the old bytes"}), (ARENA, None)]),
    "grow-success-clears-one-byte": (
        "a successful realloc clears byte 1 of the moved bytes",
        realloc_fault(REALLOC + "  block_data(b)[1] = 0;\n"),
        [(GROW, {"the bytes move with the grow"}),
         (ARENA, assertion("test_arena", "test_grow_reallocates_own_block", "q != NULL && has_pattern(q, 64)"))]),
    "grow-failure-drops-block": (
        "the old block is replaced by realloc's NULL result (p = realloc(p, n)), so the arena loses it",
        realloc_fault(REALLOC.replace("  b = (CuBlock *)realloc(a->head, total);\n",
                                      "  a->head = (CuBlock *)realloc(a->head, total);\n  b = a->head;\n")),
        [(GROW, {"release returns every held byte"}), (ARENA, None)]),
    "grow-injected-failure-unmarked": (
        "cu_arena_fail_at's refusal of a reallocation does not mark the arena failed",
        [("src/cu_arena.c", INJECTED, INJECTED.replace("    a->failed = 1;\n", ""))],
        [(ARENA, assertion("test_arena", "test_grow_failure",
                           "cu_arena_failed(&a) && a.held == held && has_pattern(p, 64)")), (GROW, None)]),
    "grow-limit-refusal-unmarked": (
        "the limit's refusal of a reallocation does not mark the arena failed",
        [("src/cu_arena.c", LIMIT, LIMIT.replace("    a->failed = 1;\n", ""))],
        [(ARENA, assertion("test_arena", "test_grow_failure", LIMIT_ASSERT)), (GROW, None)]),
    "grow-limit-refusal-unrecorded": (
        "the limit's refusal of a reallocation does not record the refused bytes",
        [("src/cu_arena.c", LIMIT, LIMIT.replace("    a->refused = extra;\n", ""))],
        [(ARENA, assertion("test_arena", "test_grow_failure", LIMIT_ASSERT)), (GROW, None)]),
}


def conf(root):
    """dcheck.conf's settings, with dcheck's defaults, read as dcheck reads them."""
    script = ('PROJECT_DEFINES=""; CBMC_UNWIND=8; CBMC_OBJECT_BITS=8; . ./dcheck.conf; '
              'printf "%s\\n%s\\n%s\\n" "$PROJECT_DEFINES" "$CBMC_UNWIND" "$CBMC_OBJECT_BITS"')
    out = subprocess.run(["bash", "-c", script], cwd=root, capture_output=True, text=True, check=True).stdout
    defines, unwind, bits = out.split("\n")[:3]
    return ["-D" + d for d in defines.split()], unwind, bits


def apply(copy, patches):
    for name, old, new in patches:
        path = copy / name
        text = path.read_text()
        if text.count(old) != 1:
            raise SystemExit("stale control: %s no longer contains the faulted text once" % name)
        path.write_text(text.replace(old, new))


def run(cmd, cwd, seconds):
    """(status, output, seconds, peak KiB): status is the exit code, -signal, or
    None on timeout. The whole process group is killed at the deadline. A
    missing program is status 127, as in the shell."""
    with tempfile.TemporaryFile() as out:
        start = time.monotonic()
        try:
            proc = subprocess.Popen(cmd, cwd=cwd, stdout=out, stderr=subprocess.STDOUT, start_new_session=True)
        except FileNotFoundError:
            return 127, "%s: not found" % cmd[0], 0, 0
        status = None
        while True:
            pid, raw, usage = os.wait4(proc.pid, os.WNOHANG)
            if pid:
                status = os.waitstatus_to_exitcode(raw)
                proc.returncode = status
                break
            if time.monotonic() - start > seconds:
                os.killpg(proc.pid, signal.SIGKILL)
                os.wait4(proc.pid, 0)
                proc.returncode = -signal.SIGKILL
                usage = None
                break
            time.sleep(0.02)
        elapsed = time.monotonic() - start
        out.seek(0)
        text = out.read().decode(errors="replace")
    return status, text, elapsed, usage.ru_maxrss if usage else 0


def build_and_run(check, copy, opts):
    """(verdict, detail, seconds, peak KiB) for one check on one copy, where
    verdict is pass, fail (the check reported a failure) or broken."""
    kind, target, data = check
    if kind == "test":
        status, out, _, _ = run(["make", "-s", "build/" + target], copy, BUILD_SECONDS)
        if status != 0:
            return "broken", "build failed: " + tail(out), 0, 0
        status, out, seconds, peak = run(["build/" + target], copy, TEST_SECONDS)
    elif kind == "fuzz":
        binary = "build/" + target
        cmd = [opts.clang, "-std=c99", "-g", "-O1", "-fsanitize=fuzzer,address,undefined",
               "-fno-sanitize-recover=all", *opts.defines, "-I", "include", "-I", str(opts.lib),
               "-o", binary, "fuzz/%s.c" % target, *sources(copy), "-lm"]
        (copy / "build").mkdir(exist_ok=True)
        status, out, _, _ = run(cmd, copy, BUILD_SECONDS)
        if status != 0:
            return "broken", "build failed: " + tail(out), 0, 0
        (copy / "build" / "input").write_bytes(data)
        status, out, seconds, peak = run([binary, "build/input"], copy, TEST_SECONDS)
    else:
        harness = copy / "proofs" / (target + ".c")
        if "dcheck-unwindset:" in harness.read_text():
            return "broken", "the harness sets dcheck-unwindset, which this runner does not apply", 0, 0
        cmd = ["cbmc", "proofs/%s.c" % target, *sources(copy), "-I", "include", "-I", str(opts.lib),
               "--bounds-check", "--pointer-check", "--signed-overflow-check", "--pointer-overflow-check",
               "--undefined-shift-check", "--div-by-zero-check", "--memory-leak-check",
               "--unwind", opts.unwind, "--unwinding-assertions", "--object-bits", opts.object_bits,
               "--trace", "--compact-trace"]
        status, out, seconds, peak = run(cmd, copy, CBMC_SECONDS)
        failed = re.findall(r"^\[[^]]+\] (?:line \d+ )?(.*): FAILURE$", out, re.M)
        if status == 0 and "VERIFICATION SUCCESSFUL" in out:
            return "pass", "", seconds, peak
        if status == 10 and failed:
            return "fail", set(failed), seconds, peak
        # A limit, a parse error or a timeout: CBMC checked nothing.
        return "broken", "cbmc stopped (%s): %s" % (status, tail(out)), seconds, peak
    if status is None:
        return "broken", "timed out", seconds, peak
    if status == 0:
        return "pass", "", seconds, peak
    return "fail", out, seconds, peak


def sources(copy):
    return sorted(str(p.relative_to(copy)) for p in (copy / "src").glob("*.c"))


def tail(out):
    lines = [line for line in out.splitlines() if line.strip()]
    return lines[-1] if lines else "(no output)"


def matches(check, detail, expected):
    """The line that shows the expected failure, or None."""
    if check[0] == "cbmc":
        return ", ".join(sorted(detail)) if detail == expected else None
    if not all(piece in detail for piece in expected):
        return None
    return next(line for line in detail.splitlines() if expected[0] in line).strip()


def measure(seconds, peak):
    return "%.1fs, %d MiB" % (seconds, (peak + 1023) // 1024) if seconds else ""


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--dcheck", required=True, help="dcheck executable (its lib/ is on the include path)")
    parser.add_argument("--root", type=Path, default=ROOT, help="source tree to copy (default: this checkout)")
    parser.add_argument("--only", action="append", choices=sorted(CONTROLS), help="run only this control")
    parser.add_argument("--work", type=Path, help="scratch directory, kept afterwards (default: a removed temp dir)")
    parser.add_argument("--clang", default=os.environ.get("DCHECK_CLANG", "clang"))
    opts = parser.parse_args()
    opts.lib = Path(os.path.realpath(opts.dcheck)).parent.parent / "lib"
    if not opts.lib.is_dir():
        parser.error("no toolchain lib/ beside %s" % opts.dcheck)
    opts.defines, opts.unwind, opts.object_bits = conf(opts.root)
    names = opts.only or list(CONTROLS)
    work = opts.work or Path(tempfile.mkdtemp(prefix="c-utils-controls-"))
    work.mkdir(parents=True, exist_ok=True)
    ok = True
    try:
        def copy_of(name, patches):
            copy = work / name
            shutil.rmtree(copy, ignore_errors=True)
            copy.mkdir()
            for item in COPIED:
                src = opts.root / item
                (shutil.copytree if src.is_dir() else shutil.copy2)(src, copy / item)
            apply(copy, patches)
            return copy

        baseline = copy_of("baseline", [])
        checks = []
        for name in names:
            for check, _ in CONTROLS[name][2]:
                if check not in checks:
                    checks.append(check)
        for check in checks:
            verdict, detail, seconds, peak = build_and_run(check, baseline, opts)
            if verdict == "pass":
                print("PASS baseline %s %s" % (check[1], measure(seconds, peak)))
            else:
                ok = False
                print("FAIL baseline %s: %s" % (check[1], detail if verdict == "broken" else tail(str(detail))))
        for name in names:
            what, patches, expectations = CONTROLS[name]
            copy = copy_of(name, patches)
            for check, expected in expectations:
                verdict, detail, seconds, peak = build_and_run(check, copy, opts)
                if expected is None:
                    if verdict == "pass":
                        print("UNREACHED %s by %s, as recorded" % (name, check[1]))
                    else:
                        ok = False
                        print("REACHED %s by %s: %s" % (name, check[1], detail if verdict == "broken" else
                                                        ", ".join(sorted(detail)) if check[0] == "cbmc" else tail(detail)))
                    continue
                line = matches(check, detail, expected) if verdict == "fail" else None
                if line is not None:
                    print("CAUGHT %s by %s: %s %s" % (name, check[1], line, measure(seconds, peak)))
                    continue
                ok = False
                if verdict == "pass":
                    print("SURVIVED %s: %s passes (%s)" % (name, check[1], what))
                elif verdict == "fail":
                    shown = ", ".join(sorted(detail)) if check[0] == "cbmc" else tail(detail)
                    print("WRONG FAILURE %s by %s: %s" % (name, check[1], shown))
                else:
                    print("BROKEN %s by %s: %s" % (name, check[1], detail))
    finally:
        if opts.work is None:
            shutil.rmtree(work, ignore_errors=True)
    print("fault controls: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
