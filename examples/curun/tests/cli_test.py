"""Drive the real CLI; bytes, limits, deadlines and child ownership are oracles."""
import ctypes
import errno
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

BINARY = Path(sys.argv.pop(1)).resolve()


class CommandTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if sys.platform != 'linux':
            raise unittest.SkipTest('curun communication backend is Linux-only')
        if ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0) != 0:
            raise RuntimeError('cannot enable subreaper oracle')

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='curun test ')
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        self.marker = self.work / 'child.pid'
        self.fixture = self.work / 'child.py'
        self.fixture.write_text('''import json,os,signal,sys,time
from pathlib import Path
mode=sys.argv[1]
if mode=='args':
    os.write(1,json.dumps(sys.argv[2:]).encode())
elif mode=='env':
    os.write(1,os.environ['CURUN_TEST_VALUE'].encode())
elif mode=='emit':
    os.write(1,bytes.fromhex(sys.argv[2]));os.write(2,bytes.fromhex(sys.argv[3]))
elif mode=='flood':
    import fcntl
    assert fcntl.fcntl(1,fcntl.F_SETPIPE_SZ,4096)<131089
    assert fcntl.fcntl(2,fcntl.F_SETPIPE_SZ,4096)<147487
    a=bytes((i*17+3)%256 for i in range(131089))
    b=bytes((i*31+9)%256 for i in range(147487))
    os.write(1,a);os.write(2,b)
    assert os.read(0,1)==b''
elif mode=='exit':
    sys.exit(int(sys.argv[2]))
elif mode=='signal':
    os.kill(os.getpid(),signal.SIGTERM)
else:
    signal.signal(signal.SIGTERM,signal.SIG_IGN)
    if mode=='delayed-close':time.sleep(.6)
    if mode in ('closed','delayed-close'):
        os.close(1);os.close(2)
    p=Path(sys.argv[2]);p.with_suffix('.tmp').write_text(str(os.getpid()))
    p.with_suffix('.tmp').replace(p)
    time.sleep(60)
''')

    def command(self, mode, *args, options=()):
        return [str(BINARY), *options, '--', sys.executable, str(self.fixture), mode, *map(str, args)]

    def run_cli(self, command, **kwargs):
        return subprocess.run(command, capture_output=True, timeout=6, **kwargs)

    def broken_pipe(self):
        reader, writer = os.pipe()
        os.close(reader)
        self.addCleanup(os.close, writer)
        return writer

    def test_literal_arguments_and_environment(self):
        args = ['', 'space here', ';echo bad', '$HOME', '*', 'line\nbreak', '--report']
        result = self.run_cli(self.command('args', *args))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), args)
        self.assertEqual(result.stderr, b'')
        result = self.run_cli(self.command('env'), env={**os.environ, 'CURUN_TEST_VALUE': 'exact value'})
        self.assertEqual((result.returncode, result.stdout, result.stderr), (0, b'exact value', b''))

    def test_binary_exact_limits_and_zero(self):
        out, err = b'a\x00\xff\n', b'error\x00'
        result = self.run_cli(self.command('emit', out.hex(), err.hex(), options=['--stdout-limit','4','--stderr-limit','6']))
        self.assertEqual((result.returncode, result.stdout, result.stderr), (0, out, err))
        result = self.run_cli(self.command('emit', '', '', options=['--stdout-limit','0','--stderr-limit','0']))
        self.assertEqual((result.returncode, result.stdout, result.stderr), (0, b'', b''))
        result = self.run_cli(self.command('emit', '01', '', options=['--stdout-limit','0']))
        self.assertEqual(result.returncode, 125)
        self.assertEqual(result.stdout, b'')
        self.assertIn(b'capture limit exceeded', result.stderr)

    def test_each_stream_overflow_reports_failure_and_prefix(self):
        result = self.run_cli(self.command('emit', '0001020304', '', options=['--stdout-limit','4']))
        self.assertEqual((result.returncode, result.stdout), (125, b'\x00\x01\x02\x03'))
        self.assertIn(b'capture limit exceeded', result.stderr)
        result = self.run_cli(self.command('emit', '', '0102030405', options=['--stderr-limit','4']))
        self.assertEqual(result.returncode, 125)
        self.assertTrue(result.stderr.endswith(b'\x01\x02\x03\x04'), result.stderr)

    def test_dual_pipe_flood_and_stdin_eof(self):
        result = self.run_cli(self.command('flood', options=['--timeout-ms','3000']), input=b'parent input must not reach the child')
        self.assertEqual(result.returncode, 0, result.stderr[-300:])
        self.assertEqual(result.stdout, bytes((i*17+3)%256 for i in range(131089)))
        self.assertEqual(result.stderr, bytes((i*31+9)%256 for i in range(147487)))

    def test_exit_and_signal_reports(self):
        for code in [0,7,124,125,200]:
            result = self.run_cli(self.command('exit', code, options=['--report']))
            self.assertEqual(result.returncode, code)
            self.assertEqual(result.stderr, f'curun: child exited {code}\n'.encode())
        result = self.run_cli(self.command('signal', options=['--report']))
        self.assertEqual(result.returncode, 128+signal.SIGTERM)
        self.assertEqual(result.stderr, f'curun: child signal {signal.SIGTERM}\n'.encode())

    def cleanup_process(self, process):
        if process.poll() is None:
            process.kill()
        process.communicate(timeout=5)
        if self.marker.exists():
            pid = int(self.marker.read_text())
            try:
                owned, _status = os.waitpid(pid, os.WNOHANG)
            except ChildProcessError:
                return
            if owned == 0:
                # Adopted, unreaped direct child; its PID cannot be reused.
                os.kill(pid, signal.SIGKILL)
                os.waitpid(pid, 0)

    def start_blocked(self, mode, timeout_ms):
        process = subprocess.Popen(self.command(mode, self.marker, options=['--timeout-ms',str(timeout_ms),'--report']),
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.cleanup_process, process)
        end = time.monotonic()+3
        while not self.marker.exists() and process.poll() is None and time.monotonic()<end:
            time.sleep(.005)
        self.assertTrue(self.marker.exists(), 'child did not become ready')
        return process

    def assert_reaped(self):
        pid = int(self.marker.read_text())
        with self.assertRaises(ChildProcessError, msg='CLI left an adopted live or zombie child'):
            os.waitpid(pid, os.WNOHANG)

    def test_timeout_reaps_while_streams_remain_open(self):
        process = self.start_blocked('open', 300)
        out, err = process.communicate(timeout=3)
        self.assertEqual((process.returncode,out), (124,b''))
        self.assertIn(b'deadline exceeded',err)
        self.assertIn(f'child signal {signal.SIGKILL}'.encode(),err)
        self.assert_reaped()

    def test_one_budget_includes_wait_after_stream_eof(self):
        start = time.monotonic()
        process = self.start_blocked('delayed-close', 1000)
        _out, err = process.communicate(timeout=3)
        elapsed = time.monotonic()-start
        self.assertEqual(process.returncode,124,err)
        self.assertLess(elapsed,1.45,'wait restarted a full timeout after communication')
        self.assert_reaped()

    def test_cancel_during_wait_and_communication(self):
        for mode in ['closed','open']:
            with self.subTest(mode=mode):
                self.marker.unlink(missing_ok=True)
                process = self.start_blocked(mode,500)
                process.terminate()
                _out, err = process.communicate(timeout=3)
                self.assertEqual(process.returncode,128+signal.SIGTERM,err)
                self.assertIn(b'interrupted by signal',err)
                self.assertNotIn(b'deadline exceeded',err)
                self.assert_reaped()

    def test_output_failure_after_child_reaping(self):
        if not Path('/dev/full').exists():
            self.skipTest('/dev/full unavailable')
        with open('/dev/full','wb') as output:
            result = subprocess.run(self.command('emit','010203',''),stdout=output,stderr=subprocess.PIPE,timeout=4)
        self.assertEqual(result.returncode,125)
        self.assertIn(b'output failed',result.stderr)
        result = subprocess.run(self.command('emit','010203',''),stdout=self.broken_pipe(),stderr=subprocess.PIPE,timeout=4)
        self.assertEqual(result.returncode,125,result.stderr)
        self.assertIn(b'output failed',result.stderr)
        result = subprocess.run(self.command('emit','010203','0405'),stdout=self.broken_pipe(),stderr=self.broken_pipe(),timeout=4)
        self.assertEqual(result.returncode,125)

    def test_report_only_output_failure(self):
        if not Path('/dev/full').exists():
            self.skipTest('/dev/full unavailable')
        with open('/dev/full','wb') as error:
            result = subprocess.run(self.command('exit',0,options=['--report']),stdout=subprocess.PIPE,stderr=error,timeout=4)
        self.assertEqual(result.returncode,125)

    def test_closed_help_and_usage_pipes(self):
        result = subprocess.run([str(BINARY),'--help'],stdout=self.broken_pipe(),stderr=subprocess.PIPE,timeout=4)
        self.assertEqual(result.returncode,125,result.stderr)
        result = subprocess.run([str(BINARY),'--bad-option'],stdout=subprocess.PIPE,stderr=self.broken_pipe(),timeout=4)
        self.assertEqual(result.returncode,125)

    def test_usage_boundaries_and_start_failure(self):
        for options in [[],['--'],['--timeout-ms','0','--','/not-run'],['--timeout-ms','-1','--','/not-run'],
                        ['--stdout-limit','16777217','--','/not-run'],['--stderr-limit','9999999999999999999999','--','/not-run'],
                        ['--','bare-command']]:
            result = self.run_cli([str(BINARY),*options])
            self.assertEqual(result.returncode,2,(options,result.stderr))
        result = self.run_cli([str(BINARY),'--report','--','/missing/curun-program'])
        self.assertIn(result.returncode,[125,127])
        self.assertIn(b'start failed' if result.returncode==125 else b'child exited 127',result.stderr)
        result = self.run_cli(self.command('exit',0,options=['--stdout-limit','16777216','--stderr-limit','16777216']))
        self.assertEqual(result.returncode,0,result.stderr)


if __name__=='__main__':
    unittest.main()
