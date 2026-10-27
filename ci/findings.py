#!/usr/bin/env python3
"""Judge a nightly run from dcheck's JSON result and keep its findings issue.

A run passes when its acceptance exited 0, dcheck's result passes and every
--require check is among its passes, so a required check cannot pass by
being skipped. Each failed
check becomes a finding: its diagnostics (file, line, rule) and the end of its
log. One open issue titled TITLE holds the current findings: a failing run
opens it or replaces its body and comments, and a passing run comments and
closes it. The exit status is the verdict alone; a Gitea call that fails
after its retries is reported and does not change it.

A call is retried only while Gitea gives no answer of its own (no JSON, as
when it is down or restarting), and only within --within seconds for all the
calls together. An error from Gitea itself ends the attempt. A create that
may have reached Gitea before its answer was lost is looked up before it is
sent again, so a retry cannot open a second issue or post a comment twice.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time

TITLE = "Nightly findings"
MAX_DIAGNOSTICS = 40
LOG_LINES = 60
LOG_CHARS = 8000
MAX_BODY = 60000
TRUNCATED = "\n(truncated; the run's artifact has the full logs)\n"
PAGE = 50


def load_result(path):
    """dcheck.result/0 from PATH, or None when the run produced none.

    The result is the file's last line that opens a JSON object: dcheck-tools
    exec prints its profile line on the same stdout first."""
    try:
        lines = [l for l in Path(path).read_text().splitlines() if l.startswith("{")]
        result = json.loads(lines[-1])
    except (OSError, ValueError, IndexError):
        return None
    if not isinstance(result, dict) or result.get("schema") != "dcheck.result/0":
        return None
    return result


def judge(result, required, exit_status=0):
    """(passed, reasons): the verdict and why a failing run failed."""
    reasons = ["acceptance exited %d" % exit_status] if exit_status else []
    if result is None:
        return False, reasons + ["no dcheck result: the run stopped before dcheck finished"]
    if result.get("status") != "pass":
        reasons.append("dcheck %s: %s" % (result.get("tier", "?"), ", ".join(result.get("failed", [])) or "fail"))
    passed = {c.get("name") for c in result.get("checks", []) if c.get("status") == "pass"}
    for name in required:
        if name not in passed:
            reasons.append("required check %s did not pass" % name)
    return not reasons, reasons


def tail(path, lines):
    try:
        text = Path(path).read_text(errors="replace")
    except OSError:
        return "(log missing: %s)" % path
    text = "\n".join(text.splitlines()[-lines:])
    return text if len(text) <= LOG_CHARS else "..." + text[-LOG_CHARS:]


def fence(text):
    """TEXT in a code block that no backtick run inside it can close."""
    longest = run = 0
    for ch in text:
        run = run + 1 if ch == "`" else 0
        longest = max(longest, run)
    ticks = "`" * max(3, longest + 1)
    return "%s\n%s\n%s" % (ticks, text, ticks)


def diagnostic(d):
    if d.get("file") and d.get("line"):
        where = "%s:%s" % (d["file"], d["line"])
        rule = " [%s]" % d["rule"] if d.get("rule") else ""
        return "`%s`%s %s" % (where, rule, d.get("message", d.get("text", "")))
    return d.get("text", "")


def report(result, reasons, root, rev, run_url, log):
    """The findings issue body, in Markdown."""
    parts = ["Revision `%s`, [run](%s)." % (rev, run_url), ""]
    parts += ["- %s" % r for r in reasons]
    failed = [c for c in (result or {}).get("checks", []) if c.get("status") == "fail"]
    for check in failed:
        parts += ["", "## %s" % check.get("name", "?")]
        if check.get("detail"):
            parts.append(check["detail"])
        diagnostics = check.get("diagnostics", [])
        if diagnostics:
            parts.append("")
            parts += ["- %s" % diagnostic(d) for d in diagnostics[:MAX_DIAGNOSTICS]]
            if len(diagnostics) > MAX_DIAGNOSTICS:
                parts.append("- ... and %d more" % (len(diagnostics) - MAX_DIAGNOSTICS))
        if check.get("log"):
            parts += ["", "Last %d lines of `%s`:" % (LOG_LINES, check["log"]), "",
                      fence(tail(Path(root) / check["log"], LOG_LINES))]
    if not failed and log:
        parts += ["", "Last %d lines of the run's log:" % LOG_LINES, "", fence(tail(log, LOG_LINES))]
    # Whole parts only, so a cut never leaves a code block open.
    body = ""
    for part in parts:
        if len(body) + len(part) + 1 > MAX_BODY - len(TRUNCATED):
            return body + TRUNCATED
        body += part + "\n"
    return body


class Gitea:
    """The issue calls, through the runner's tea login."""

    def __init__(self, login, repo, attempts=1, pause=120, within=None, timeout=60):
        self.login, self.repo, self.attempts, self.pause = login, repo, attempts, pause
        self.timeout = timeout
        self.deadline = None if within is None else time.monotonic() + within

    def call(self, method, path, body=None, expect="id", applied=None):
        """The response to METHOD PATH. Before a retry, APPLIED (when given)
        looks for what an earlier attempt may have created, and its result
        is returned instead of sending the request again."""
        command = ["tea", "api", "--login", self.login, "-X", method, "/repos/%s%s" % (self.repo, path)]
        for attempt in range(1, self.attempts + 1):
            if attempt > 1 and applied is not None:
                found = applied()
                if found is not None:
                    return found
            with tempfile.NamedTemporaryFile("w", suffix=".json") as data:
                if body is not None:
                    json.dump(body, data)
                    data.flush()
                    command_now = command + ["-d", "@" + data.name]
                else:
                    command_now = command
                try:
                    out = subprocess.run(command_now, capture_output=True, text=True,
                                         timeout=self.timeout).stdout
                    response = json.loads(out)
                except (OSError, ValueError, subprocess.SubprocessError):
                    response = None
            # tea exits 0 on an HTTP error, so the response's shape is the check.
            if isinstance(response, list) or (isinstance(response, dict) and expect in response):
                return response
            if isinstance(response, dict) and "message" in response:
                raise RuntimeError("%s %s: %s" % (method, path, response["message"]))
            if attempt == self.attempts or not self.time_for_another():
                break
            time.sleep(self.pause)
        raise RuntimeError("%s %s failed" % (method, path))

    def time_for_another(self):
        return self.deadline is None or time.monotonic() + self.pause + self.timeout <= self.deadline

    def open_issue(self):
        page = 1
        while True:
            issues = self.call("GET", "/issues?state=open&type=issues&limit=%d&page=%d" % (PAGE, page))
            found = next((i for i in issues if i.get("title") == TITLE), None)
            if found is not None or len(issues) < PAGE:
                return found
            page += 1

    def comment(self, number, body):
        def posted():
            comments = self.call("GET", "/issues/%d/comments?limit=%d" % (number, PAGE))
            return next((c for c in reversed(comments) if c.get("body") == body), None)
        return self.call("POST", "/issues/%d/comments" % number, {"body": body}, applied=posted)


def publish(gitea, passed, body, rev, run_url):
    """Open, update or close the findings issue; return what was done."""
    issue = gitea.open_issue()
    if passed:
        if issue is None:
            return "no open findings"
        number = issue["number"]
        gitea.comment(number, "Clean on `%s` ([run](%s)); closing." % (rev, run_url))
        gitea.call("PATCH", "/issues/%d" % number, {"state": "closed"}, expect="number")
        return "closed #%d" % number
    if issue is None:
        created = gitea.call("POST", "/issues", {"title": TITLE, "body": body}, expect="number",
                             applied=gitea.open_issue)
        return "opened #%d" % created["number"]
    number = issue["number"]
    gitea.call("PATCH", "/issues/%d" % number, {"body": body}, expect="number")
    gitea.comment(number, "Still failing on `%s` ([run](%s)); the description has the current findings."
                  % (rev, run_url))
    return "updated #%d" % number


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--result", required=True, help="dcheck --json output")
    parser.add_argument("--root", required=True, help="directory the result's log paths are relative to")
    parser.add_argument("--require", action="append", default=[], help="a check that must pass")
    parser.add_argument("--log", help="the run's own log, quoted when no check failed")
    parser.add_argument("--exit-status", type=int, default=0, help="the acceptance run's exit status")
    parser.add_argument("--rev", required=True)
    parser.add_argument("--run-url", required=True)
    parser.add_argument("--login", help="tea login; without it the issue is left alone")
    parser.add_argument("--repo", help="owner/name")
    parser.add_argument("--attempts", type=int, help="tries per call (default 1, or unlimited with --within)")
    parser.add_argument("--within", type=float, help="seconds all the issue calls may take together")
    args = parser.parse_args(argv)
    result = load_result(args.result)
    passed, reasons = judge(result, args.require, args.exit_status)
    body = report(result, reasons, args.root, args.rev, args.run_url, args.log)
    # dcheck --json prints no summary of its own; the job log gets this one.
    for check in (result or {}).get("checks", []):
        print("%s %s%s" % (check.get("status", "?").upper(), check.get("name", "?"),
                           " (%s)" % check["detail"] if check.get("detail") else ""))
    if not passed:
        print(body, end="")
    print("findings: %s" % ("none" if passed else "; ".join(reasons)))
    if args.login and args.repo:
        attempts = args.attempts or (sys.maxsize if args.within else 1)
        try:
            print("findings issue: " + publish(Gitea(args.login, args.repo, attempts, within=args.within),
                                               passed, body, args.rev, args.run_url))
        except RuntimeError as error:
            print("findings issue not updated: %s" % error, file=sys.stderr)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
