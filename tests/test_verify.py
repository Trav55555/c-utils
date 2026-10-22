"""Offline runner checks and opt-in-by-availability Linux cgroup regressions."""
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
RUNNER = Path(os.environ.get("VERIFY_RUNNER", ROOT / "tools/verify_deep.py"))
SPEC = importlib.util.spec_from_file_location("verify_deep", ROOT / "tools/verify_deep.py")
VERIFY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFY)


class RunnerTests(unittest.TestCase):
    def test_scope_limits_and_literal_arguments(self):
        command = ["checker path", "deep", "; not a shell"]
        for seconds in [1800, 2400]:
            argv = VERIFY.scope_command("test.scope", seconds, command)
            for value in ["MemoryMax=3G", "MemorySwapMax=0", "RuntimeMaxSec=" + str(seconds), "KillSignal=SIGKILL"]:
                self.assertIn(value, argv)
            self.assertEqual(argv[-4:], ["--", *command])
            self.assertIn("--scope", argv)
            self.assertIn("--collect", argv)
            self.assertIn("--expand-environment=no", argv)

    def test_normal_status_is_preserved(self):
        for status in [0, 7]:
            with mock.patch.object(VERIFY.subprocess, "Popen") as popen, \
                    mock.patch.object(VERIFY, "cleanup_scope", return_value=False):
                popen.return_value.wait.return_value = status
                self.assertEqual(VERIFY.run(["checker", "deep"], 1800), status)
                popen.return_value.wait.assert_any_call(timeout=0.1)

    def test_sigterm_during_cleanup_is_deferred(self):
        previous = signal.getsignal(signal.SIGTERM)
        calls = 0
        def population(_unit):
            nonlocal calls
            calls += 1
            if calls == 1:
                os.kill(os.getpid(), signal.SIGTERM)
                return True
            return False
        with mock.patch.object(VERIFY.subprocess, "Popen") as popen, \
                mock.patch.object(VERIFY.subprocess, "run") as stop, \
                mock.patch.object(VERIFY, "populated", side_effect=population):
            popen.return_value.wait.return_value = 0
            popen.return_value.poll.return_value = 0
            self.assertEqual(VERIFY.run(["checker"], 1), 128 + signal.SIGTERM)
            self.assertEqual(stop.call_count, 1)
        self.assertEqual(signal.getsignal(signal.SIGTERM), previous)

    def test_missing_manager_has_no_unbounded_fallback(self):
        with mock.patch.object(VERIFY.subprocess, "Popen", side_effect=FileNotFoundError) as popen:
            with self.assertRaises(FileNotFoundError):
                VERIFY.run(["checker"], 1)
            self.assertEqual(popen.call_count, 1)

    def test_missing_scope_is_skip_unless_required(self):
        with mock.patch.object(sys, "platform", "unsupported"):
            with mock.patch.dict(os.environ, {"VERIFY_REQUIRE_SCOPE": "0"}):
                with self.assertRaises(unittest.SkipTest):
                    ScopeTests.setUpClass()
            with mock.patch.dict(os.environ, {"VERIFY_REQUIRE_SCOPE": "1"}):
                with self.assertRaisesRegex(RuntimeError, "required live scope tests unavailable"):
                    ScopeTests.setUpClass()

    def test_make_forwards_local_and_ci_budgets(self):
        for settings, seconds in [([], "1800"), (["DEEP_SECONDS=2400"], "2400")]:
            result = subprocess.run(["make", "--no-print-directory", "-n", "-C", str(ROOT),
                                     "deep", "DCHECK=checker", *settings],
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('--seconds "' + seconds + '" -- "checker" deep', result.stdout)
        ci = (ROOT / "ci/verify.sh").read_text()
        self.assertIn("deep DEEP_SECONDS=2400", ci)
        self.assertIn("VERIFY_REQUIRE_SCOPE=1", ci)

    def test_bad_arguments_never_start_a_command(self):
        for arguments in [[], ["--seconds", "0", "--", "touch", "never"]]:
            result = subprocess.run([sys.executable, str(ROOT / "tools/verify_deep.py"), *arguments],
                                    capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 2)


def scope_unavailable(reason):
    if os.environ.get("VERIFY_REQUIRE_SCOPE") == "1":
        raise RuntimeError("required live scope tests unavailable: " + reason)
    raise unittest.SkipTest(reason)


class ScopeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if sys.platform != "linux" or not Path("/sys/fs/cgroup/cgroup.controllers").exists():
            scope_unavailable("Linux cgroup v2 required")
        try:
            result = subprocess.run(["systemctl", "--user", "show-environment"],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
        except (OSError, subprocess.SubprocessError) as error:
            scope_unavailable("user systemd unavailable: " + str(error))
        if result.returncode:
            scope_unavailable("user systemd unavailable")
        cls.libc = ctypes.CDLL(None, use_errno=True)
        if not hasattr(cls.libc, "pidfd_open") or not hasattr(cls.libc, "pidfd_send_signal"):
            scope_unavailable("libc pidfd functions required for identity-safe test cleanup")
        probe = cls.libc.pidfd_open(os.getpid(), 0)
        if probe < 0:
            scope_unavailable("kernel pidfd support required for test cleanup")
        os.close(probe)
        # Keep orphan PIDs owned by this test until reaped, avoiding PID reuse.
        if cls.libc.prctl(36, 1, 0, 0, 0) != 0:
            raise RuntimeError("cannot enable test subreaper")

    def open_pidfd(self):
        fd = self.libc.pidfd_open(self.pid, 0)
        if fd < 0:
            raise OSError(ctypes.get_errno(), "pidfd_open")
        return fd

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        self.pid = None
        self.pidfd = None
        self.reaped = False
        self.addCleanup(self.cleanup_child)
        child = self.work / "child.py"
        child.write_text("import os,signal,time\nfrom pathlib import Path\n"
                         "signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
                         f"Path({str(self.work / 'pending')!r}).write_text(str(os.getpid()))\n"
                         f"os.replace({str(self.work / 'pending')!r}, {str(self.work / 'ready')!r})\n"
                         "time.sleep(60)\n")
        parent = self.work / "parent.py"
        parent.write_text("import subprocess,sys,time\nfrom pathlib import Path\n"
                          f"subprocess.Popen([sys.executable,{str(child)!r}], start_new_session=True, "
                          "stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)\n"
                          f"while not Path({str(self.work / 'ready')!r}).exists(): time.sleep(.01)\n"
                          "if len(sys.argv)==1: time.sleep(60)\n")
        self.parent = parent

    def cleanup_child(self):
        if self.pid is None and (self.work / "ready").exists():
            self.pid = int((self.work / "ready").read_text())
        if self.pid is not None and not self.reaped:
            # pidfd keeps cleanup tied to this child even before orphan adoption.
            try:
                if self.pidfd is None:
                    self.pidfd = self.open_pidfd()
                if self.libc.pidfd_send_signal(self.pidfd, signal.SIGKILL, None, 0) != 0:
                    if ctypes.get_errno() != 3:  # ESRCH: already stopped.
                        raise OSError(ctypes.get_errno(), "pidfd_send_signal")
            except ProcessLookupError:
                pass
            try:
                os.waitpid(self.pid, 0)
            except ChildProcessError:
                pass
        if self.pidfd is not None:
            os.close(self.pidfd)

    def start(self, orphan=False, seconds=2, runner=None):
        output = (self.work / "output").open("wb")
        self.addCleanup(output.close)
        process = subprocess.Popen([sys.executable, str(runner or RUNNER), "--seconds", str(seconds), "--",
                                    sys.executable, str(self.parent), *(["orphan"] if orphan else [])],
                                   stdout=output, stderr=output)
        def stop_launcher():
            if process.poll() is None:
                process.kill()
            process.wait(timeout=5)
        self.addCleanup(stop_launcher)
        deadline = time.monotonic() + 5
        while not (self.work / "ready").exists() and time.monotonic() < deadline and process.poll() is None:
            time.sleep(.02)
        self.assertTrue((self.work / "ready").exists(), (self.work / "output").read_text())
        self.pid = int((self.work / "ready").read_text())
        self.pidfd = self.open_pidfd()
        return process

    def assert_child_stopped(self):
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            try:
                pid, status = os.waitpid(self.pid, os.WNOHANG)
            except ChildProcessError:
                pid = 0  # Still owned by its live parent until the scope stops.
            if pid == self.pid:
                self.reaped = True
                self.assertTrue(os.WIFSIGNALED(status), status)
                self.assertEqual(os.WTERMSIG(status), signal.SIGKILL)
                return
            time.sleep(.02)
        self.fail("detached SIGTERM-ignoring descendant survived verification limit")

    def test_deadline_cleans_detached_term_ignoring_child(self):
        process = self.start()
        self.assertNotEqual(process.wait(timeout=8), 0)
        self.assert_child_stopped()

    def test_signal_before_spawn_handle_is_published_cleans_scope(self):
        wrapper = self.work / "early-signal.py"
        wrapper.write_text(
            "import importlib.util,os,signal,sys,time\nfrom pathlib import Path\n"
            f"spec=importlib.util.spec_from_file_location('early_verify',{str(RUNNER)!r})\n"
            "module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)\n"
            "original=module.subprocess.Popen\n"
            "def spawn(*args,**kwargs):\n"
            "    child=original(*args,**kwargs)\n"
            "    module.subprocess.Popen=original\n"
            "    deadline=time.monotonic()+5\n"
            f"    while not Path({str(self.work / 'ready')!r}).exists() and time.monotonic()<deadline: time.sleep(.01)\n"
            f"    if not Path({str(self.work / 'ready')!r}).exists(): raise RuntimeError('child setup failed')\n"
            "    os.kill(os.getpid(),signal.SIGTERM)\n"
            "    return child\n"
            "module.subprocess.Popen=spawn\nsys.exit(module.main())\n")
        process = self.start(seconds=10, runner=wrapper)
        self.assertEqual(process.wait(timeout=3), 128 + signal.SIGTERM)
        self.assert_child_stopped()

    def test_scope_deadline_survives_controller_sigkill(self):
        process = self.start()
        process.kill()
        self.assertEqual(process.wait(timeout=5), -signal.SIGKILL)
        self.assert_child_stopped()

    def test_sigterm_stops_scope_without_waiting_for_runtime_cap(self):
        process = self.start(seconds=10)
        process.terminate()
        self.assertEqual(process.wait(timeout=5), 128 + signal.SIGTERM)
        self.assert_child_stopped()

    def test_sighup_stops_scope_without_waiting_for_runtime_cap(self):
        previous = signal.signal(signal.SIGHUP, signal.SIG_DFL)
        try:
            process = self.start(seconds=10)
        finally:
            signal.signal(signal.SIGHUP, previous)
        process.send_signal(signal.SIGHUP)
        self.assertEqual(process.wait(timeout=3), 128 + signal.SIGHUP)
        self.assert_child_stopped()

    def test_nohup_ignore_survives_in_runner_and_checker(self):
        previous = signal.signal(signal.SIGHUP, signal.SIG_IGN)
        try:
            process = self.start(seconds=10)
        finally:
            signal.signal(signal.SIGHUP, previous)
        process.send_signal(signal.SIGHUP)
        self.assertEqual(self.libc.pidfd_send_signal(self.pidfd, signal.SIGHUP, None, 0), 0)
        time.sleep(0.1)
        self.assertIsNone(process.poll())
        process.terminate()
        self.assertEqual(process.wait(timeout=3), 128 + signal.SIGTERM)
        self.assert_child_stopped()

    def test_inheritable_descriptor_survives_scope(self):
        with tempfile.TemporaryFile() as stream:
            stream.write(b"inherited lock descriptor\n")
            stream.seek(0)
            os.set_inheritable(stream.fileno(), True)
            result = subprocess.run([sys.executable, str(RUNNER), "--seconds", "5", "--",
                                     sys.executable, "-c", "import os,sys; os.write(1,os.read(int(sys.argv[1]),64))",
                                     str(stream.fileno())], close_fds=False,
                                    capture_output=True, timeout=8)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, b"inherited lock descriptor\n")

    def test_cloexec_descriptor_does_not_survive_scope(self):
        with tempfile.TemporaryFile() as stream:
            info = os.fstat(stream.fileno())
            self.assertFalse(os.get_inheritable(stream.fileno()))
            probe = ("import os,sys\ntry: s=os.fstat(int(sys.argv[1]))\n"
                     "except OSError: sys.exit(0)\n"
                     "sys.exit(83 if (s.st_dev,s.st_ino)==tuple(map(int,sys.argv[2:])) else 0)")
            result = subprocess.run([sys.executable, str(RUNNER), "--seconds", "5", "--",
                                     sys.executable, "-c", probe, str(stream.fileno()),
                                     str(info.st_dev), str(info.st_ino)], close_fds=False,
                                    capture_output=True, timeout=8)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_literal_argument_roundtrip(self):
        arguments = ["$HOME", "${USER}", "$$", "$__CU_VERIFY_LITERAL_MISSING__",
                     "%n", "%%", "$(not-a-command)", "; not a shell", "space here", ""]
        result = subprocess.run([sys.executable, str(RUNNER), "--seconds", "5", "--",
                                 sys.executable, "-c", "import json,sys; print(json.dumps(sys.argv[1:]))",
                                 *arguments], capture_output=True, text=True, timeout=8)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), arguments)

    def test_make_deep_preserves_failure(self):
        checker = self.work / "checker"
        checker.write_text('#!/bin/sh\n[ "$1" = deep ] || exit 99\nexit 7\n')
        checker.chmod(0o755)
        result = subprocess.run(["make", "--no-print-directory", "-C", str(ROOT), "deep",
                                 "DCHECK=" + str(checker), "DEEP_SECONDS=2"],
                                capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Error 7", result.stderr)

    def test_success_with_orphan_is_failure_and_cleans(self):
        process = self.start(orphan=True, seconds=10)
        self.assertEqual(process.wait(timeout=3), 1)
        self.assertIn("left processes running; scope stopped", (self.work / "output").read_text())
        self.assert_child_stopped()


if __name__ == "__main__":
    unittest.main()
