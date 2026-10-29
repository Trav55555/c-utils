"""Offline checks for ci/timings.py: the job-log summary of check and
semantics file times."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("timings", ROOT / "ci/timings.py")
TIMINGS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TIMINGS)

TSV = ("subset\ttotal\t1249056\t0\n"
       "sanitize\tlibrary\t63613\t0\n"
       "sanitize\ttotal\t17468303\t0\n"
       "semantics\ttotal\t1153000000\t0\n")
SEMANTICS = ("DCHECK: semantics suite: full tests: tests (3 files)\n"
             "test_a: PASS\n"
             "DCHECK: tests/test_a.c took 12 s of 300 s\n"
             "DCHECK: tests/test_b.c timed out under cerberus after 300 s\n"
             "DCHECK: tests/test_b.c took 300 s of 300 s\n"
             "DCHECK: tests/test_c.c took 150 s of 300 s\n")


class Timings(unittest.TestCase):
    def logs(self, tsv=None, semantics=None):
        d = Path(self.enterContext(tempfile.TemporaryDirectory()))
        if tsv is not None:
            (d / "timing.tsv").write_text(tsv)
        if semantics is not None:
            (d / "semantics.log").write_text(semantics)
        return d

    def test_totals_only_from_total_rows_in_order(self):
        self.assertEqual(TIMINGS.check_totals(TSV),
                         [("subset", 1.249056), ("sanitize", 17.468303), ("semantics", 1153.0)])

    def test_files_slowest_first(self):
        self.assertEqual([t[0] for t in TIMINGS.file_times(SEMANTICS)],
                         ["tests/test_b.c", "tests/test_c.c", "tests/test_a.c"])

    def test_report_names_files_at_half_their_limit(self):
        lines = TIMINGS.report(self.logs(TSV, SEMANTICS))
        self.assertEqual(lines[0], "check times: subset 1 s, sanitize 17 s, semantics 1153 s")
        self.assertEqual(lines[1], "slowest semantics files (3 timed):")
        self.assertEqual(lines[2], "  300 s of 300 s  tests/test_b.c")
        self.assertEqual(lines[-1], "at half their limit or more: tests/test_b.c, tests/test_c.c")

    def test_report_without_per_file_lines(self):
        # A toolchain that predates per-file times still gets its check totals.
        self.assertEqual(TIMINGS.report(self.logs(TSV, "test_a: PASS\n")),
                         ["check times: subset 1 s, sanitize 17 s, semantics 1153 s"])

    def test_report_with_no_logs(self):
        self.assertEqual(TIMINGS.report(self.logs()), [])

    def test_slowest_list_is_capped(self):
        many = "".join(f"DCHECK: tests/t{i}.c took {i} s of 300 s\n" for i in range(30))
        lines = TIMINGS.report(self.logs(semantics=many))
        self.assertEqual(len(lines), 1 + TIMINGS.SLOWEST)
        self.assertEqual(lines[1], "  29 s of 300 s  tests/t29.c")


if __name__ == "__main__":
    unittest.main()
