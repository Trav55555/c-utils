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
        expected = ("vars.CUTILS_CI_REPOSITORY != '' && "
                    "github.repository == vars.CUTILS_CI_REPOSITORY && "
                    "(github.event_name == 'schedule' || "
                    "((github.event_name == 'push' || github.event_name == 'workflow_dispatch') && "
                    "vars.CUTILS_CI_ACTOR != '' && "
                    "github.actor == vars.CUTILS_CI_ACTOR))")
        self.assertEqual(actual, expected)
        self.assertIn("runs-on: ${{ vars.CUTILS_CI_RUNNER }}", text)
        for key in SETTINGS:
            self.assertIn(key + ": ${{ vars." + key + " }}", text)
        self.assertIn("persist-credentials: false", text)
        self.assertNotIn("pull_request", text)

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
        self.assertIn("--require regions", text)
        self.assertIn('DCHECK_JSON="$work/result.json"', text)

    def test_deep_setup_failure_keeps_the_findings_issue(self):
        text = (ROOT / "ci/verify.sh").read_text()
        armed = text.index("trap setup_failed EXIT")
        self.assertLess(armed, text.index("fetch_dependencies.py"))
        self.assertLess(armed, text.index("make -C \"$toolchain\" regions/regions"))
        self.assertLess(text.index("deep DEEP_SECONDS"), text.index("trap - EXIT"))
        # The lock is held until the findings issue is kept.
        self.assertNotIn("exec 9>&-", text)

    def test_offline_checks_are_wired_into_both_pipelines(self):
        for path in [ROOT / ".github/workflows/test.yml", ROOT / "ci/verify.sh"]:
            self.assertIn("-m unittest discover", path.read_text())
            self.assertIn("-p 'test_ci*.py'", path.read_text())


if __name__ == "__main__":
    unittest.main()
