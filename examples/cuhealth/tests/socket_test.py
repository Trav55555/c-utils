"""Finite loopback tests; subprocesses are test infrastructure, never service jobs."""
import email.utils
import errno
import http.client
import io
import json
from pathlib import Path
import signal
import select
import os
import atexit
import socket
import subprocess
import sys
import time

EXE = str(Path(sys.argv[1]).resolve())


owned = []


def cleanup():
    for p in owned:
        if p.poll() is None:
            p.kill()
        p.communicate(timeout=3)


atexit.register(cleanup)


def start(*args):
    p = subprocess.Popen([EXE, *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    owned.append(p)
    try:
        deadline = time.monotonic() + 3
        line = b''
        while not line.endswith(b'\n'):
            remaining = deadline - time.monotonic()
            assert remaining > 0 and select.select([p.stdout], [], [], remaining)[0], 'readiness timeout'
            byte = os.read(p.stdout.fileno(), 1)
            assert byte and len(line) < 64, 'invalid readiness output'
            line += byte
        assert line.startswith(b'127.0.0.1:'), line
        port = int(line.split(b':')[1])
        assert 0 < port <= 65535
        return p, port
    except BaseException:
        if p.poll() is None:
            p.kill()
        p.communicate(timeout=3)
        raise


def exchange(port, request, pause=0):
    with socket.create_connection(('127.0.0.1', port), timeout=3) as s:
        s.sendall(request)
        if pause:
            time.sleep(pause)
        try:
            s.shutdown(socket.SHUT_WR)
        except OSError as exc:
            # Early rejection may close the peer before our half-close. Still
            # collect the reply: callers must validate it, not assume success.
            if exc.errno != errno.ENOTCONN:
                raise
        data = b''
        while True:
            try:
                part = s.recv(4096)
            except ConnectionResetError:
                break  # Rejecting without draining can reset unread pipelined input.
            if not part:
                return data
            data += part
        return data


class MemorySocket:
    def __init__(self, wire):
        self.wire = wire

    def makefile(self, *args):
        return io.BytesIO(self.wire)


def parse(wire, method='GET'):
    r = http.client.HTTPResponse(MemorySocket(wire), method=method)
    r.begin()
    body = r.read()
    assert r.getheader('Connection') == 'close'
    assert email.utils.parsedate_to_datetime(r.getheader('Date')).utcoffset().total_seconds() == 0
    assert wire.count(b'HTTP/1.1 ') == 1
    return r, body


def test_exchange():
    """Model early rejection deterministically, not a kernel timing guarantee."""
    from unittest.mock import patch

    good = (b'HTTP/1.1 400 Bad Request\r\nConnection: close\r\n'
            b'Date: Thu, 01 Jan 1970 00:00:00 GMT\r\n'
            b'Content-Length: 12\r\n\r\nbad request\n')

    class Peer:
        def __init__(self, wire, error):
            self.wire = wire
            self.error = error

        def __enter__(self):
            return self

        def __exit__(self, *args):
            pass

        def sendall(self, request):
            assert request == b'request'

        def shutdown(self, how):
            assert how == socket.SHUT_WR
            if self.error is not None:
                raise OSError(self.error, 'modeled half-close failure')

        def recv(self, size):
            assert size == 4096
            if self.wire is None:
                raise ConnectionResetError(errno.ECONNRESET, 'modeled peer reset')
            wire, self.wire = self.wire, None
            return wire

    def check(wire, error):
        with patch.object(socket, 'create_connection', return_value=Peer(wire, error)):
            response, body = parse(exchange(1, b'request'))
        assert response.status == 400 and body == b'bad request\n'
        assert int(response.getheader('Content-Length')) == len(body)

    for error in (None, errno.ENOTCONN):
        check(good, error)
    try:
        check(good, errno.EPERM)
    except OSError as exc:
        assert exc.errno == errno.EPERM
    else:
        raise AssertionError('unexpected shutdown error was swallowed')
    for wire in (b'', good[:-1], good.replace(b'400 Bad Request', b'200 OK'),
                 good.replace(b'bad request\n', b'wrong reply\n'),
                 good.replace(b'Connection: close', b'Connection: alive')):
        try:
            check(wire, errno.ENOTCONN)
        except (AssertionError, http.client.HTTPException):
            pass
        else:
            raise AssertionError('invalid rejection response was accepted')


def run():
    p, port = start('--requests', '8', '--timeout-ms', '300')
    try:
        for method, path, status in [('GET', '/health', 200), ('HEAD', '/health', 200),
                                     ('GET', '/status', 200), ('HEAD', '/status', 200),
                                     ('GET', '/unknown', 404), ('POST', '/health', 405),
                                     ('GET', '/health?x=1', 404), ('GET', '/%68ealth', 404)]:
            wire = exchange(port, f'{method} {path} HTTP/1.1\r\nHost: local\r\n\r\n'.encode())
            r, body = parse(wire, method)
            # The worker may still be checking on slow/instrumented builds.
            assert r.status == status or (path == '/status' and r.status == 503)
            if method == 'HEAD':
                assert wire.endswith(b'\r\n\r\n') and body == b''
                assert int(r.getheader('Content-Length')) > 0
            else:
                assert int(r.getheader('Content-Length')) == len(body)
                if path == '/health' and r.status == 200:
                    assert body == b'ok\n'
                if path == '/status':
                    assert json.loads(body)['status'] in ('checking', 'ready', 'failed')
            if status == 405:
                assert r.getheader('Allow') == 'GET, HEAD'
                assert body == b'method not allowed\n'
        assert p.wait(timeout=3) == 0
    finally:
        if p.poll() is None:
            p.kill()
        p.communicate(timeout=3)

    bad = [b'GET /health HTTP/1.1\r\n\r\n',
           b'GET /health HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx',
           b'GET /health HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n',
           b'GET /health HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n',
           b'GET /health HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\n\r\n',
           b'GET /health HTTP/1.1\nHost: x\n\n',
           b'GET /health HTTP/1.1\r\nHost: x\r\nX: ' + b'a' * 2048 + b'\r\n\r\n',
           b'GET /health HTTP/1.1\r\nHost: x\r\n' + b'X: a\r\n' * 16 + b'\r\n']
    p, port = start('--requests', str(len(bad)), '--timeout-ms', '300')
    try:
        for request in bad:
            r, body = parse(exchange(port, request))
            assert r.status == 400 and body == b'bad request\n'
        assert p.wait(timeout=3) == 0
    finally:
        if p.poll() is None:
            p.kill()
        p.communicate(timeout=3)

    p, port = start('--requests', '1', '--timeout-ms', '300')
    one = b'GET /health HTTP/1.1\r\nHost: x\r\n\r\n'
    wire = exchange(port, one + one)
    r, body = parse(wire)
    assert r.status == 200 and body == b'ok\n'
    assert len(wire.split(b'\r\n\r\n', 1)[1]) == 3
    assert p.wait(timeout=3) == 0
    p.communicate(timeout=3)

    p, port = start('--requests', '1', '--timeout-ms', '300')
    wire = exchange(port, b'HEAD /health HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\n\r\n')
    r, body = parse(wire, 'HEAD')
    assert r.status == 400 and body == b'' and wire.endswith(b'\r\n\r\n')
    assert int(r.getheader('Content-Length')) == len(b'bad request\n')
    assert p.wait(timeout=3) == 0
    p.communicate(timeout=3)

    p, port = start('--requests', '1', '--timeout-ms', '100')
    before = time.monotonic()
    wire = exchange(port, b'GET /health HTTP/1.1\r\nHost:', pause=0.2)
    # Sticky reader timeout may prevent sending 400; close is required regardless.
    assert wire == b'' or parse(wire)[0].status == 400
    assert p.wait(timeout=3) == 0 and time.monotonic() - before < 3
    p.communicate(timeout=3)

    # Readiness publication failure must join the active worker, not SIGPIPE.
    p = subprocess.Popen([EXE, '--rounds', '1000000'], stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE)
    owned.append(p)
    p.stdout.close()
    assert p.wait(timeout=3) == 1
    p.communicate(timeout=3)

    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        p, port = start('--rounds', '1000000', '--timeout-ms', '1000')
        p.send_signal(sig)
        assert p.wait(timeout=3) == 0
        p.communicate(timeout=3)
    p, port = start('--timeout-ms', '60')
    assert p.wait(timeout=3) == 0  # Idle one-shot expiry.
    p.communicate(timeout=3)
    for args in (['--port', '65536'], ['--rounds', '1000001'], ['--requests', '0'],
                 ['--timeout-ms', '0'], ['--rounds', '-1'], ['--port'], ['--wat', '1']):
        assert subprocess.run([EXE, *args], capture_output=True, timeout=3).returncode == 2
    print('cuhealth socket: pass')


if __name__ == '__main__':
    test_exchange()
    run()
