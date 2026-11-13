"""Regression checks for differential-fuzzer input and oracle validation."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import diff_fuzz


def executable(path, source):
    path.write_text("#!%s\n%s\n" % (sys.executable, source), encoding="utf-8")
    path.chmod(0o755)
    return str(path)


class DiffFuzzTests(unittest.TestCase):
    def test_oracle_rejects_bad_cases_responses(self):
        modes = {
            "empty": "[]",
            "short": '["only one"]',
            "nonstrings": '["html", 7]',
            "malformed": "{",
        }
        with tempfile.TemporaryDirectory() as tmp:
            for mode, response in modes.items():
                with self.subTest(mode=mode):
                    path = Path(tmp) / (mode + ".py")
                    oracle = executable(path, "import sys\nsys.stdout.write(%r)" % response)
                    runner = diff_fuzz.Runner(oracle, "unused-driver", 1)
                    with self.assertRaises(diff_fuzz.OracleProtocolError):
                        runner.oracle_many(diff_fuzz.SET_BY_NAME["commonmark"], [b"one", b"two"])
                    runner.pool.shutdown()

    def test_run_rejects_nonpositive_bounds(self):
        with tempfile.TemporaryDirectory() as tmp:
            driver = executable(Path(tmp) / "driver.py", "pass")
            oracle = executable(Path(tmp) / "oracle.py", "pass")
            for args, message in [(["--count", "0"], "--count must be positive"),
                                  (["--jobs", "0"], "--jobs must be positive"),
                                  (["--shrink-max", "-1"], "--shrink-max must be nonnegative"),
                                  (["--sets", ""], "--sets must name at least one option set"),
                                  (["--sets", "typo"], "unknown option set: typo")]:
                with self.subTest(args=args):
                    result = subprocess.run(
                        [sys.executable, "-I", str(Path(__file__).with_name("diff_fuzz.py")),
                         "--driver", driver, "--oracle", oracle] + args,
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False)
                    self.assertEqual(result.returncode, 2)
                    self.assertIn(message, result.stderr)

    def test_comparison_control_passes_and_detects_known_bad_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            oracle = executable(root / "oracle.py", """
import json, sys
with open(sys.argv[2], encoding="utf-8") as f:
    cases = json.load(f)
json.dump(["expected"] * len(cases), sys.stdout)
""")
            driver = executable(root / "driver.py", """
import os, sys
sys.stdout.write(os.environ["STUB_DRIVER_OUTPUT"])
""")
            command = [sys.executable, "-I", str(Path(__file__).with_name("diff_fuzz.py")),
                       "--oracle", oracle, "--driver", driver, "--sets", "commonmark",
                       "--count", "1", "--seed", "23", "--jobs", "1", "--shrink-max", "0"]
            env = dict(os.environ, STUB_DRIVER_OUTPUT="expected")
            control = subprocess.run(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     text=True, check=False)
            self.assertEqual(control.returncode, 0, control.stderr + control.stdout)
            self.assertIn("1 documents, 0 mismatches", control.stdout)

            env["STUB_DRIVER_OUTPUT"] = "incorrect"
            bad = subprocess.run(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 text=True, check=False)
            self.assertEqual(bad.returncode, 1, bad.stderr + bad.stdout)
            self.assertIn("1 documents, 1 mismatches", bad.stdout)


class CorpusSelectionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.corpus_runner = os.environ.get("C_MARK_TEST_CORPUS")
        if not cls.corpus_runner or not Path(cls.corpus_runner).is_file():
            raise RuntimeError("run through make test-tools to build test_corpus")

    def test_empty_array_and_unmatched_suite_fail_even_in_report_mode(self):
        with tempfile.TemporaryDirectory() as tmp:
            empty = Path(tmp) / "empty.json"
            empty.write_text("[]", encoding="utf-8")
            result = subprocess.run([self.corpus_runner, str(empty), "--report"],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False)
            self.assertEqual(result.returncode, 2)
            self.assertIn("contains no corpus cases", result.stderr)

            cases = Path(tmp) / "cases.json"
            cases.write_text('[{"suite":"spec"}]', encoding="utf-8")
            result = subprocess.run([self.corpus_runner, str(cases), "--suite", "missing", "--report"],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False)
            self.assertEqual(result.returncode, 2)
            self.assertIn("corpus selection matched no cases", result.stderr)


if __name__ == "__main__":
    unittest.main()
