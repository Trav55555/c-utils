"""Offline checks for private CI settings and pinned source extraction."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("fetch_dependencies", ROOT / "ci/fetch_dependencies.py")
FETCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FETCH)
NAME = FETCH.NAMES[0]
SETTINGS = {
    "CUTILS_CI_API_LOGIN": "ci-test",
    "CUTILS_CI_TOOLCHAIN_REPOSITORY": "owner/toolchain",
}


class ConfigurationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        self.path = self.work / "dependencies.json"
        self.config = {"schema": "c-utils.ci-dependencies/1",
                       NAME: {"revision": "a" * 40, "tree": "b" * 40}}

    def save(self, config):
        self.path.write_text(json.dumps(config))

    def invoke(self):
        with mock.patch("sys.argv", ["fetch_dependencies.py", str(self.path), str(self.work)]):
            FETCH.main()

    def test_real_manifest_contains_only_pins(self):
        config = FETCH.configuration(ROOT / "ci/dependencies.json")
        self.assertEqual(set(config[NAME]), {"revision", "tree"})

    def test_reject_schema_membership_and_origin_fields(self):
        for config in [[], None, {}, {**self.config, "extra": {}},
                       {**self.config, "schema": "c-utils.ci-dependencies/0"},
                       {**self.config, NAME: {**self.config[NAME], "url": "https://private.invalid/repo.git"}},
                       {**self.config, NAME: []}]:
            with self.subTest(config=config):
                self.save(config)
                with self.assertRaises(ValueError):
                    FETCH.configuration(self.path)

    def test_reject_malformed_pins(self):
        for key in ["revision", "tree"]:
            for value in [None, 42, [], "a" * 39, "a" * 41, "A" * 40, "z" * 40]:
                with self.subTest(key=key, value=value):
                    config = {**self.config, NAME: {**self.config[NAME], key: value}}
                    self.save(config)
                    with self.assertRaises(ValueError):
                        FETCH.configuration(self.path)
            self.save({**self.config, NAME: {k: v for k, v in self.config[NAME].items() if k != key}})
            with self.assertRaises(ValueError):
                FETCH.configuration(self.path)

    def test_valid_private_source(self):
        self.assertEqual(FETCH.private_source(SETTINGS), ("ci-test", "owner/toolchain"))

    def test_missing_settings_do_not_use_default_login(self):
        self.save(self.config)
        for settings in [{}, {"CUTILS_CI_API_LOGIN": "ci-test"},
                         {"CUTILS_CI_TOOLCHAIN_REPOSITORY": "owner/toolchain"}]:
            with self.subTest(settings=settings), mock.patch.dict(os.environ, settings, clear=True), \
                    mock.patch.object(FETCH.subprocess, "run") as run:
                with self.assertRaises(ValueError):
                    self.invoke()
                run.assert_not_called()

    def test_reject_unsafe_login_names(self):
        for value in ["", "--default", "a b", "a/b", "x\ny", "a;echo", "../login"]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                FETCH.private_source({**SETTINGS, "CUTILS_CI_API_LOGIN": value})

    def test_reject_unsafe_repository_paths(self):
        for value in ["", "owner", "/owner/repo", "owner/repo/extra", "../repo", "owner/..",
                      "owner/repo?ref=other", "https://private.invalid/owner/repo", "owner/repo%2fother"]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                FETCH.private_source({**SETTINGS, "CUTILS_CI_TOOLCHAIN_REPOSITORY": value})

    def test_invalid_manifest_never_executes_transport(self):
        self.save({**self.config, NAME: {**self.config[NAME], "tree": "bad"}})
        with mock.patch.dict(os.environ, SETTINGS, clear=True), mock.patch.object(FETCH.subprocess, "run") as run:
            with self.assertRaises(ValueError):
                self.invoke()
            run.assert_not_called()

    def test_explicit_transport_and_tree_verification(self):
        self.save(self.config)
        output = io.StringIO()
        with mock.patch.dict(os.environ, SETTINGS, clear=True), \
                mock.patch.object(FETCH.subprocess, "run") as run, \
                mock.patch.object(FETCH, "extract") as extract, contextlib.redirect_stdout(output):
            self.invoke()
        archive = self.work / (NAME + ".tar.gz")
        run.assert_called_once_with(
            ["tea", "api", "--login", "ci-test", "/repos/owner/toolchain/archive/" + "a" * 40 + ".tar.gz",
             "-o", str(archive)], check=True, timeout=90)
        extract.assert_called_once_with(archive, self.work / NAME, "b" * 40)
        for private_value in SETTINGS.values():
            self.assertNotIn(private_value, output.getvalue())

    def test_transport_failure_does_not_extract_or_fall_back(self):
        self.save(self.config)
        failures = [subprocess.CalledProcessError(7, ["tea"]),
                    subprocess.TimeoutExpired(["tea"], 90)]
        for failure in failures:
            with self.subTest(failure=type(failure).__name__), \
                    mock.patch.dict(os.environ, SETTINGS, clear=True), \
                    mock.patch.object(FETCH.subprocess, "run", side_effect=failure) as run, \
                    mock.patch.object(FETCH, "extract") as extract:
                with self.assertRaises(type(failure)):
                    self.invoke()
                self.assertEqual(run.call_count, 1)
                extract.assert_not_called()


class ArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        self.archive = self.work / "source.tar.gz"
        self.target = self.work / "extracted"

    def pack(self, names, link_type=None):
        with tarfile.open(self.archive, "w:gz") as archive:
            for name in names:
                info = tarfile.TarInfo(name)
                info.mode = 0o644
                if link_type is not None:
                    info.type = link_type
                    info.linkname = "../../escape"
                archive.addfile(info)

    def test_reject_path_traversal_and_git_metadata(self):
        for name in ["/absolute/file", "root/../escape", "root/.git/config", "root/sub/.git/config"]:
            with self.subTest(name=name):
                self.pack([name])
                with self.assertRaises(ValueError):
                    FETCH.extract(self.archive, self.target, "a" * 40)
                self.assertFalse(self.target.exists())

    def test_reject_links_and_devices(self):
        for kind in [tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.CHRTYPE, tarfile.FIFOTYPE]:
            with self.subTest(kind=kind):
                self.pack(["root/entry"], kind)
                with self.assertRaises(ValueError):
                    FETCH.extract(self.archive, self.target, "a" * 40)
                self.assertFalse(self.target.exists())

    def test_reject_multiple_roots_duplicates_and_root_files(self):
        for names in [["one/a", "two/b"], ["root/a", "root/a"], ["root"]]:
            with self.subTest(names=names):
                self.pack(names)
                with self.assertRaises(ValueError):
                    FETCH.extract(self.archive, self.target, "a" * 40)
                self.assertFalse(self.target.exists())

    def test_verify_complete_git_tree(self):
        payload = b"hello\n"
        blob = hashlib.sha1(b"blob 6\0" + payload).digest()
        entry = b"100644 payload.txt\0" + blob
        tree = hashlib.sha1(b"tree " + str(len(entry)).encode() + b"\0" + entry).hexdigest()
        with tarfile.open(self.archive, "w:gz") as archive:
            info = tarfile.TarInfo("root/payload.txt")
            info.mode = 0o644
            info.size = len(payload)
            archive.addfile(info, io.BytesIO(payload))
        FETCH.extract(self.archive, self.target, tree)
        self.assertEqual((self.target / "payload.txt").read_bytes(), payload)
        with self.assertRaisesRegex(ValueError, "pinned source tree mismatch"):
            FETCH.extract(self.archive, self.work / "wrong-tree", "0" * 40)


class WorkflowTests(unittest.TestCase):
    def test_authorization_guard_requires_private_configuration(self):
        text = (ROOT / ".gitea/workflows/ci.yml").read_text()
        match = re.search(r"    if: >-\n((?:      .*\n)+)", text)
        self.assertIsNotNone(match)
        actual = " ".join(line.strip() for line in match.group(1).splitlines())
        expected = ("always() && (github.event_name != 'schedule' || needs.unbuilt.outputs.build == 'true') && "
                    "vars.CUTILS_CI_REPOSITORY != '' && "
                    "github.repository == vars.CUTILS_CI_REPOSITORY && "
                    "(github.event_name == 'schedule' || "
                    "((github.event_name == 'push' || github.event_name == 'workflow_dispatch') && "
                    "vars.CUTILS_CI_ACTOR != '' && "
                    "github.actor == vars.CUTILS_CI_ACTOR))")
        self.assertEqual(actual, expected)
        # Push gates on the shared runner; the deep tier on its own runner
        # when one is configured, else the shared one.
        self.assertIn("runs-on: ${{ github.event_name != 'push' && vars.CUTILS_CI_DEEP_RUNNER"
                      " || vars.CUTILS_CI_RUNNER }}", text)
        for key in SETTINGS:
            self.assertIn(key + ": ${{ vars." + key + " }}", text)
        self.assertIn("persist-credentials: false", text)
        self.assertNotIn("pull_request", text)

    def test_scheduled_runs_build_only_an_unbuilt_head(self):
        text = (ROOT / ".gitea/workflows/ci.yml").read_text()
        self.assertIn("  unbuilt:\n    if: github.event_name == 'schedule' && vars.CUTILS_CI_REPOSITORY != '' && "
                      "github.repository == vars.CUTILS_CI_REPOSITORY\n    runs-on: ${{ vars.CUTILS_CI_RUNNER }}\n", text)
        self.assertIn("  verify:\n    needs: unbuilt\n", text)
        self.assertIn("'c-utils nightly'; then\n            echo build=false", text)
        self.assertIn("- name: Record the nightly result on the commit\n        if: always() && github.event_name != 'push'\n", text)
        self.assertIn('\\"context\\":\\"c-utils nightly\\"', text)

    def test_every_branch_push_runs_the_gate(self):
        text = (ROOT / ".gitea/workflows/ci.yml").read_text()
        self.assertIn("on:\n  push:\n    branches: ['**']\n", text)
        self.assertNotIn("verify/**", text)

    def test_push_records_the_gate_result_on_the_commit(self):
        text = (ROOT / ".gitea/workflows/ci.yml").read_text()
        step = text[text.index("- name: Record the gate result on the commit\n"):]
        step = step[:step.index('"$API/statuses/$SHA"')]
        self.assertIn("if: always() && github.event_name == 'push'\n", step)
        self.assertIn("TOKEN: ${{ github.token }}", step)
        self.assertIn("STATE: ${{ job.status == 'success' && 'success' || 'failure' }}", step)
        self.assertIn("RUN_URL: ${{ github.server_url }}/${{ github.repository }}/actions/runs/${{ github.run_id }}",
                      step)
        self.assertIn('\\"context\\":\\"c-utils gate\\",\\"target_url\\":\\"$RUN_URL\\"', step)

    def test_push_runs_gate_and_nightly_runs_deep(self):
        text = (ROOT / ".gitea/workflows/ci.yml").read_text()
        self.assertIn("cron: '0 6 * * *'", text)
        self.assertIn("workflow_dispatch:", text)
        self.assertIn("group: c-utils-${{ github.event_name }}-${{ github.ref }}", text)
        self.assertIn("if: github.event_name == 'push'\n        run: bash ci/verify.sh gate\n", text)
        self.assertIn("if: github.event_name != 'push'\n        run: bash ci/verify.sh deep\n", text)

    def test_findings_issue_uses_only_private_login_and_trusted_repository(self):
        text = (ROOT / "ci/verify.sh").read_text()
        self.assertIn('--login "$CUTILS_CI_API_LOGIN" --repo "$GITHUB_REPOSITORY"', text)
        self.assertIn('DCHECK_JSON="$work/result.json"', text)

    def test_deep_setup_failure_keeps_the_findings_issue(self):
        text = (ROOT / "ci/verify.sh").read_text()
        armed = text.index("trap setup_failed EXIT")
        self.assertLess(armed, text.index("fetch_dependencies.py"))
        self.assertLess(armed, text.index("make -C \"$toolchain\" regions/regions"))
        self.assertLess(text.index("deep DEEP_SECONDS"), text.index("trap - EXIT"))
        # The lock is held until the findings issue is kept.
        self.assertNotIn("exec 9>&-", text)

    def test_local_deep_budget_fits_the_job(self):
        script = (ROOT / "ci/verify.sh").read_text()
        workflow = (ROOT / ".gitea/workflows/ci.yml").read_text()
        deep = int(re.search(r"deep DEEP_SECONDS=(\d+)", script).group(1))
        reporting = int(re.search(r"--within (\d+)", script).group(1))
        job = int(re.search(r"    timeout-minutes: (\d+)", workflow.split("  verify:\n", 1)[1]).group(1)) * 60
        self.assertGreaterEqual(deep, 6000)
        self.assertGreaterEqual(job, deep + reporting + 1800)

    def test_gate_runs_each_subproject_after_the_root(self):
        text = (ROOT / "ci/verify.sh").read_text()
        root_gate = text.index('gate_project "$work/c-utils" "$root/ci-artifacts"')
        sub_gate = text.index('gate_project "$work/c-utils/$sub" "$root/ci-artifacts/${sub//\\//_}"')
        self.assertLess(root_gate, sub_gate)
        self.assertIn('tools/verify_deep.py" --seconds 900 -- "$toolchain/bin/dcheck" gate', text)

    def test_offline_checks_are_wired_into_both_pipelines(self):
        for path in [ROOT / ".github/workflows/test.yml", ROOT / "ci/verify.sh"]:
            self.assertIn("-m unittest discover", path.read_text())
            self.assertIn("-p 'test_ci*.py'", path.read_text())
        self.assertIn("if [ -f c-mark/Makefile ]; then make -C c-mark test-tools; fi",
                      (ROOT / ".github/workflows/test.yml").read_text())
        self.assertIn('if [[ -f "$root/c-mark/Makefile" ]]; then make -C "$root/c-mark" test-tools; fi',
                      (ROOT / "ci/verify.sh").read_text())


GATE = ["subset", "lint", "matrix", "analyzer", "sanitize", "msan", "tsan", "release", "wasm", "i386",
        "coverage", "regions"]


def verify_sh(script, *args, cwd=None):
    """SCRIPT run in bash after sourcing ci/verify.sh, which then only defines
    its check lists and functions."""
    return subprocess.run(["bash", "-c", 'source "$0" && ' + script, str(ROOT / "ci/verify.sh"), *args],
                          capture_output=True, text=True, timeout=30, cwd=cwd)


def gate_log(checks=GATE, skips=("authority",), last="dcheck gate: PASS (skipped: authority)"):
    lines = ["dcheck-tools: profile ok"]
    lines += ["PASS %s" % name if name != "coverage" else "PASS coverage (93.78% lines, min 90%)"
              for name in checks]
    lines += ["SKIP %s (no ward.toml)" % name for name in skips]
    return "\n".join(lines + [last]) + "\n"


class RequiredCheckTests(unittest.TestCase):
    def verdict(self, log, *allowed):
        with tempfile.NamedTemporaryFile("w", suffix=".log") as file:
            file.write(log)
            file.flush()
            return verify_sh('gate_verdict "$@"', file.name, *allowed)

    def test_lists(self):
        out = verify_sh('printf "%s\\n" "${gate_required[*]}" "${deep_required[*]}" "${allowed_skips[*]}"')
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertEqual(out.stdout.splitlines(),
                         [" ".join(GATE), " ".join(GATE + ["semantics", "valgrind", "fuzz", "cbmc"]), "authority"])

    def test_every_required_pass_with_authority_skipped_passes(self):
        out = self.verdict(gate_log(), "authority")
        self.assertEqual((out.returncode, out.stdout), (0, ""))

    def test_missing_required_check_fails(self):
        out = self.verdict(gate_log([c for c in GATE if c != "tsan"]), "authority")
        self.assertEqual((out.returncode, out.stdout), (1, "required check tsan did not run\n"))

    def test_skipped_or_failed_required_check_fails(self):
        for line in ["SKIP i386 (no i386 toolchain)", "FAIL i386   (log: .dcheck/logs/i386.log)"]:
            with self.subTest(line=line):
                log = gate_log([c for c in GATE if c != "i386"]).replace("dcheck gate", line + "\ndcheck gate")
                out = self.verdict(log, "authority")
                self.assertEqual((out.returncode, out.stdout), (1, "required check i386 did not pass: %s\n" % line))

    def test_skip_outside_the_allow_list_fails(self):
        out = self.verdict(gate_log(), "")
        self.assertEqual((out.returncode, out.stdout), (1, "check authority skipped and not allowed to\n"))
        out = self.verdict(gate_log(skips=("authority", "newcheck")), "authority")
        self.assertEqual((out.returncode, out.stdout), (1, "check newcheck skipped and not allowed to\n"))

    def test_subproject_allowed_skip_is_not_required(self):
        log = gate_log([c for c in GATE if c != "wasm"], skips=("authority", "wasm"))
        self.assertEqual(self.verdict(log, "authority").returncode, 1)
        self.assertEqual(self.verdict(log, "authority", "wasm").returncode, 0)

    def test_dcheck_verdict_must_pass(self):
        out = self.verdict(gate_log(last="dcheck gate: FAIL (lint)"), "authority")
        self.assertEqual((out.returncode, out.stdout), (1, "dcheck gate did not pass\n"))

    def test_deep_findings_require_every_deep_check(self):
        with tempfile.TemporaryDirectory() as directory:
            out = verify_sh('python3() { printf "%s\\n" "$@"; }; root="$1" work=/w GITHUB_REPOSITORY=o/r '
                            'CUTILS_CI_API_LOGIN=l findings 0', str(ROOT), cwd=directory)
        self.assertEqual(out.returncode, 0, out.stderr)
        args = out.stdout.splitlines()
        required = [args[i + 1] for i, a in enumerate(args) if a == "--require"]
        allowed = [args[i + 1] for i, a in enumerate(args) if a == "--allow-skip"]
        self.assertEqual(required, GATE + ["semantics", "valgrind", "fuzz", "cbmc"])
        self.assertEqual(allowed, ["authority"])

    def test_deep_findings_name_the_runs_commit(self):
        # The runner's GITHUB_SHA, so a job killed mid-run still names its
        # commit; git only when the variable is unset.
        def rev(env):
            with tempfile.TemporaryDirectory() as directory:
                out = verify_sh('python3() { printf "%s\\n" "$@"; }; root="$1" work=/w GITHUB_REPOSITORY=o/r '
                                'CUTILS_CI_API_LOGIN=l ' + env + ' findings 0', str(ROOT), cwd=directory)
            self.assertEqual(out.returncode, 0, out.stderr)
            args = out.stdout.splitlines()
            return args[args.index("--rev") + 1]
        self.assertEqual(rev("GITHUB_SHA=abc123"), "abc123")
        head = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"],
                              capture_output=True, text=True, check=True).stdout.strip()
        self.assertEqual(rev("GITHUB_SHA="), head)


class SubprojectTests(unittest.TestCase):
    def test_tracked_subprojects_only_root_excluded(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            for conf in ["dcheck.conf", "examples/a/dcheck.conf", "c-mark/dcheck.conf", "untracked/dcheck.conf",
                         "examples/a/not-dcheck.conf"]:
                (repo / conf).parent.mkdir(parents=True, exist_ok=True)
                (repo / conf).write_text("SRC_DIR=src\n")
            subprocess.run(["git", "init", "-q", str(repo)], check=True)
            subprocess.run(["git", "-C", str(repo), "add", "dcheck.conf", "examples", "c-mark"], check=True)
            out = verify_sh('subprojects "$1"', str(repo))
            self.assertEqual((out.returncode, out.stdout), (0, "c-mark\nexamples/a\n"))
            empty = Path(directory) / "empty"
            subprocess.run(["git", "init", "-q", str(empty)], check=True)
            self.assertEqual(verify_sh('subprojects "$1"', str(empty)).stdout, "")

    def test_this_repository_has_the_example_subprojects(self):
        out = verify_sh('subprojects "$1"', str(ROOT))
        self.assertIn("examples/curun\n", out.stdout)
        self.assertIn("examples/cuhealth\n", out.stdout)


if __name__ == "__main__":
    unittest.main()
