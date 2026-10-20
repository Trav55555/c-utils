"""Regressions for signal and error paths that must still stop the owned scope."""
import importlib.util
import os
from pathlib import Path
import signal
import subprocess
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('verify_cleanup', ROOT / 'tools/verify_deep.py')
VERIFY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFY)


class CleanupTests(unittest.TestCase):
    def test_signal_inside_spawn_is_deferred_until_handle_is_owned(self):
        class EarlySignal(Exception):
            pass
        def old_handler(_signum, _frame):
            raise EarlySignal()
        old = signal.signal(signal.SIGTERM, old_handler)
        self.addCleanup(signal.signal, signal.SIGTERM, old)
        launcher = mock.Mock()
        launcher.poll.return_value = None
        launcher.wait.return_value = 0
        def spawn(*_args, **_kwargs):
            os.kill(os.getpid(), signal.SIGTERM)
            return launcher
        with mock.patch.object(VERIFY.subprocess, 'Popen', side_effect=spawn), \
                mock.patch.object(VERIFY.subprocess, 'run') as stop, \
                mock.patch.object(VERIFY, 'populated', side_effect=[True, False]):
            stop.return_value.returncode = 0
            self.assertEqual(VERIFY.run(['checker'], 10), 128 + signal.SIGTERM)
            launcher.kill.assert_called_once()
            self.assertEqual(stop.call_count, 1)
        self.assertIs(signal.getsignal(signal.SIGTERM), old_handler)

    def test_inherited_sighup_ignore_is_preserved(self):
        previous = signal.signal(signal.SIGHUP, signal.SIG_IGN)
        self.addCleanup(signal.signal, signal.SIGHUP, previous)
        launcher = mock.Mock()
        launcher.poll.return_value = 0
        launcher.wait.return_value = 0
        def spawn(*_args, **_kwargs):
            self.assertEqual(signal.getsignal(signal.SIGHUP), signal.SIG_IGN)
            os.kill(os.getpid(), signal.SIGHUP)
            return launcher
        with mock.patch.object(VERIFY.subprocess, 'Popen', side_effect=spawn), \
                mock.patch.object(VERIFY, 'cleanup_scope', return_value=False):
            self.assertEqual(VERIFY.run(['checker'], 1), 0)
        self.assertEqual(signal.getsignal(signal.SIGHUP), signal.SIG_IGN)

    def test_failed_reap_still_attempts_scope_stop(self):
        def wait(timeout):
            if timeout == 10:
                raise subprocess.TimeoutExpired('launcher', 10)
            return 0
        with mock.patch.object(VERIFY.subprocess, 'Popen') as popen, \
                mock.patch.object(VERIFY.subprocess, 'run') as stop, \
                mock.patch.object(VERIFY, 'populated', side_effect=[True, False]):
            popen.return_value.wait.side_effect = wait
            popen.return_value.poll.return_value = None
            stop.return_value.returncode = 0
            with self.assertRaises(subprocess.TimeoutExpired):
                VERIFY.run(['checker'], 1)
            self.assertEqual(stop.call_count, 1)

    def test_failed_inspection_still_attempts_scope_stop(self):
        with mock.patch.object(VERIFY.subprocess, 'Popen') as popen, \
                mock.patch.object(VERIFY.subprocess, 'run') as stop, \
                mock.patch.object(VERIFY, 'populated', side_effect=RuntimeError('inspection unavailable')):
            popen.return_value.wait.return_value = 0
            popen.return_value.poll.return_value = 0
            stop.return_value.returncode = 0
            with self.assertRaisesRegex(RuntimeError, 'inspection unavailable'):
                VERIFY.run(['checker'], 1)
            self.assertEqual(stop.call_count, 1)

    def test_handlers_and_mask_restored_after_cleanup_failure(self):
        signals = [signal.SIGHUP, signal.SIGINT, signal.SIGTERM]
        handlers = {sig: signal.getsignal(sig) for sig in signals}
        mask = signal.pthread_sigmask(signal.SIG_BLOCK, set())
        with mock.patch.object(VERIFY.subprocess, 'Popen') as popen, \
                mock.patch.object(VERIFY.subprocess, 'run', side_effect=OSError('stop unavailable')), \
                mock.patch.object(VERIFY, 'populated', return_value=True):
            popen.return_value.wait.return_value = 0
            popen.return_value.poll.return_value = 0
            with self.assertRaises(OSError):
                VERIFY.run(['checker'], 1)
        self.assertEqual({sig: signal.getsignal(sig) for sig in signals}, handlers)
        self.assertEqual(signal.pthread_sigmask(signal.SIG_BLOCK, set()), mask)


if __name__ == '__main__':
    unittest.main()
