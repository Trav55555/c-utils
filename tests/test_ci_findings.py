"""Offline checks for ci/findings.py: the nightly verdict, the findings report
and the issue calls, with Gitea replaced by a recorder."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("findings", ROOT / "ci/findings.py")
FINDINGS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FINDINGS)


def result(status="pass", checks=()):
    return {"schema": "dcheck.result/0", "tier": "deep", "status": status,
            "failed": [c["name"] for c in checks if c["status"] == "fail"],
            "skipped": [c["name"] for c in checks if c["status"] == "skip"], "checks": list(checks)}


REGIONS = {"name": "regions", "status": "pass"}
SEMANTICS_FAIL = {"name": "semantics", "status": "fail", "detail": "32 files",
                  "log": ".dcheck/logs/semantics.log",
                  "diagnostics": [{"text": "DCHECK: semantics/sem_x.c failed under cerberus"},
                                  {"text": "src/a.c:7:3: error[rg-unchecked]: x", "file": "src/a.c",
                                   "line": 7, "column": 3, "severity": "error", "message": "x",
                                   "rule": "rg-unchecked"}]}


class Recorder(FINDINGS.Gitea):
    """Answers the issue calls from a list of open issues and records them."""

    def __init__(self, open_issues=(), fail=False):
        super().__init__("login", "owner/repo")
        self.open_issues, self.fail, self.calls = list(open_issues), fail, []

    def call(self, method, path, body=None, expect="id"):
        if self.fail:
            raise RuntimeError("%s %s failed" % (method, path))
        self.calls.append((method, path.split("?")[0], body))
        if method == "GET":
            return self.open_issues
        return {"id": 1, "number": 9}


class JudgeTests(unittest.TestCase):
    def test_no_result_fails(self):
        self.assertFalse(FINDINGS.judge(None, [])[0])

    def test_pass_with_required_check_passing(self):
        self.assertEqual(FINDINGS.judge(result("pass", [REGIONS]), ["regions"]), (True, []))

    def test_required_check_skipped_or_absent_fails(self):
        for checks in [[{"name": "regions", "status": "skip"}], []]:
            with self.subTest(checks=checks):
                self.assertEqual(FINDINGS.judge(result("pass", checks), ["regions"]),
                                 (False, ["required check regions did not pass"]))

    def test_failed_checks_are_named(self):
        self.assertEqual(FINDINGS.judge(result("fail", [REGIONS, SEMANTICS_FAIL]), ["regions"]),
                         (False, ["dcheck deep: semantics"]))


class ReportTests(unittest.TestCase):
    def test_failed_check_diagnostics_and_log_tail(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / ".dcheck/logs/semantics.log"
            log.parent.mkdir(parents=True)
            log.write_text("".join("line %d\n" % i for i in range(100)) + "```end\n")
            body = FINDINGS.report(result("fail", [SEMANTICS_FAIL]), ["dcheck deep: semantics"],
                                   directory, "abc", "https://h/r/1", None)
        self.assertIn("## semantics", body)
        self.assertIn("32 files", body)
        self.assertIn("- `src/a.c:7` [rg-unchecked] x", body)
        self.assertIn("- DCHECK: semantics/sem_x.c failed under cerberus", body)
        # The tail is the last 60 lines.
        self.assertIn("line 99", body)
        self.assertNotIn("line 39\n", body)
        # The log's own ``` cannot close the block it is quoted in.
        self.assertIn("````\n", body)

    def test_without_result_quotes_the_run_log(self):
        with tempfile.NamedTemporaryFile("w", suffix=".log") as run_log:
            run_log.write("make: *** [deep] Error 1\n")
            run_log.flush()
            body = FINDINGS.report(None, ["no dcheck result"], "/", "abc", "u", run_log.name)
        self.assertIn("make: *** [deep] Error 1", body)

    def test_body_is_truncated(self):
        huge = dict(SEMANTICS_FAIL, log=None, diagnostics=[{"text": "x" * 2000}] * 40)
        body = FINDINGS.report(result("fail", [huge]), [], "/", "abc", "u", None)
        self.assertLess(len(body), FINDINGS.MAX_BODY + 200)
        self.assertTrue(body.endswith("full logs)\n"))


class PublishTests(unittest.TestCase):
    ISSUE = {"number": 4, "title": FINDINGS.TITLE}
    OTHER = {"number": 5, "title": "Something else"}

    def test_pass_without_open_issue_does_nothing(self):
        gitea = Recorder([self.OTHER])
        self.assertEqual(FINDINGS.publish(gitea, True, "b", "abc", "u"), "no open findings")
        self.assertEqual([c[0] for c in gitea.calls], ["GET"])

    def test_pass_comments_and_closes(self):
        gitea = Recorder([self.OTHER, self.ISSUE])
        self.assertEqual(FINDINGS.publish(gitea, True, "b", "abc", "u"), "closed #4")
        self.assertEqual(gitea.calls[1][:2], ("POST", "/issues/4/comments"))
        self.assertEqual(gitea.calls[2], ("PATCH", "/issues/4", {"state": "closed"}))

    def test_failure_opens_issue(self):
        gitea = Recorder([self.OTHER])
        self.assertEqual(FINDINGS.publish(gitea, False, "body", "abc", "u"), "opened #9")
        self.assertEqual(gitea.calls[1], ("POST", "/issues", {"title": FINDINGS.TITLE, "body": "body"}))

    def test_failure_updates_open_issue(self):
        gitea = Recorder([self.ISSUE])
        self.assertEqual(FINDINGS.publish(gitea, False, "body", "abc", "u"), "updated #4")
        self.assertEqual(gitea.calls[1], ("PATCH", "/issues/4", {"body": "body"}))
        self.assertEqual(gitea.calls[2][:2], ("POST", "/issues/4/comments"))


class TeaTests(unittest.TestCase):
    def test_error_body_is_retried_then_raised(self):
        # tea exits 0 on an HTTP error, so the response's shape is the check.
        gitea = FINDINGS.Gitea("login", "owner/repo", attempts=2, pause=0)
        error = subprocess.CompletedProcess([], 0, stdout='{"message":"not found"}', stderr="")
        with mock.patch.object(FINDINGS.subprocess, "run", return_value=error) as run:
            with self.assertRaises(RuntimeError):
                gitea.call("POST", "/issues", {"title": "t"}, expect="number")
        self.assertEqual(run.call_count, 2)
        self.assertEqual(run.call_args[0][0][:7],
                         ["tea", "api", "--login", "login", "-X", "POST", "/repos/owner/repo/issues"])

    def test_expected_shape_is_returned(self):
        gitea = FINDINGS.Gitea("login", "owner/repo", attempts=2, pause=0)
        ok = subprocess.CompletedProcess([], 0, stdout='{"id":3,"number":2}', stderr="")
        with mock.patch.object(FINDINGS.subprocess, "run", return_value=ok):
            self.assertEqual(gitea.call("POST", "/issues", {"title": "t"}, expect="number")["number"], 2)


class MainTests(unittest.TestCase):
    """The exit status is the verdict, whether or not the issue could be kept."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.path = Path(temporary.name) / "result.json"
        self.base = ["--result", str(self.path), "--root", temporary.name, "--require", "regions",
                     "--rev", "abc", "--run-url", "u"]

    def main(self, argv):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return FINDINGS.main(argv)

    def test_verdict(self):
        self.path.write_text(json.dumps(result("pass", [REGIONS])))
        self.assertEqual(self.main(self.base), 0)
        self.assertEqual(self.main(self.base + ["--require", "cbmc"]), 1)
        # A failure outside dcheck (a leftover process, make itself) is a finding too.
        self.assertEqual(self.main(self.base + ["--exit-status", "2"]), 1)
        self.path.write_text("not json")
        self.assertEqual(self.main(self.base), 1)

    def test_profile_line_before_result(self):
        # dcheck-tools exec's profile line can precede the result on stdout.
        self.path.write_text("Verified checker profile: /p\n" + json.dumps(result("pass", [REGIONS])))
        self.assertEqual(self.main(self.base), 0)

    def test_issue_failure_does_not_change_verdict(self):
        self.path.write_text(json.dumps(result("pass", [REGIONS])))
        with mock.patch.object(FINDINGS, "Gitea", lambda *a: Recorder(fail=True)):
            self.assertEqual(self.main(self.base + ["--login", "l", "--repo", "o/r"]), 0)

    def test_no_login_leaves_issue_alone(self):
        self.path.write_text(json.dumps(result("fail", [SEMANTICS_FAIL])))
        with mock.patch.object(FINDINGS, "Gitea") as gitea:
            self.assertEqual(self.main(self.base + ["--repo", "o/r"]), 1)
        gitea.assert_not_called()


if __name__ == "__main__":
    unittest.main()
