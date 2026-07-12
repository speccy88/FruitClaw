#!/usr/bin/env python3
"""Exercise the NuttX network profile services from a host on the same LAN."""

from __future__ import annotations

import argparse
import concurrent.futures
import ftplib
import hashlib
import http.client
import io
import os
import socket
import sys
import threading
import time
from dataclasses import dataclass


@dataclass
class Result:
    name: str
    seconds: float
    detail: str


class AuditFailure(RuntimeError):
    pass


EXPECTED_ERRORS = (AuditFailure, OSError) + ftplib.all_errors


def timed(name: str, operation) -> Result:
    print(f"RUN   {name}", flush=True)
    started = time.monotonic()
    detail = operation()
    result = Result(name, time.monotonic() - started, detail or "ok")
    print(
        f"PASS  {result.name:<32} {result.seconds:7.3f}s  {result.detail}",
        flush=True,
    )
    return result


def http_request(
    host: str,
    port: int,
    method: str,
    path: str,
    timeout: float,
    body: bytes | None = None,
) -> tuple[int, bytes]:
    connection = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        connection.request(method, path, body=body, headers={"Connection": "close"})
        response = connection.getresponse()
        return response.status, response.read()
    finally:
        connection.close()


def http_get(host: str, port: int, path: str, timeout: float) -> tuple[int, bytes]:
    return http_request(host, port, "GET", path, timeout)


def audit_http(
    args: argparse.Namespace,
    payload: bytes,
    remote_name: str,
    check_ftp_visibility: bool,
) -> list[Result]:
    results: list[Result] = []

    def empty_probe() -> str:
        with socket.create_connection((args.host, args.http_port), args.timeout):
            pass
        status, body = http_get(args.host, args.http_port, "/", args.timeout)
        if status != 200 or not body:
            raise AuditFailure(f"HTTP died after empty probe: status={status} bytes={len(body)}")
        return f"status={status} bytes={len(body)}"

    results.append(timed("http empty-client survival", empty_probe))

    def confinement() -> str:
        for path in ("/../wifi.conf", "/%2e%2e/wifi.conf"):
            status, body = http_get(args.host, args.http_port, path, args.timeout)
            if status == 200:
                raise AuditFailure(f"HTTP path escaped document root: {path}")
            if b"ssid=" in body.lower() or b"password=" in body.lower():
                raise AuditFailure(f"HTTP returned credential-like data for {path}")
        return "direct and encoded parent traversal rejected"

    results.append(timed("http document-root confinement", confinement))

    def methods() -> str:
        for method in ("DELETE", "OPTIONS", "PUT", "POST"):
            status, _ = http_request(
                args.host, args.http_port, method, "/index.html", args.timeout, b"audit"
            )
            if status < 400:
                raise AuditFailure(f"HTTP {method} unexpectedly returned {status}")
        return "DELETE, OPTIONS, PUT, and POST rejected"

    results.append(timed("http static-method enforcement", methods))

    def query_and_exact_path() -> str:
        status, queried = http_get(
            args.host, args.http_port, "/?network-audit=1", args.timeout
        )
        if status != 200 or not queried:
            raise AuditFailure(f"root query failed: status={status} bytes={len(queried)}")
        status, _ = http_get(
            args.host, args.http_port, "/index.html.not-a-file", args.timeout
        )
        if status != 404:
            raise AuditFailure(f"nonexistent path returned HTTP {status}")
        return "query ignored for lookup and filename match remained exact"

    results.append(timed("http query and exact lookup", query_and_exact_path))

    def head_request() -> str:
        connection = http.client.HTTPConnection(
            args.host, args.http_port, timeout=args.timeout
        )
        try:
            connection.request("HEAD", "/", headers={"Connection": "close"})
            response = connection.getresponse()
            body = response.read()
            length = response.getheader("Content-Length")
            if response.status != 200 or body or length is None:
                raise AuditFailure(
                    f"HEAD failed: status={response.status} "
                    f"body={len(body)} content-length={length!r}"
                )
            return f"status=200 content-length={length} body=0"
        finally:
            connection.close()

    results.append(timed("http HEAD", head_request))

    def stalled_peer() -> str:
        stalled = socket.create_connection((args.host, args.http_port), args.timeout)
        try:
            status, body = http_get(args.host, args.http_port, "/", args.timeout)
            if status != 200 or not body:
                raise AuditFailure(f"parallel request failed: status={status} bytes={len(body)}")
        finally:
            stalled.close()
        return "valid request completed while another client sent no request"

    results.append(timed("http stalled-peer isolation", stalled_peer))

    def one_request(_: int) -> int:
        status, body = http_get(args.host, args.http_port, "/", args.timeout)
        if status != 200 or not body:
            raise AuditFailure(f"status={status} bytes={len(body)}")
        return len(body)

    def concurrent_requests() -> str:
        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as executor:
            sizes = list(executor.map(one_request, range(100)))
        return f"100 requests, {sum(sizes)} response bytes"

    results.append(timed("http concurrent requests", concurrent_requests))

    def ftp_visibility() -> str:
        status, body = http_get(
            args.host, args.http_port, "/" + remote_name, args.timeout
        )
        if status != 200:
            raise AuditFailure(f"uploaded file returned HTTP {status}")
        if body != payload:
            raise AuditFailure(
                f"uploaded file mismatch: expected={len(payload)} actual={len(body)}"
            )
        return f"status={status} sha256={hashlib.sha256(body).hexdigest()}"

    if check_ftp_visibility:
        results.append(timed("http serves FTP upload", ftp_visibility))
    return results


def ftp_connect(args: argparse.Namespace) -> ftplib.FTP:
    ftp = ftplib.FTP()
    ftp.connect(args.host, args.ftp_port, timeout=args.timeout)
    ftp.login(args.ftp_user, args.ftp_password)
    return ftp


def ftp_passive_endpoint(ftp: ftplib.FTP, command: str) -> tuple[str, int]:
    if ftp.sock is None:
        raise AuditFailure("FTP control socket is not connected")

    peer = ftp.sock.getpeername()
    if command == "PASV":
        _, port = ftplib.parse227(ftp.sendcmd("PASV"))
        return peer[0], port
    if command == "EPSV":
        return ftplib.parse229(ftp.sendcmd("EPSV"), peer)
    raise AuditFailure(f"unsupported passive command: {command}")


def ftp_early_close_store(
    ftp: ftplib.FTP,
    command: str,
    remote_name: str,
    payload: bytes,
    ordering: str,
    command_first_delay: float,
) -> None:
    ftp.voidcmd("TYPE I")
    host, port = ftp_passive_endpoint(ftp, command)
    preliminary: str | None = None
    if ordering == "connect-first":
        data = socket.create_connection(
            (host, port), ftp.timeout, source_address=ftp.source_address
        )
        try:
            # Hold the connected socket idle long enough for the FTP worker
            # to preaccept it while waiting for the transfer command.

            time.sleep(1.0)
            ftp.putcmd("STOR " + remote_name)
        except Exception:
            data.close()
            raise
    elif ordering == "command-first-close":
        ftp.putcmd("STOR " + remote_name)

        # FileZilla sends STOR before opening its data socket.  Give the
        # worker time to enter its bounded listener poll, then finish and
        # close the tiny transfer before reading the preliminary 150.

        time.sleep(command_first_delay)
        data = socket.create_connection(
            (host, port), ftp.timeout, source_address=ftp.source_address
        )
    elif ordering == "command-first-150":
        ftp.putcmd("STOR " + remote_name)
        preliminary = ftp.getresp()
        if not preliminary.startswith("150"):
            raise AuditFailure(
                f"{command} STOR returned {preliminary!r}, expected 150"
            )
        data = socket.create_connection(
            (host, port), ftp.timeout, source_address=ftp.source_address
        )
    else:
        raise AuditFailure(f"unsupported early-close ordering: {ordering}")
    try:
        data.sendall(payload)
    finally:
        data.close()

    if preliminary is None:
        preliminary = ftp.getresp()
    if not preliminary.startswith("150"):
        raise AuditFailure(
            f"{command} early-close STOR returned {preliminary!r}, expected 150"
        )
    complete = ftp.voidresp()
    if not complete.startswith("226"):
        raise AuditFailure(
            f"{command} early-close STOR returned {complete!r}, expected 226"
        )


def audit_ftp(args: argparse.Namespace, payload: bytes, remote_name: str) -> list[Result]:
    results: list[Result] = []
    ftp = ftp_connect(args)
    gui_names = {
        "PASV": remote_name + ".gui-pasv",
        "EPSV": remote_name + ".gui-epsv",
    }
    cleanup = {
        remote_name,
        remote_name + ".active",
        remote_name + ".append",
        *gui_names.values(),
    }
    try:
        if ftp.pwd() != "/":
            raise AuditFailure(f"unexpected FTP root: {ftp.pwd()}")

        def second_control() -> str:
            durations = []
            for _ in range(20):
                started = time.monotonic()
                other = ftp_connect(args)
                try:
                    response = other.sendcmd("NOOP")
                    if not response.startswith("200"):
                        raise AuditFailure(f"rapid control NOOP failed: {response}")
                finally:
                    other.quit()
                durations.append(time.monotonic() - started)
            return (
                "20 rapid logins while first session was idle, "
                f"max={max(durations):.3f}s"
            )

        results.append(timed("ftp second control session", second_control))

        def directory_listing() -> str:
            entries: list[str] = []
            response = ftp.retrlines("LIST", entries.append)
            if not response.startswith("226"):
                raise AuditFailure(
                    f"FTP LIST returned {response!r}, expected 226"
                )
            return f"entries={len(entries)}"

        results.append(timed("ftp directory listing", directory_listing))

        def passive_roundtrip() -> str:
            ftp.set_pasv(True)
            ftp.storbinary("STOR " + remote_name, io.BytesIO(payload))
            received = bytearray()
            ftp.retrbinary("RETR " + remote_name, received.extend)
            if bytes(received) != payload:
                raise AuditFailure(
                    f"passive mismatch: expected={len(payload)} actual={len(received)}"
                )
            return f"bytes={len(received)} sha256={hashlib.sha256(received).hexdigest()}"

        results.append(timed("ftp passive SD roundtrip", passive_roundtrip))

        def gui_style_tiny_uploads() -> str:
            for command, name in gui_names.items():
                for iteration in range(25):
                    ordering = (
                        "connect-first",
                        "command-first-close",
                        "command-first-150",
                    )[iteration % 3]
                    command_first_delay = (
                        5.0 if (iteration // 3) % 2 == 0 else 1.0
                    )
                    prefix = f"{command}-{iteration:02d}:".encode()
                    tiny_payload = (prefix + payload)[:552]
                    if len(tiny_payload) != 552:
                        raise AuditFailure("GUI-style payload is not 552 bytes")

                    try:
                        ftp_early_close_store(
                            ftp,
                            command,
                            name,
                            tiny_payload,
                            ordering,
                            command_first_delay,
                        )
                    except ftplib.all_errors as error:
                        raise AuditFailure(
                            f"{command} {ordering} early-close iteration "
                            f"{iteration} failed: {error}"
                        ) from error
                    received = bytearray()
                    ftp.retrbinary("RETR " + name, received.extend)
                    if bytes(received) != tiny_payload:
                        raise AuditFailure(
                            f"{command} early-close mismatch at iteration "
                            f"{iteration}: expected={len(tiny_payload)} "
                            f"actual={len(received)}"
                        )
                    ftp.delete(name)

            return (
                "PASV=25 EPSV=25 bytes=552, connect-first, 1s/5s "
                "close-before-150, and wait-for-150 orderings; every RETR "
                "matched"
            )

        results.append(timed("ftp GUI-style tiny uploads", gui_style_tiny_uploads))

        def append_and_resume() -> str:
            first = payload[:4096]
            second = payload[4096:8192]
            append_name = remote_name + ".append"
            ftp.storbinary("STOR " + append_name, io.BytesIO(first))
            ftp.storbinary("APPE " + append_name, io.BytesIO(second))
            combined = bytearray()
            ftp.retrbinary("RETR " + append_name, combined.extend)
            if bytes(combined) != first + second:
                raise AuditFailure("APPE did not preserve and append file data")

            resumed = bytearray()
            offset = 12345
            ftp.retrbinary("RETR " + remote_name, resumed.extend, rest=offset)
            if bytes(resumed) != payload[offset:]:
                raise AuditFailure("REST retrieval returned the wrong range")

            whole = bytearray()
            ftp.retrbinary("RETR " + remote_name, whole.extend)
            if bytes(whole) != payload:
                raise AuditFailure("REST state leaked into the following transfer")
            return "APPE, REST range, and post-REST full transfer passed"

        results.append(timed("ftp append and resume", append_and_resume))

        def active_upload() -> str:
            active_name = remote_name + ".active"
            ftp.set_pasv(False)
            ftp.storbinary("STOR " + active_name, io.BytesIO(payload[:32768]))
            ftp.set_pasv(True)
            received = bytearray()
            ftp.retrbinary("RETR " + active_name, received.extend)
            if bytes(received) != payload[:32768]:
                raise AuditFailure("active-mode upload mismatch")
            return f"bytes={len(received)}"

        results.append(timed("ftp active-mode upload", active_upload))

        def confinement() -> str:
            ftp.cwd("/")
            try:
                ftp.retrbinary("RETR ../wifi.conf", lambda _: None)
            except ftplib.error_perm:
                pass
            else:
                raise AuditFailure("FTP escaped its configured root")
            ftp.cwd("..")
            if ftp.pwd() != "/":
                raise AuditFailure(f"FTP parent traversal changed root to {ftp.pwd()}")
            return "parent traversal rejected"

        results.append(timed("ftp root confinement", confinement))
    finally:
        ftp.set_pasv(True)
        for name in cleanup:
            try:
                ftp.delete(name)
            except ftplib.all_errors:
                pass
        try:
            ftp.quit()
        except ftplib.all_errors:
            ftp.close()

    return results


def audit_mixed_traffic(args: argparse.Namespace) -> list[Result]:
    payload = bytes(
        (index * 29 + 11) & 0xff for index in range(args.stress_size)
    )
    expected_hash = hashlib.sha256(payload).hexdigest()
    remote_name = "network-mixed-stress.bin"

    def mixed_roundtrip() -> str:
        gate = threading.Event()

        def ftp_roundtrip() -> tuple[float, float]:
            gate.wait()
            ftp = ftp_connect(args)
            received = bytearray()
            try:
                ftp.set_pasv(True)
                started = time.monotonic()
                ftp.storbinary(
                    "STOR " + remote_name,
                    io.BytesIO(payload),
                    blocksize=32768,
                )
                upload_seconds = time.monotonic() - started
                started = time.monotonic()
                ftp.retrbinary(
                    "RETR " + remote_name,
                    received.extend,
                    blocksize=32768,
                )
                download_seconds = time.monotonic() - started
                if bytes(received) != payload:
                    raise AuditFailure(
                        "mixed FTP mismatch: "
                        f"expected={len(payload)} actual={len(received)}"
                    )
                return upload_seconds, download_seconds
            finally:
                try:
                    ftp.delete(remote_name)
                except ftplib.all_errors:
                    pass
                try:
                    ftp.quit()
                except ftplib.all_errors:
                    ftp.close()

        def one_http(_: int) -> float:
            gate.wait()
            started = time.monotonic()
            status, body = http_get(
                args.host, args.http_port, "/", args.timeout
            )
            if status != 200 or not body:
                raise AuditFailure(
                    f"mixed HTTP status={status} bytes={len(body)}"
                )
            return time.monotonic() - started

        workers = args.stress_http_workers + 1
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as executor:
            ftp_future = executor.submit(ftp_roundtrip)
            http_futures = [
                executor.submit(one_http, index)
                for index in range(args.stress_http_requests)
            ]
            gate.set()
            latencies = sorted(future.result() for future in http_futures)
            upload_seconds, download_seconds = ftp_future.result()

        p95_index = max(0, int(len(latencies) * 0.95) - 1)
        return (
            f"FTP={len(payload)} bytes sha256={expected_hash} "
            f"upload={upload_seconds:.3f}s download={download_seconds:.3f}s, "
            f"HTTP={len(latencies)} p95={latencies[p95_index]:.3f}s "
            f"max={max(latencies):.3f}s"
        )

    return [timed("mixed FTP and HTTP stress", mixed_roundtrip)]


class TelnetClient:
    IAC = 255
    DONT = 254
    DO = 253
    WONT = 252
    WILL = 251
    SB = 250
    SE = 240
    ECHO = 1
    SGA = 3

    def __init__(self, sock: socket.socket):
        self.sock = sock
        self.plain = bytearray()
        self.options: list[tuple[int, int]] = []
        self.remote_enabled: set[int] = set()
        self.local_enabled: set[int] = set()
        self.state = "data"
        self.pending_command = 0

    def _reply(self, command: int, option: int) -> None:
        if command == self.WILL:
            if option in {self.ECHO, self.SGA}:
                if option in self.remote_enabled:
                    return
                self.remote_enabled.add(option)
                response = self.DO
            else:
                response = self.DONT
        elif command == self.WONT:
            if option not in self.remote_enabled:
                return
            self.remote_enabled.discard(option)
            response = self.DONT
        elif command == self.DO:
            if option == self.SGA:
                if option in self.local_enabled:
                    return
                self.local_enabled.add(option)
                response = self.WILL
            else:
                response = self.WONT
        else:
            if option not in self.local_enabled:
                return
            self.local_enabled.discard(option)
            response = self.WONT
        self.sock.sendall(bytes((self.IAC, response, option)))

    def feed(self, data: bytes) -> None:
        for byte in data:
            if self.state == "data":
                if byte == self.IAC:
                    self.state = "command"
                else:
                    self.plain.append(byte)
            elif self.state == "command":
                if byte == self.IAC:
                    self.plain.append(byte)
                    self.state = "data"
                elif byte in {self.WILL, self.WONT, self.DO, self.DONT}:
                    self.pending_command = byte
                    self.state = "option"
                elif byte == self.SB:
                    self.state = "subnegotiation"
                else:
                    self.state = "data"
            elif self.state == "option":
                self.options.append((self.pending_command, byte))
                self._reply(self.pending_command, byte)
                self.state = "data"
            elif self.state == "subnegotiation":
                if byte == self.IAC:
                    self.state = "subnegotiation-iac"
            elif self.state == "subnegotiation-iac":
                self.state = "data" if byte == self.SE else "subnegotiation"

    def receive_until(self, markers: tuple[bytes, ...], timeout: float) -> bytes:
        start = len(self.plain)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            current = bytes(self.plain[start:])
            if any(marker in current for marker in markers):
                break
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                break
            self.feed(chunk)
        return bytes(self.plain[start:])

    def send_line(self, line: bytes) -> None:
        self.sock.sendall(line + b"\r\n")

    def last_echo_command(self) -> int | None:
        echo = [command for command, option in self.options if option == self.ECHO]
        return echo[-1] if echo else None


def telnet_command(client: TelnetClient, command: bytes, timeout: float) -> bytes:
    client.send_line(command)
    return client.receive_until((b"nsh>",), timeout)


def telnet_login(client: TelnetClient, args: argparse.Namespace) -> None:
    greeting = client.receive_until((b"login:", b"nsh>"), args.timeout)
    if b"nsh>" in greeting:
        return
    if b"login:" not in greeting:
        raise AuditFailure(f"Telnet returned neither login nor prompt: {greeting[-160:]!r}")
    if args.telnet_password is None:
        raise AuditFailure(
            "Telnet requires authentication; set NUTTX_TELNET_PASSWORD or "
            "pass --telnet-password"
        )

    client.send_line(args.telnet_user.encode())
    prompt = client.receive_until((b"password:",), args.timeout)
    if b"password:" not in prompt:
        raise AuditFailure(f"Telnet did not request a password: {prompt[-160:]!r}")

    # Collect any option update emitted immediately after the prompt.  A
    # real Telnet client will locally echo the password if the server sends
    # WONT ECHO here, even though a raw socket would not.

    client.receive_until((), 0.3)
    if client.last_echo_command() != TelnetClient.WILL:
        raise AuditFailure("Telnet did not keep WILL ECHO active for password entry")

    password = args.telnet_password.encode()
    client.send_line(password)
    response = client.receive_until((b"nsh>", b"Login failed"), args.timeout)
    if password and password in response:
        raise AuditFailure("Telnet echoed the password")
    if b"nsh>" not in response:
        raise AuditFailure(f"Telnet login failed: {response[-160:]!r}")


def audit_telnet(args: argparse.Namespace) -> list[Result]:
    def simultaneous_shells() -> str:
        clients = [
            TelnetClient(
                socket.create_connection((args.host, args.telnet_port), args.timeout)
            )
            for _ in range(2)
        ]
        try:
            for client in clients:
                client.sock.settimeout(0.25)
                telnet_login(client, args)
            outputs = [
                telnet_command(client, b"uname -a", args.timeout)
                for client in clients
            ]
            for number, output in enumerate(outputs, 1):
                if b"NuttX" not in output or b"nsh>" not in output:
                    raise AuditFailure(
                        f"Telnet session {number} incomplete: {output[-160:]!r}"
                    )

            storage = telnet_command(clients[0], b"df /mnt/sd0", args.timeout)
            sd_rows = [
                line.split()
                for line in storage.splitlines()
                if line.rstrip().endswith(b"/mnt/sd0")
            ]
            if not sd_rows or len(sd_rows[-1]) < 5:
                raise AuditFailure("df did not report a mounted /mnt/sd0 filesystem")
            try:
                block_size, blocks = (int(value) for value in sd_rows[-1][:2])
            except ValueError as error:
                raise AuditFailure(f"invalid /mnt/sd0 df row: {sd_rows[-1]!r}") from error
            if block_size <= 0 or blocks <= 1:
                raise AuditFailure(f"/mnt/sd0 has no usable mounted capacity: {sd_rows[-1]!r}")

            def rapid_commands(item: tuple[int, TelnetClient]) -> int:
                number, client = item
                markers = [f"telnet-{number}-{index}".encode() for index in range(20)]
                start = len(client.plain)
                client.sock.sendall(
                    b"".join(b"echo " + marker + b"\r\n" for marker in markers)
                )
                deadline = time.monotonic() + args.timeout * 2
                while time.monotonic() < deadline:
                    try:
                        chunk = client.sock.recv(4096)
                    except socket.timeout:
                        continue
                    if not chunk:
                        break
                    client.feed(chunk)
                    plain = bytes(client.plain[start:])
                    if markers[-1] in plain and plain.count(b"nsh>") >= len(markers):
                        break
                plain = bytes(client.plain[start:])
                missing = [marker for marker in markers if marker not in plain]
                if missing:
                    raise AuditFailure(
                        f"Telnet session {number} lost {len(missing)} rapid commands"
                    )
                if plain.count(b"nsh>") < len(markers):
                    raise AuditFailure(
                        f"Telnet session {number} returned only "
                        f"{plain.count(b'nsh>')} of {len(markers)} prompts"
                    )
                return len(markers)

            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
                counts = list(executor.map(rapid_commands, enumerate(clients, 1)))

            def require_closed(endpoint: tuple[str, int], label: str) -> None:
                try:
                    stale = socket.create_connection(
                        endpoint, min(args.timeout, 1.0)
                    )
                except OSError:
                    return
                stale.close()
                raise AuditFailure(f"{label} still accepted a connection")

            def stop_ftp() -> None:
                stopped = telnet_command(
                    clients[0], b"ftpd_stop", args.timeout * 2
                )
                if b"stopped" not in stopped.lower():
                    raise AuditFailure(
                        f"ftpd_stop did not confirm shutdown: {stopped[-200:]!r}"
                    )

            def start_ftp() -> None:
                started = telnet_command(
                    clients[0], b"ftpd_start -4", args.timeout
                )
                if (
                    b"Starting the FTP daemon" not in started
                    and b"is running" not in started
                ):
                    raise AuditFailure(
                        f"ftpd_start did not start cleanly: {started[-200:]!r}"
                    )

                deadline = time.monotonic() + args.timeout
                while True:
                    try:
                        restarted = ftp_connect(args)
                        restarted.quit()
                        return
                    except ftplib.all_errors:
                        if time.monotonic() >= deadline:
                            raise AuditFailure(
                                "FTP did not accept login after restart"
                            )
                        time.sleep(0.1)

            pending = ftp_connect(args)
            pending_endpoint = ftp_passive_endpoint(pending, "PASV")
            try:
                stop_ftp()
                require_closed(
                    (args.host, args.ftp_port), "FTP control port after stop"
                )
                require_closed(
                    pending_endpoint, "unconnected PASV listener after stop"
                )
            finally:
                pending.close()
                start_ftp()

            idle = ftp_connect(args)
            idle_endpoint = ftp_passive_endpoint(idle, "EPSV")
            idle_data = socket.create_connection(idle_endpoint, args.timeout)
            try:
                time.sleep(0.5)
                response = idle.sendcmd("NOOP")
                if not response.startswith("200"):
                    raise AuditFailure(
                        f"NOOP with preaccepted idle data failed: {response}"
                    )

                stop_ftp()
                idle_data.settimeout(min(args.timeout, 1.0))
                try:
                    residual = idle_data.recv(1)
                except socket.timeout as error:
                    raise AuditFailure(
                        "preaccepted idle data socket survived ftpd_stop"
                    ) from error
                except OSError:
                    pass
                else:
                    if residual:
                        raise AuditFailure(
                            "preaccepted idle data socket returned unexpected data"
                        )

                require_closed(
                    (args.host, args.ftp_port), "FTP control port after stop"
                )
            finally:
                idle_data.close()
                idle.close()
                start_ftp()
        finally:
            for client in clients:
                client.sock.close()
        return (
            f"two shells, {sum(counts)} rapid commands, "
            f"SD={block_size * blocks} bytes, FTP listener and "
            "preaccepted shutdown passed"
        )

    return [timed("telnet concurrent shells", simultaneous_shells)]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("host")
    parser.add_argument(
        "--profile",
        choices=("fruit-jam-network", "pico-2-w-network"),
        default="fruit-jam-network",
    )
    parser.add_argument("--http-port", type=int, default=80)
    parser.add_argument("--ftp-port", type=int, default=21)
    parser.add_argument("--telnet-port", type=int, default=23)
    parser.add_argument("--ftp-user", default="ftp")
    parser.add_argument("--ftp-password", default="")
    parser.add_argument("--telnet-user", default="admin")
    parser.add_argument(
        "--telnet-password", default=os.environ.get("NUTTX_TELNET_PASSWORD")
    )
    parser.add_argument("--size", type=int, default=256 * 1024)
    parser.add_argument("--stress-size", type=int, default=2 * 1024 * 1024)
    parser.add_argument("--stress-http-requests", type=int, default=200)
    parser.add_argument("--stress-http-workers", type=int, default=12)
    parser.add_argument("--timeout", type=float, default=5.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.size < 32768:
        print("FAIL: --size must be at least 32768 bytes", file=sys.stderr)
        return 2
    if args.stress_size != 0 and args.stress_size < 32768:
        print(
            "FAIL: --stress-size must be zero or at least 32768 bytes",
            file=sys.stderr,
        )
        return 2
    if args.stress_http_requests < 1 or args.stress_http_workers < 1:
        print(
            "FAIL: mixed-stress HTTP request and worker counts must be positive",
            file=sys.stderr,
        )
        return 2

    payload = bytes((index * 17 + 3) & 0xff for index in range(args.size))
    remote_name = "network-audit.bin"
    results: list[Result] = []

    try:
        results.extend(audit_ftp(args, payload, remote_name))
        if args.stress_size:
            results.extend(audit_mixed_traffic(args))

        shared_web_root = args.profile == "fruit-jam-network"
        if shared_web_root:
            # HTTP verifies the file while it is still present, so put it
            # back after the FTP audit's cleanup phase.

            def prepare_http_payload() -> str:
                ftp = ftp_connect(args)
                try:
                    ftp.storbinary("STOR " + remote_name, io.BytesIO(payload))
                finally:
                    ftp.quit()
                return f"bytes={len(payload)}"

            results.append(
                timed("ftp prepares HTTP payload", prepare_http_payload)
            )

        try:
            results.extend(
                audit_http(args, payload, remote_name, shared_web_root)
            )
        finally:
            if shared_web_root:
                ftp = ftp_connect(args)
                try:
                    ftp.delete(remote_name)
                finally:
                    ftp.quit()

        results.extend(audit_telnet(args))
    except EXPECTED_ERRORS as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1

    print(f"PASS  network service audit ({len(results)} checks)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
