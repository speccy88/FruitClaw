#!/usr/bin/env python3
"""Host-side FruitClaw unattended soak and recovery harness.

The board-side FruitClaw watchdog is the primary safety net.  Normal images
reset back into the app; optional bring-up images may convert watchdog resets
to BOOTSEL.  This host harness adds an outside observer: it proves
MCP/status/CDC progress, and when progress becomes stale it can explicitly
force the RP2350 back to BOOTSEL for reflashing.  The authoritative BOOTSEL
check is always `picotool info -a`.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import glob
import http.client
import json
import os
import re
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from typing import Any


DEFAULT_DEVICE_IP = "192.168.1.7"
DEFAULT_SERIAL_GLOB = "/dev/cu.usbmodem*"
DEFAULT_HTTP_BOOTSEL_PATHS = (
    "/bootsel",
    "/cgi-bin/bootsel",
    "/fruitjamctl/bootsel",
)
DEFAULT_SERIAL_CHECK_INTERVAL_SEC = 30.0
SERIAL_PROMPT = b"nsh>"
IPV4_RE = re.compile(r"\b(?:\d{1,3}\.){3}\d{1,3}\b")
IFCONFIG_INET_RE = re.compile(
    r"\binet(?:\s+addr)?[: ]+((?:\d{1,3}\.){3}\d{1,3})\b"
)
COUNTER_BASELINE_KEYS = (
    "telegram_fails",
    "router_failed",
    "agent_failures",
    "agent_tool_failures",
    "agent_reply_failures",
    "scheduler_failures",
    "network_recovery_failures",
    "webserver_exits",
    "mcp_failures",
    "mcp_tool_failures",
)


def utc_now() -> str:
    return _dt.datetime.now(_dt.UTC).isoformat(timespec="seconds")


def redact_secret_text(text: str) -> str:
    secrets = (
        os.environ.get("FRUITCLAW_DEEPSEEK_API_KEY", ""),
        os.environ.get("FRUITCLAW_TELEGRAM_TOKEN", ""),
        os.environ.get("FRUITCLAW_WIFI_PASSWORD", ""),
    )

    for secret in secrets:
        if secret:
            text = text.replace(secret, "[redacted]")

    text = re.sub(r"\bsk-[A-Za-z0-9_-]{12,}\b", "[redacted]", text)
    text = re.sub(r"\b\d{7,}:[A-Za-z0-9_-]{20,}\b", "[redacted]", text)
    return text


def redact_log_value(value: Any) -> Any:
    if isinstance(value, str):
        return redact_secret_text(value)
    if isinstance(value, list):
        return [redact_log_value(item) for item in value]
    if isinstance(value, tuple):
        return tuple(redact_log_value(item) for item in value)
    if isinstance(value, dict):
        return {key: redact_log_value(item) for key, item in value.items()}
    return value


def log_event(args: argparse.Namespace, event: str, **fields: Any) -> None:
    row = {"ts": utc_now(), "event": event}
    row.update(redact_log_value(fields))
    text = json.dumps(row, separators=(",", ":"), sort_keys=True)
    print(text, flush=True)
    if args.log:
        with open(args.log, "a", encoding="utf-8") as fp:
            fp.write(text + "\n")


def valid_device_ip(ip: str) -> bool:
    try:
        parts = [int(part) for part in ip.split(".")]
    except ValueError:
        return False

    if len(parts) != 4 or any(part < 0 or part > 255 for part in parts):
        return False
    if parts[0] in (0, 127) or parts[0] >= 224:
        return False
    if parts[3] in (0, 255):
        return False
    return True


def extract_device_ip(text: str) -> str | None:
    for match in IFCONFIG_INET_RE.finditer(text):
        ip = match.group(1)
        if valid_device_ip(ip):
            return ip

    for ip in IPV4_RE.findall(text):
        if valid_device_ip(ip):
            return ip

    return None


def update_device_ip(args: argparse.Namespace, ip: str, reason: str) -> bool:
    if not valid_device_ip(ip):
        return False

    old_ip = args.device_ip
    old_mcp_url = args.mcp_url
    changed = old_ip != ip
    args.device_ip = ip

    if not getattr(args, "_mcp_url_explicit", False):
        args.mcp_url = f"http://{ip}/mcp"
        changed = changed or old_mcp_url != args.mcp_url

    if changed:
        setattr(args, "_docs_checked", False)
        setattr(args, "_mcp_tools_checked", False)
        log_event(args, "device_ip_updated", old_ip=old_ip, new_ip=ip,
                  old_mcp_url=old_mcp_url, new_mcp_url=args.mcp_url,
                  reason=reason)
    else:
        log_event(args, "device_ip_confirmed", ip=ip, reason=reason)

    return changed


def run_cmd(argv: list[str], timeout: float) -> tuple[int, str]:
    try:
        proc = subprocess.run(
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
            check=False,
        )
        return proc.returncode, proc.stdout
    except FileNotFoundError as exc:
        return 127, str(exc)
    except subprocess.TimeoutExpired as exc:
        out = exc.stdout or ""
        return 124, out + "\ncommand timed out"


def picotool_info(args: argparse.Namespace) -> tuple[bool, str]:
    rc, out = run_cmd([args.picotool, "info", "-a"], args.command_timeout)
    return rc == 0, out.strip()


def picotool_force_bootsel(args: argparse.Namespace) -> tuple[int, str]:
    rc, out = run_cmd([args.picotool, "reboot", "-u", "-f"],
                      args.command_timeout)
    return rc, out.strip()


def wait_for_bootsel(args: argparse.Namespace, timeout: float) -> tuple[bool, str]:
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        ok, out = picotool_info(args)
        last = out
        if ok:
            return True, out
        time.sleep(1)
    return False, last


def mcp_rpc(args: argparse.Namespace, payload: dict[str, Any],
            timeout: float | None = None) -> dict[str, Any]:
    body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    req = urllib.request.Request(
        args.mcp_url,
        data=body,
        headers={"Content-Type": "application/json", "Connection": "close"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=timeout or args.command_timeout) as resp:
        try:
            raw = resp.read()
        except http.client.IncompleteRead as exc:
            raw = exc.partial
            if not raw:
                raise

        data = raw.decode("utf-8", "replace")
    return json.loads(data)


def http_get_text(url: str, timeout: float) -> tuple[int, str]:
    req = urllib.request.Request(url, headers={"Connection": "close"},
                                 method="GET")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        data = resp.read().decode("utf-8", "replace")
        return int(resp.status), data


def check_static_docs(args: argparse.Namespace) -> tuple[bool, str]:
    base = f"http://{args.device_ip}"

    try:
        status, html = http_get_text(f"{base}/index.html",
                                     args.command_timeout)
        if status != 200 or (
            "Open /docs Manual" not in html or "/site/home.md" not in html
        ):
            return False, f"index.html docs check failed: http={status}"

        status, docs_html = http_get_text(f"{base}/docs/index.html",
                                          args.command_timeout)
        if status != 200 or "FruitClaw Manual" not in docs_html:
            return False, f"docs/index.html returned http={status}"

        status, raw_index = http_get_text(f"{base}/docs/index.json",
                                          args.command_timeout)
        if status != 200:
            return False, f"docs/index.json returned http={status}"

        index = json.loads(raw_index)
        pages = index.get("pages")
        if not isinstance(pages, list) or not pages:
            return False, "docs/index.json has no pages"

        slugs = {page.get("slug") for page in pages if isinstance(page, dict)}
        if not {"home", "tools"}.issubset(slugs):
            return False, f"docs page set incomplete: {sorted(slugs)}"
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError,
            http.client.IncompleteRead, OSError) as exc:
        return False, f"static docs check failed: {type(exc).__name__}: {exc}"

    return True, "ok"


def mcp_tools_count(args: argparse.Namespace) -> int:
    result = mcp_rpc(
        args,
        {
            "jsonrpc": "2.0",
            "id": int(time.time() * 1000) & 0x7fffffff,
            "method": "tools/list",
            "params": {},
        },
    )
    tools = result.get("result", {}).get("tools", [])
    if not isinstance(tools, list):
        return -1
    return len(tools)


def mcp_tool_text(result: dict[str, Any]) -> str:
    try:
        content = result["result"]["content"]
    except (KeyError, TypeError):
        return ""

    if not isinstance(content, list):
        return ""

    parts: list[str] = []
    for item in content:
        if isinstance(item, dict) and item.get("type") == "text":
            parts.append(str(item.get("text", "")))
    return "\n".join(parts)


def mcp_tool(args: argparse.Namespace, name: str,
             arguments: dict[str, Any],
             timeout: float | None = None) -> dict[str, Any]:
    return mcp_rpc(
        args,
        {
            "jsonrpc": "2.0",
            "id": int(time.time() * 1000) & 0x7fffffff,
            "method": "tools/call",
            "params": {"name": name, "arguments": arguments},
        },
        timeout,
    )


def wait_for_mcp(args: argparse.Namespace, timeout: float) -> tuple[bool, str]:
    deadline = time.monotonic() + timeout
    last = ""

    while time.monotonic() < deadline:
        bootsel, info = picotool_info(args)
        if bootsel:
            first = info.splitlines()[0] if info else "BOOTSEL"
            return False, "board entered BOOTSEL: " + first

        ok, last = mcp_ping_ok(args, 1, timeout=3)
        if ok:
            return True, "ok"

        refreshed, refresh_reason = refresh_mcp_endpoint(
            args, "wait-for-mcp"
        )
        if refreshed:
            return True, "ok"
        last = f"{last}; refresh={refresh_reason}"

        time.sleep(2)

    return False, "MCP did not return before timeout: " + last


def bootsel_failure(args: argparse.Namespace) -> str | None:
    bootsel, info = picotool_info(args)
    if bootsel:
        first = info.splitlines()[0] if info else "BOOTSEL"
        return "board entered BOOTSEL: " + first

    return None


def http_bootsel_urls(args: argparse.Namespace) -> list[str]:
    urls: list[str] = []

    for url in args.http_bootsel_url or []:
        if url:
            urls.append(url)

    for path in args.http_bootsel_path or []:
        if path.startswith("http://") or path.startswith("https://"):
            urls.append(path)
        else:
            urls.append(f"http://{args.device_ip}{path}")

    return urls


def http_bootsel(args: argparse.Namespace, url: str) -> str:
    req = urllib.request.Request(url, method="GET")
    with urllib.request.urlopen(req, timeout=args.command_timeout) as resp:
        data = resp.read(512).decode("utf-8", "replace")
        return f"http={resp.status} body={data[:160]!r}"


def telnet_command(args: argparse.Namespace, command: str,
                   line_ending: bytes = b"\r\n") -> str:
    iac = 255
    will = 251
    wont = 252
    do = 253
    dont = 254
    sb = 250
    se = 240
    echo = 1
    sga = 3
    naws = 31

    def consume_telnet(chunk: bytes, sock: socket.socket) -> bytes:
        plain = bytearray()
        reply = bytearray()
        i = 0

        while i < len(chunk):
            ch = chunk[i]
            if ch != iac:
                plain.append(ch)
                i += 1
                continue

            if i + 1 >= len(chunk):
                break

            cmd = chunk[i + 1]
            if cmd == iac:
                plain.append(iac)
                i += 2
                continue

            if cmd in (will, wont, do, dont):
                if i + 2 >= len(chunk):
                    break

                opt = chunk[i + 2]
                if cmd == do:
                    response = will if opt == sga else wont
                elif cmd == will:
                    response = do if opt in (echo, sga, naws) else dont
                elif cmd == wont:
                    response = dont
                else:
                    response = wont

                reply.extend((iac, response, opt))
                i += 3
                continue

            if cmd == sb:
                i += 2
                while i + 1 < len(chunk):
                    if chunk[i] == iac and chunk[i + 1] == se:
                        i += 2
                        break
                    i += 1
                continue

            i += 2

        if reply:
            sock.sendall(reply)

        return bytes(plain)

    deadline = time.monotonic() + args.telnet_timeout
    initial = bytearray()
    response = bytearray()

    with socket.create_connection((args.device_ip, args.telnet_port),
                                  timeout=args.telnet_timeout) as sock:
        sock.settimeout(0.25)
        while time.monotonic() < deadline:
            try:
                chunk = sock.recv(4096)
            except TimeoutError:
                break
            except socket.timeout:
                break
            if not chunk:
                break
            initial.extend(consume_telnet(chunk, sock))
            if b"nsh>" in initial or b"login:" in initial:
                break

        time.sleep(0.1)
        sock.sendall(command.encode("utf-8") + line_ending)
        sock.settimeout(0.25)
        while time.monotonic() < deadline:
            try:
                chunk = sock.recv(4096)
            except TimeoutError:
                break
            except socket.timeout:
                time.sleep(0.05)
                continue
            if not chunk:
                break
            response.extend(consume_telnet(chunk, sock))
            if b"nsh>" in response:
                break

    return (initial + response).decode("utf-8", "replace")


def run_telnet_cr_smoke(args: argparse.Namespace) -> tuple[bool, str]:
    variants = (
        ("cr", b"\r"),
        ("lf", b"\n"),
        ("crlf", b"\r\n"),
    )

    for name, ending in variants:
        marker = f"fruitclaw-telnet-{name}-ok"
        try:
            out = telnet_command(args, f"echo {marker}", ending)
        except (OSError, TimeoutError) as exc:
            return False, (
                f"telnet {name} failed: {type(exc).__name__}: {exc}"
            )

        if marker not in out or "nsh>" not in out:
            return False, (
                f"telnet {name} did not execute command; "
                f"output={out[-240:]!r}"
            )

    return True, "ok"


def resolve_serial_port(args: argparse.Namespace) -> str | None:
    if args.no_serial:
        return None
    if args.serial and args.serial != "auto":
        return args.serial
    matches = sorted(glob.glob(args.serial_glob))
    return matches[0] if matches else None


def _serial_command_direct(port: str, command: str, timeout: float) -> str:
    try:
        import serial  # type: ignore
    except ImportError as exc:
        raise RuntimeError("pyserial is required for serial checks") from exc

    with serial.Serial(port, 115200, timeout=0.25, write_timeout=1) as ser:
        ser.dtr = False
        ser.rts = False
        time.sleep(0.2)
        ser.reset_input_buffer()
        ser.write((command + "\n").encode("utf-8"))
        ser.flush()
        deadline = time.monotonic() + timeout
        buf = bytearray()
        saw_prompt = False
        while time.monotonic() < deadline:
            chunk = ser.read(4096)
            if chunk:
                buf.extend(chunk)
                if SERIAL_PROMPT in buf:
                    saw_prompt = True
                    break
            else:
                time.sleep(0.1)

        text = buf.decode("utf-8", "replace")
        if not saw_prompt:
            preview = text[-160:].replace("\r", "\\r").replace("\n", "\\n")
            raise TimeoutError(
                f"serial command did not return NSH prompt on {port}; "
                f"bytes={len(buf)} tail={preview!r}"
            )

        return text


def _serial_touch_1200_direct(port: str) -> None:
    try:
        import serial  # type: ignore
    except ImportError as exc:
        raise RuntimeError("pyserial is required for 1200-baud recovery") from exc

    with serial.Serial(port, 1200, timeout=0.1, write_timeout=1) as ser:
        ser.dtr = False
        ser.rts = False
        time.sleep(0.5)


def _serial_send_direct(port: str, command: str) -> None:
    try:
        import serial  # type: ignore
    except ImportError as exc:
        raise RuntimeError("pyserial is required for serial send") from exc

    with serial.Serial(port, 115200, timeout=0.1, write_timeout=1) as ser:
        ser.dtr = False
        ser.rts = False
        time.sleep(0.2)
        ser.write((command + "\n").encode("utf-8"))
        ser.flush()
        time.sleep(0.1)


def _serial_worker_run(kind: str, port: str, command: str,
                       timeout: float) -> str:
    def alarm_handler(signum: int, frame: Any) -> None:
        raise TimeoutError(f"serial {kind} timed out inside child on {port}")

    if hasattr(signal, "SIGALRM"):
        signal.signal(signal.SIGALRM, alarm_handler)
        signal.setitimer(signal.ITIMER_REAL, timeout + 1.0)

    try:
        if kind == "command":
            return _serial_command_direct(port, command, timeout)
        if kind == "touch1200":
            _serial_touch_1200_direct(port)
            return ""
        if kind == "send":
            _serial_send_direct(port, command)
            return ""
        raise RuntimeError(f"unknown serial worker kind: {kind}")
    finally:
        if hasattr(signal, "SIGALRM"):
            signal.setitimer(signal.ITIMER_REAL, 0.0)


def _serial_worker_main(argv: list[str]) -> int:
    if len(argv) < 5:
        print(json.dumps({
            "ok": False,
            "error": "usage: --_serial-worker <kind> <port> <timeout> [command]",
        }))
        return 2

    kind = argv[2]
    port = argv[3]
    timeout = float(argv[4])
    command = argv[5] if len(argv) > 5 else ""

    try:
        output = _serial_worker_run(kind, port, command, timeout)
        print(json.dumps({"ok": True, "output": output}))
        return 0
    except BaseException as exc:
        print(json.dumps({
            "ok": False,
            "error": f"{type(exc).__name__}: {exc}",
        }))
        return 1


def _run_serial_worker(kind: str, port: str, command: str,
                       timeout: float) -> str:
    argv = [
        sys.executable,
        os.path.abspath(__file__),
        "--_serial-worker",
        kind,
        port,
        str(timeout),
        command,
    ]

    proc = subprocess.Popen(
        argv,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        start_new_session=hasattr(os, "setsid"),
    )

    try:
        out, _ = proc.communicate(timeout=timeout + 3.0)
    except subprocess.TimeoutExpired:
        try:
            if hasattr(os, "killpg"):
                os.killpg(proc.pid, signal.SIGKILL)
            else:
                proc.kill()
        except OSError:
            pass
        if proc.stdout is not None:
            proc.stdout.close()
        raise TimeoutError(
            f"serial {kind} timed out opening or using {port}"
        ) from None

    lines = [line for line in out.splitlines() if line.strip()]
    if not lines:
        raise RuntimeError(
            f"serial {kind} worker exited with status {proc.returncode}"
        )

    try:
        result = json.loads(lines[-1])
    except json.JSONDecodeError as exc:
        raise RuntimeError(
            f"serial {kind} worker returned invalid output: {out[-200:]}"
        ) from exc

    if not result.get("ok"):
        raise RuntimeError(str(result.get("error", "unknown serial error")))
    return str(result.get("output", ""))


def serial_command(port: str, command: str, timeout: float) -> str:
    return _run_serial_worker("command", port, command, timeout)


def nsh_quote_arg(value: str) -> str:
    return '"' + (
        value
        .replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("$", "\\$")
        .replace("`", "\\`")
    ) + '"'


def serial_touch_1200(port: str, timeout: float) -> None:
    _run_serial_worker("touch1200", port, "", timeout)


def serial_send(port: str, command: str, timeout: float) -> None:
    _run_serial_worker("send", port, command, timeout)


def serial_ip_commands(args: argparse.Namespace) -> list[str]:
    commands: list[str] = []
    for command in args.serial_ip_command or []:
        if command and command not in commands:
            commands.append(command)

    for command in ("ifconfig wlan0", "ifconfig"):
        if command not in commands:
            commands.append(command)

    return commands


def discover_ip_from_serial(args: argparse.Namespace, reason: str) -> str | None:
    port = resolve_serial_port(args)
    if port is None:
        log_event(args, "ip_discover_skipped", reason=reason,
                  why="no serial port")
        return None

    for command in serial_ip_commands(args):
        try:
            out = serial_command(port, command, args.serial_ip_timeout_sec)
        except (OSError, RuntimeError, TimeoutError) as exc:
            ip = extract_device_ip(str(exc))
            if ip is not None:
                update_device_ip(args, ip, reason)
                log_event(args, "ip_discover_ok", reason=reason,
                          serial=port, command=command, ip=ip,
                          source="partial-error")
                return ip

            log_event(args, "ip_discover_command_failed", reason=reason,
                      serial=port, command=command,
                      error=f"{type(exc).__name__}: {exc}")
            continue

        ip = extract_device_ip(out)
        if ip is not None:
            update_device_ip(args, ip, reason)
            log_event(args, "ip_discover_ok", reason=reason, serial=port,
                      command=command, ip=ip)
            return ip

        log_event(args, "ip_discover_no_address", reason=reason, serial=port,
                  command=command, output_tail=out[-200:])

    return None


def recover_network_from_serial(args: argparse.Namespace,
                                reason: str) -> tuple[bool, str]:
    if not args.serial_network_recover:
        return False, "serial network recovery disabled"

    port = resolve_serial_port(args)
    if port is None:
        return False, "no serial port"

    try:
        out = serial_command(port, "fruitclaw wifi-up",
                             args.serial_network_recover_timeout_sec)
        log_event(args, "serial_network_recover_done", reason=reason,
                  serial=port, output_tail=out[-400:])
    except (OSError, RuntimeError, TimeoutError) as exc:
        return False, f"{type(exc).__name__}: {exc}"

    discover_ip_from_serial(args, f"{reason}:serial-network-recover")
    if args.ip_rediscover_settle_sec > 0:
        time.sleep(args.ip_rediscover_settle_sec)

    ok, why = mcp_ping_ok(args, 1, timeout=3)
    if ok:
        log_event(args, "serial_network_recover_ok", reason=reason,
                  device_ip=args.device_ip, mcp_url=args.mcp_url)
        return True, "ok"

    return False, why


def mcp_ping_ok(args: argparse.Namespace, rpc_id: int,
                timeout: float = 3.0) -> tuple[bool, str]:
    try:
        ping = mcp_rpc(
            args,
            {"jsonrpc": "2.0", "id": rpc_id, "method": "ping"},
            timeout=timeout,
        )
        if "result" in ping:
            return True, "ok"
        return False, f"MCP ping returned no result: {ping}"
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError,
            http.client.IncompleteRead, OSError) as exc:
        return False, f"{type(exc).__name__}: {exc}"


def refresh_mcp_endpoint(args: argparse.Namespace,
                         reason: str) -> tuple[bool, str]:
    if not args.rediscover_ip_on_failure:
        return False, "IP rediscovery disabled"

    now = time.monotonic()
    last = getattr(args, "_last_ip_rediscover_mono", 0.0)
    if now - float(last) < args.ip_rediscover_min_interval_sec:
        return False, "IP rediscovery throttled"

    setattr(args, "_last_ip_rediscover_mono", now)
    if args.current_endpoint_retry_sec > 0:
        time.sleep(args.current_endpoint_retry_sec)
        ok, why = mcp_ping_ok(args, 1, timeout=3)
        if ok:
            log_event(args, "mcp_endpoint_retry_ok", reason=reason,
                      mcp_url=args.mcp_url)
            return True, "ok"
        log_event(args, "mcp_endpoint_retry_failed", reason=reason,
                  mcp_url=args.mcp_url, error=why)

    ip = discover_ip_from_serial(args, reason)
    if ip is None:
        return False, "serial IP discovery found no address"

    if args.ip_rediscover_settle_sec > 0:
        time.sleep(args.ip_rediscover_settle_sec)

    ok, why = mcp_ping_ok(args, 1, timeout=3)
    if ok:
        log_event(args, "mcp_endpoint_refresh_ok", reason=reason,
                  device_ip=args.device_ip, mcp_url=args.mcp_url)
        return True, "ok"

    log_event(args, "mcp_endpoint_refresh_failed", reason=reason,
              device_ip=args.device_ip, mcp_url=args.mcp_url, error=why)

    recovered, recover_reason = recover_network_from_serial(args, reason)
    if recovered:
        return True, "ok"

    log_event(args, "serial_network_recover_failed", reason=reason,
              error=recover_reason)
    return False, why


def tolerate_mcp_health_failure(args: argparse.Namespace, stage: str,
                                reason: str) -> bool:
    failures = int(getattr(args, "_mcp_health_failures", 0)) + 1
    setattr(args, "_mcp_health_failures", failures)
    log_event(args, "mcp_health_failed", stage=stage, failures=failures,
              max_failures=args.max_mcp_health_failures, reason=reason)
    return failures <= args.max_mcp_health_failures


def reset_mcp_health_failures(args: argparse.Namespace) -> None:
    failures = int(getattr(args, "_mcp_health_failures", 0))
    if failures > 0:
        log_event(args, "mcp_health_recovered", failures=failures)
        setattr(args, "_mcp_health_failures", 0)


def wait_for_serial_port(args: argparse.Namespace, timeout: float) -> str | None:
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        port = resolve_serial_port(args)
        if port is not None:
            return port
        time.sleep(0.5)

    return resolve_serial_port(args)


def serial_interact_secret(port: str, command: str,
                           responses: list[tuple[str, str]],
                           timeout: float) -> None:
    try:
        import serial  # type: ignore
    except ImportError as exc:
        raise RuntimeError("pyserial is required for provisioning") from exc

    with serial.Serial(port, 115200, timeout=0.1, write_timeout=2) as ser:
        ser.dtr = False
        ser.rts = False
        time.sleep(0.2)
        ser.reset_input_buffer()
        ser.write((command + "\n").encode("utf-8"))
        ser.flush()

        buf = bytearray()
        sent = 0
        deadline = time.monotonic() + timeout

        while time.monotonic() < deadline:
            chunk = ser.read(512)
            if chunk:
                buf.extend(chunk)
                text = buf.decode("utf-8", "replace").lower()

                if sent < len(responses) and responses[sent][0].lower() in text:
                    ser.write((responses[sent][1] + "\n").encode("utf-8"))
                    ser.flush()
                    sent += 1
                    buf.clear()
                    continue

                if sent >= len(responses) and SERIAL_PROMPT in buf:
                    return
            else:
                time.sleep(0.03)

    raise TimeoutError(
        f"provision command did not complete on {port}: {command}"
    )


def provision_from_env(args: argparse.Namespace, reason: str) -> bool:
    if not args.provision_from_env:
        return True

    port = wait_for_serial_port(args, args.provision_serial_wait_sec)
    if port is None:
        bootsel, info = picotool_info(args)
        if bootsel:
            rc, out = run_cmd([args.picotool, "reboot", "-a"],
                              args.command_timeout)
            log_event(args, "provision_reboot_app_from_bootsel",
                      reason=reason, rc=rc, output_tail=out[-400:],
                      bootsel_info=info.splitlines()[0] if info else "BOOTSEL")
            if rc == 0:
                port = wait_for_serial_port(args,
                                            args.provision_serial_wait_sec)

        if port is None:
            log_event(args, "provision_failed", reason=reason,
                      error="no serial port")
            return False

    ssid = os.environ.get("FRUITCLAW_WIFI_SSID", "")
    password = os.environ.get("FRUITCLAW_WIFI_PASSWORD", "")
    deepseek = os.environ.get("FRUITCLAW_DEEPSEEK_API_KEY", "")
    telegram = os.environ.get("FRUITCLAW_TELEGRAM_TOKEN", "")

    try:
        if ssid or password:
            if not ssid or not password:
                raise RuntimeError(
                    "both FRUITCLAW_WIFI_SSID and "
                    "FRUITCLAW_WIFI_PASSWORD are required"
                )

            out = serial_command(
                port,
                "fruitclaw config set-wifi "
                f"{nsh_quote_arg(ssid)} {nsh_quote_arg(password)}",
                args.provision_command_timeout,
            )
            if "usage:" in out:
                serial_interact_secret(
                    port, "fruitclaw config set-wifi",
                    [("ssid:", ssid), ("password:", password)],
                    args.provision_command_timeout,
                )
            log_event(args, "provision_wifi_configured", reason=reason)

        if deepseek:
            out = serial_command(
                port,
                "fruitclaw config set-secret deepseek "
                f"{nsh_quote_arg(deepseek)}",
                args.provision_command_timeout,
            )
            if "usage:" in out:
                serial_interact_secret(
                    port, "fruitclaw config set-secret deepseek",
                    [("secret value:", deepseek)],
                    args.provision_command_timeout,
                )
            log_event(args, "provision_secret_configured",
                      reason=reason, name="deepseek")

        if telegram:
            out = serial_command(
                port,
                "fruitclaw config set-secret telegram "
                f"{nsh_quote_arg(telegram)}",
                args.provision_command_timeout,
            )
            if "usage:" in out:
                serial_interact_secret(
                    port, "fruitclaw config set-secret telegram",
                    [("secret value:", telegram)],
                    args.provision_command_timeout,
                )
            log_event(args, "provision_secret_configured",
                      reason=reason, name="telegram")

        if args.provision_wifi_up:
            out = serial_command(port, "fruitclaw wifi-up",
                                 args.provision_wifi_timeout)
            log_event(args, "provision_wifi_up_done",
                      reason=reason, output_tail=out[-400:])
            discover_ip_from_serial(args, f"{reason}:wifi-up")

        return True
    except Exception as exc:
        log_event(args, "provision_failed", reason=reason,
                  error=f"{type(exc).__name__}: {exc}")
        return False


def serial_check_due(args: argparse.Namespace, now: float) -> bool:
    interval = args.serial_check_interval_sec
    last = getattr(args, "_last_serial_check_mono", None)

    if interval < 0:
        return False
    if interval == 0 or last is None:
        args._last_serial_check_mono = now
        return True
    if now - float(last) >= interval:
        args._last_serial_check_mono = now
        return True
    return False


STATUS_RE = {
    "queues": re.compile(r"queues: main=(\d+) agent=(\d+)"),
    "telegram": re.compile(
        r"telegram_status: .*polls=(\d+) fails=(\d+) .*http=(-?\d+) "
        r"last_poll_age_ms=(-?\d+) .*updates=(\d+) queued=(\d+) ignored=(\d+)"
        r".*notify_queue=(\d+)(?: notify_inflight=(\d+))? "
        r"notify_enqueued=(\d+) notify_sent=(\d+) "
        r"notify_failed=(\d+) notify_dropped=(\d+)"
    ),
    "router": re.compile(
        r"router_status: .*routed=(\d+) dropped=(\d+) failed=(\d+) "
        r"last_ret=(-?\d+) last_age_ms=(-?\d+) last=([^/\s]+)/(\S+)"
    ),
    "agent": re.compile(
        r"agent_status: .*started=(\d+) done=(\d+) failures=(\d+) "
        r"tools=(\d+) tool_failures=(\d+) replies=(\d+) "
        r"reply_failures=(\d+) last_ret=(-?\d+) last_reply_ret=(-?\d+) "
        r"last_event_age_ms=(-?\d+) last_done_age_ms=(-?\d+) "
        r"last_reply_age_ms=(-?\d+).* tool=(\S+)"
    ),
    "deepseek": re.compile(
        r"deepseek_status: calls=(\d+) fails=(\d+) .*http=(-?\d+) "
        r"last_start_age_ms=(-?\d+) last_success_age_ms=(-?\d+)"
    ),
    "scheduler": re.compile(
        r"scheduler_status: .*ticks=(\d+) fired=(\d+) failures=(\d+) "
        r"last_ret=(-?\d+) last_tick_age_ms=(-?\d+) "
        r"last_fire_age_ms=(-?\d+) used=(\d+) enabled=(\d+)"
    ),
    "network_recovery": re.compile(
        r"network_recovery: running=(yes|no) attempts=(\d+) failures=(\d+) "
        r"last_ret=(-?\d+) last_start_age_ms=(-?\d+) "
        r"last_done_age_ms=(-?\d+) reason=(\S+)"
    ),
    "webserver": re.compile(
        r"webserver: supervisor=(\S+) listening=(yes|no) listens=(\d+) "
        r"exits=(\d+) last_ret=(-?\d+) last_errno=(-?\d+) "
        r"last_start_age_ms=(-?\d+) last_exit_age_ms=(-?\d+)"
    ),
    "mcp": re.compile(
        r"mcp_status: .*requests=(\d+) failures=(\d+) "
        r"notifications=(\d+)(?: notify_suppressed=\d+)? "
        r"tools=(\d+) tool_failures=(\d+) "
        r"last_ret=(-?\d+) last_http=(-?\d+) last_dispatch=(-?\d+) "
        r"last_age_ms=(-?\d+)"
    ),
    "guard": re.compile(r"guard_status: (.*)"),
    "last_tool_error": re.compile(r"last_tool_error: (.*)"),
}


def parse_status(text: str) -> dict[str, Any]:
    status: dict[str, Any] = {}
    guard_fields: list[str] = []
    in_guard = False

    for line in text.splitlines():
        stripped = line.strip()
        if stripped == "guard_status:":
            in_guard = True
            continue
        if in_guard:
            if line.startswith("    ") and stripped:
                guard_fields.append(stripped)
                continue
            in_guard = False

    if guard_fields:
        status["guard"] = " ".join(guard_fields)

    if match := STATUS_RE["queues"].search(text):
        status["main_queue"] = int(match.group(1))
        status["agent_queue"] = int(match.group(2))
    if match := STATUS_RE["telegram"].search(text):
        status["telegram_polls"] = int(match.group(1))
        status["telegram_fails"] = int(match.group(2))
        status["telegram_http"] = int(match.group(3))
        status["telegram_poll_age_ms"] = int(match.group(4))
        status["telegram_updates"] = int(match.group(5))
        status["telegram_queued"] = int(match.group(6))
        status["telegram_ignored"] = int(match.group(7))
        status["telegram_notify_queue"] = int(match.group(8))
        status["telegram_notify_inflight"] = int(match.group(9) or 0)
        status["telegram_notify_enqueued"] = int(match.group(10))
        status["telegram_notify_sent"] = int(match.group(11))
        status["telegram_notify_failed"] = int(match.group(12))
        status["telegram_notify_dropped"] = int(match.group(13))
    if match := STATUS_RE["router"].search(text):
        status["router_routed"] = int(match.group(1))
        status["router_dropped"] = int(match.group(2))
        status["router_failed"] = int(match.group(3))
        status["router_last_ret"] = int(match.group(4))
        status["router_last_age_ms"] = int(match.group(5))
        status["router_last_source"] = match.group(6)
        status["router_last_type"] = match.group(7)
    if match := STATUS_RE["agent"].search(text):
        status["agent_started"] = int(match.group(1))
        status["agent_done"] = int(match.group(2))
        status["agent_failures"] = int(match.group(3))
        status["agent_tools"] = int(match.group(4))
        status["agent_tool_failures"] = int(match.group(5))
        status["agent_replies"] = int(match.group(6))
        status["agent_reply_failures"] = int(match.group(7))
        status["agent_last_ret"] = int(match.group(8))
        status["agent_last_reply_ret"] = int(match.group(9))
        status["agent_event_age_ms"] = int(match.group(10))
        status["agent_done_age_ms"] = int(match.group(11))
        status["agent_reply_age_ms"] = int(match.group(12))
        status["agent_last_tool"] = match.group(13)
    if match := STATUS_RE["deepseek"].search(text):
        status["deepseek_calls"] = int(match.group(1))
        status["deepseek_fails"] = int(match.group(2))
        status["deepseek_http"] = int(match.group(3))
        status["deepseek_start_age_ms"] = int(match.group(4))
        status["deepseek_success_age_ms"] = int(match.group(5))
    if match := STATUS_RE["scheduler"].search(text):
        status["scheduler_ticks"] = int(match.group(1))
        status["scheduler_fired"] = int(match.group(2))
        status["scheduler_failures"] = int(match.group(3))
        status["scheduler_last_ret"] = int(match.group(4))
        status["scheduler_tick_age_ms"] = int(match.group(5))
        status["scheduler_fire_age_ms"] = int(match.group(6))
        status["scheduler_used"] = int(match.group(7))
        status["scheduler_enabled"] = int(match.group(8))
    if match := STATUS_RE["network_recovery"].search(text):
        status["network_recovery_running"] = match.group(1) == "yes"
        status["network_recovery_attempts"] = int(match.group(2))
        status["network_recovery_failures"] = int(match.group(3))
        status["network_recovery_last_ret"] = int(match.group(4))
        status["network_recovery_start_age_ms"] = int(match.group(5))
        status["network_recovery_done_age_ms"] = int(match.group(6))
        status["network_recovery_reason"] = match.group(7)
    if match := STATUS_RE["webserver"].search(text):
        status["webserver_supervisor"] = match.group(1)
        status["webserver_listening"] = match.group(2) == "yes"
        status["webserver_listens"] = int(match.group(3))
        status["webserver_exits"] = int(match.group(4))
        status["webserver_last_ret"] = int(match.group(5))
        status["webserver_last_errno"] = int(match.group(6))
        status["webserver_start_age_ms"] = int(match.group(7))
        status["webserver_exit_age_ms"] = int(match.group(8))
    if match := STATUS_RE["mcp"].search(text):
        status["mcp_requests"] = int(match.group(1))
        status["mcp_failures"] = int(match.group(2))
        status["mcp_notifications"] = int(match.group(3))
        status["mcp_tools"] = int(match.group(4))
        status["mcp_tool_failures"] = int(match.group(5))
        status["mcp_last_ret"] = int(match.group(6))
        status["mcp_last_http"] = int(match.group(7))
        status["mcp_last_dispatch"] = int(match.group(8))
        status["mcp_age_ms"] = int(match.group(9))
    if "guard" not in status and (match := STATUS_RE["guard"].search(text)):
        status["guard"] = match.group(1).strip()
    if match := STATUS_RE["last_tool_error"].search(text):
        status["last_tool_error"] = match.group(1).strip()
    return status


def counter_delta(args: argparse.Namespace, parsed: dict[str, Any],
                  key: str) -> int:
    baselines = getattr(args, "_counter_baseline", None)
    if baselines is None:
        baselines = {
            name: int(parsed.get(name, 0))
            for name in COUNTER_BASELINE_KEYS
        }
        setattr(args, "_counter_baseline", baselines)
        log_event(args, "counter_baseline", **baselines)

    return int(parsed.get(key, 0)) - int(baselines.get(key, 0))


def extract_tool_text(result: dict[str, Any]) -> str:
    try:
        content = result["result"]["content"]
        if content and content[0].get("type") == "text":
            text = content[0].get("text", "")
            parsed = json.loads(text)
            if isinstance(parsed, dict) and "output" in parsed:
                return str(parsed["output"])
            return text
    except (KeyError, TypeError, ValueError):
        pass
    return json.dumps(result)


def parse_system_status_result(result: dict[str, Any]) -> dict[str, Any] | None:
    try:
        status = result["result"].get("structuredContent")
    except (KeyError, AttributeError):
        status = None

    if isinstance(status, dict) and status.get("ok"):
        return status

    try:
        parsed = json.loads(mcp_tool_text(result))
    except (TypeError, ValueError):
        return None

    if isinstance(parsed, dict) and parsed.get("ok"):
        return parsed

    return None


def status_text_from_system_status(result: dict[str, Any]) -> str | None:
    status = parse_system_status_result(result)
    if status is None:
        return None

    lines: list[str] = []
    queues = status.get("queues")
    if isinstance(queues, dict):
        lines.append(
            f"queues: main={int(queues.get('main', 0))} "
            f"agent={int(queues.get('agent', 0))}"
        )

    for key in (
        "router",
        "agent",
        "network_recovery",
        "webserver",
        "telegram",
        "deepseek",
        "scheduler",
        "berry",
        "mcp",
    ):
        value = status.get(key)
        if isinstance(value, str):
            lines.append(value)

    guard = status.get("guard")
    if isinstance(guard, str):
        lines.append("guard_status: " + guard)

    return "\n".join(lines)


def system_status(args: argparse.Namespace) -> dict[str, Any]:
    result = mcp_tool(args, "system.status", {}, timeout=args.status_timeout)
    status = parse_system_status_result(result)
    if status is None:
        raise RuntimeError("system.status returned no parseable status")
    return status


def parse_status_from_system_status(args: argparse.Namespace) -> dict[str, Any]:
    result = mcp_tool(args, "system.status", {}, timeout=args.status_timeout)
    status_text = status_text_from_system_status(result)
    if status_text is None:
        raise RuntimeError("system.status returned no parseable status text")
    return parse_status(status_text)


def parse_status_with_retry(args: argparse.Namespace, deadline: float,
                            sleep_sec: float = 1.0) -> dict[str, Any]:
    last_exc: Exception | None = None

    while time.monotonic() < deadline:
        try:
            return parse_status_from_system_status(args)
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError,
                http.client.IncompleteRead, OSError, RuntimeError) as exc:
            last_exc = exc
            time.sleep(sleep_sec)

    if last_exc is not None:
        raise last_exc
    raise TimeoutError("system.status retry deadline expired")


def mcp_tool_json(result: dict[str, Any]) -> dict[str, Any]:
    try:
        structured = result["result"].get("structuredContent")
    except (KeyError, AttributeError):
        structured = None

    if isinstance(structured, dict):
        return structured

    text = mcp_tool_text(result)
    try:
        parsed = json.loads(text)
    except (TypeError, ValueError):
        return {}

    return parsed if isinstance(parsed, dict) else {}


def mcp_tool_expect_ok(args: argparse.Namespace, name: str,
                       arguments: dict[str, Any],
                       timeout: float | None = None) -> dict[str, Any]:
    result = mcp_tool(args, name, arguments, timeout=timeout)
    if result.get("result", {}).get("isError"):
        raise RuntimeError(f"{name} returned isError")

    body = mcp_tool_json(result)
    if body.get("ok") is not True:
        raise RuntimeError(f"{name} returned non-ok body: {body}")

    return body


def wait_for_telegram_notify_drain(args: argparse.Namespace,
                                   before: dict[str, Any],
                                   deadline: float) -> dict[str, Any]:
    last: dict[str, Any] = {}
    before_enqueued = int(before.get("telegram_notify_enqueued", 0))
    before_sent = int(before.get("telegram_notify_sent", 0))
    before_failed = int(before.get("telegram_notify_failed", 0))
    before_dropped = int(before.get("telegram_notify_dropped", 0))

    while time.monotonic() < deadline:
        last = parse_status_with_retry(args, deadline)
        failed = int(last.get("telegram_notify_failed", 0))
        dropped = int(last.get("telegram_notify_dropped", 0))
        enqueued = int(last.get("telegram_notify_enqueued", 0))
        sent = int(last.get("telegram_notify_sent", 0))
        queued = int(last.get("telegram_notify_queue", 0))
        enqueued_delta = enqueued - before_enqueued
        sent_delta = sent - before_sent

        if failed > before_failed or dropped > before_dropped:
            raise RuntimeError(
                "telegram MCP notification failure: "
                f"failed_delta={failed - before_failed} "
                f"dropped_delta={dropped - before_dropped}"
            )

        if enqueued_delta > 0 and queued == 0 and sent_delta >= enqueued_delta:
            return last

        time.sleep(2)

    raise TimeoutError(f"telegram notify queue did not drain: {last}")


def run_telegram_agent_smoke(args: argparse.Namespace) -> tuple[bool, str]:
    port = resolve_serial_port(args)
    if port is None:
        return False, "telegram agent smoke requires a serial port"

    try:
        before = parse_status_from_system_status(args)
        before_started = int(before.get("agent_started", 0))
        before_done = int(before.get("agent_done", 0))
        before_tools = int(before.get("agent_tools", 0))
        before_replies = int(before.get("agent_replies", 0))
        before_deepseek = int(before.get("deepseek_calls", 0))

        prompt = args.telegram_agent_smoke_prompt.replace('"', "'")
        serial_command(port, f'fruitclaw telegram-inject "{prompt}"',
                       args.telegram_agent_smoke_serial_timeout_sec)

        deadline = time.monotonic() + args.telegram_agent_smoke_timeout_sec
        last: dict[str, Any] = {}
        while time.monotonic() < deadline:
            last = parse_status_with_retry(args, deadline)
            if (int(last.get("agent_started", 0)) > before_started and
                    int(last.get("agent_done", 0)) > before_done and
                    int(last.get("agent_tools", 0)) > before_tools and
                    int(last.get("agent_replies", 0)) > before_replies and
                    int(last.get("deepseek_calls", 0)) > before_deepseek and
                    last.get("agent_last_tool") == "time.now" and
                    int(last.get("agent_last_ret", -1)) == 0 and
                    int(last.get("agent_last_reply_ret", -1)) == 0 and
                    int(last.get("deepseek_http", 0)) == 200):
                log_event(args, "telegram_agent_smoke_ok",
                          agent_done=last.get("agent_done"),
                          agent_tools=last.get("agent_tools"),
                          agent_replies=last.get("agent_replies"),
                          deepseek_calls=last.get("deepseek_calls"),
                          last_tool=last.get("agent_last_tool"))
                return True, "ok"

            if int(last.get("agent_failures", 0)) > int(before.get("agent_failures", 0)):
                break
            if int(last.get("agent_reply_failures", 0)) > int(before.get("agent_reply_failures", 0)):
                break

            time.sleep(3)

        return False, f"telegram agent smoke did not complete: {last}"
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError,
            http.client.IncompleteRead, OSError, RuntimeError) as exc:
        return False, (
            f"telegram agent smoke failed: {type(exc).__name__}: {exc}"
        )


def run_mcp_owner_smoke(args: argparse.Namespace) -> tuple[bool, str]:
    job_id = "mcp-owner-smoke-after"
    script_path = "scripts/mcp_owner_smoke.be"
    marker = "mcp owner smoke " + utc_now()
    op_timeout = args.mcp_owner_smoke_timeout_sec
    script = (
        "import claw\n"
        "claw.reply(\"berry mcp owner smoke\")\n"
        "claw.reply(claw.terminal_run(\"echo berry-terminal-ok\"))\n"
        "claw.reply(claw.schedule_add(\"{\\\"id\\\":\\\"" + job_id +
        "\\\",\\\"type\\\":\\\"once\\\",\\\"after_sec\\\":60,"
        "\\\"prompt\\\":\\\"reply: mcp owner smoke scheduled\\\"}\"))\n"
    )

    try:
        before = parse_status_from_system_status(args)

        log_event(args, "mcp_owner_smoke_step", step="cleanup-list")
        schedules = mcp_tool_expect_ok(args, "scheduler.list", {},
                                       timeout=op_timeout)
        if job_id in str(schedules.get("schedules", "")):
            log_event(args, "mcp_owner_smoke_step", step="cleanup-remove")
            mcp_tool_expect_ok(args, "scheduler.remove", {"id": job_id},
                               timeout=op_timeout)
        log_event(args, "mcp_owner_smoke_step", step="write-script")
        mcp_tool_expect_ok(args, "file.write_limited",
                           {"path": script_path, "text": script},
                           timeout=op_timeout)

        log_event(args, "mcp_owner_smoke_step", step="berry-run")
        berry = mcp_tool_expect_ok(args, "berry.run_script",
                                   {"path": "mcp_owner_smoke.be",
                                    "args_json": "{}"},
                                   timeout=op_timeout)
        reply = str(berry.get("reply", ""))
        if ("berry mcp owner smoke" not in reply or
                "berry-terminal-ok" not in reply or job_id not in reply):
            return False, f"Berry reply missing expected markers: {reply}"

        log_event(args, "mcp_owner_smoke_step", step="schedule-list")
        schedules = mcp_tool_expect_ok(args, "scheduler.list", {},
                                       timeout=op_timeout)
        if job_id not in str(schedules.get("schedules", "")):
            return False, "scheduler.list did not show Berry-created job"

        log_event(args, "mcp_owner_smoke_step", step="schedule-remove")
        mcp_tool_expect_ok(args, "scheduler.remove", {"id": job_id},
                           timeout=op_timeout)
        log_event(args, "mcp_owner_smoke_step", step="schedule-list-clean")
        schedules = mcp_tool_expect_ok(args, "scheduler.list", {},
                                       timeout=op_timeout)
        if job_id in str(schedules.get("schedules", "")):
            return False, "scheduler smoke job remained after remove"

        log_event(args, "mcp_owner_smoke_step", step="terminal")
        terminal = mcp_tool_expect_ok(args, "terminal.run",
                                      {"command": "uname -a"},
                                      timeout=op_timeout)
        if "NuttX" not in str(terminal.get("output", "")):
            return False, "terminal.run uname output did not contain NuttX"

        log_event(args, "mcp_owner_smoke_step", step="device-list")
        devices = mcp_tool_expect_ok(args, "device.list", {},
                                     timeout=op_timeout)
        listed = devices.get("devices")
        if not isinstance(listed, list) or "leds0" not in listed:
            return False, "device.list did not include leds0"

        log_event(args, "mcp_owner_smoke_step", step="device-read")
        read_null = mcp_tool_expect_ok(args, "device.read",
                                       {"path": "/dev/null", "max_bytes": 4},
                                       timeout=op_timeout)
        if int(read_null.get("bytes", -1)) != 0:
            return False, "device.read /dev/null returned bytes"

        log_event(args, "mcp_owner_smoke_step", step="device-write")
        write_null = mcp_tool_expect_ok(args, "device.write",
                                        {"path": "/dev/null",
                                         "mode": "text",
                                         "data": "fruitclaw"},
                                        timeout=op_timeout)
        if int(write_null.get("bytes", -1)) != len("fruitclaw"):
            return False, "device.write /dev/null byte count mismatch"

        log_event(args, "mcp_owner_smoke_step", step="neopixels-blue")
        mcp_tool_expect_ok(args, "neopixels.set",
                           {"effect": "fill", "color": "blue",
                            "brightness": 32},
                           timeout=op_timeout)
        time.sleep(0.5)
        log_event(args, "mcp_owner_smoke_step", step="neopixels-off")
        mcp_tool_expect_ok(args, "neopixels.off", {},
                           timeout=op_timeout)

        log_event(args, "mcp_owner_smoke_step", step="memory-append")
        mcp_tool_expect_ok(args, "memory.append", {"text": marker},
                           timeout=op_timeout)
        log_event(args, "mcp_owner_smoke_step", step="memory-read")
        memory = mcp_tool_expect_ok(args, "memory.read",
                                    {"max_bytes": 2048},
                                    timeout=op_timeout)
        if marker not in str(memory.get("text", "")):
            return False, "memory.read did not include owner smoke marker"

        if args.mcp_owner_smoke_require_telegram_notify:
            log_event(args, "mcp_owner_smoke_step", step="notify-drain")
            deadline = time.monotonic() + args.mcp_owner_smoke_notify_timeout_sec
            notify = wait_for_telegram_notify_drain(args, before, deadline)
        else:
            notify = parse_status_from_system_status(args)

        log_event(args, "mcp_owner_smoke_ok",
                  notify_enqueued=notify.get("telegram_notify_enqueued"),
                  notify_sent=notify.get("telegram_notify_sent"),
                  mcp_tools=notify.get("mcp_tools"))
        return True, "ok"
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError,
            http.client.IncompleteRead, OSError, RuntimeError) as exc:
        try:
            mcp_tool(args, "scheduler.remove", {"id": job_id},
                     timeout=args.status_timeout)
            mcp_tool(args, "neopixels.off", {}, timeout=args.status_timeout)
        except Exception:
            pass
        return False, f"MCP owner smoke failed: {type(exc).__name__}: {exc}"


def run_scheduler_smoke(args: argparse.Namespace) -> tuple[bool, str]:
    job_id = "soak_schedule_smoke"
    prompt = "reply: FruitClaw scheduler smoke fired"

    try:
        before = parse_status_from_system_status(args)
        before_fired = int(before.get("scheduler_fired", 0))
        before_done = int(before.get("agent_done", 0))

        listed = mcp_tool(args, "scheduler.list", {},
                          timeout=args.status_timeout)
        if job_id in mcp_tool_text(listed):
            mcp_tool(args, "scheduler.remove", {"id": job_id},
                     timeout=args.status_timeout)

        add = mcp_tool(
            args,
            "scheduler.add",
            {
                "id": job_id,
                "type": "once",
                "after_sec": max(1, args.scheduler_smoke_after_sec),
                "prompt": prompt,
            },
            timeout=args.status_timeout,
        )
        if add.get("result", {}).get("isError"):
            return False, "scheduler.add returned isError"

        deadline = time.monotonic() + args.scheduler_smoke_timeout_sec
        last: dict[str, Any] = {}
        while time.monotonic() < deadline:
            time.sleep(2)
            last = parse_status_with_retry(args, deadline)
            if (int(last.get("scheduler_fired", 0)) > before_fired and
                    int(last.get("agent_done", 0)) > before_done and
                    last.get("router_last_source") == "scheduler"):
                break
        else:
            mcp_tool(args, "scheduler.remove", {"id": job_id},
                     timeout=args.status_timeout)
            return False, (
                "scheduler smoke did not fire: "
                f"before_fired={before_fired} last={last}"
            )

        remove = mcp_tool(args, "scheduler.remove", {"id": job_id},
                          timeout=args.status_timeout)
        if remove.get("result", {}).get("isError"):
            return False, "scheduler.remove returned isError"

        listed = mcp_tool(args, "scheduler.list", {},
                          timeout=args.status_timeout)
        if job_id in mcp_tool_text(listed):
            return False, "scheduler smoke job still listed after remove"
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError,
            http.client.IncompleteRead, OSError, RuntimeError) as exc:
        return False, (
            f"scheduler smoke failed: {type(exc).__name__}: {exc}"
        )

    return True, "ok"


def run_reset_smoke(args: argparse.Namespace) -> tuple[bool, str]:
    marker_path = "notes/reset_smoke.txt"
    marker = "fruitclaw reset smoke " + utc_now()
    port = resolve_serial_port(args)

    if port is None:
        return False, "reset smoke requires a serial port"

    try:
        write = mcp_tool(
            args,
            "file.write_limited",
            {"path": marker_path, "text": marker + "\n"},
            timeout=args.status_timeout,
        )
        if write.get("result", {}).get("isError"):
            return False, "reset marker write returned isError"

        serial_send(port, "fruitclaw reboot", args.serial_timeout)
        log_event(args, "reset_smoke_reboot_sent", serial=port)

        ok, reason = wait_for_mcp(args, args.reset_smoke_timeout_sec)
        if not ok:
            return False, reason

        deadline = time.monotonic() + args.reset_smoke_timeout_sec

        last_cdc = ""
        while time.monotonic() < deadline:
            if reason := bootsel_failure(args):
                return False, reason

            port = resolve_serial_port(args)
            if port is None:
                last_cdc = "serial port missing after reset"
                time.sleep(2)
                continue

            try:
                serial_out = serial_command(port, "echo fruitclaw-reset-cdc-ok",
                                            args.serial_timeout)
                if ("fruitclaw-reset-cdc-ok" in serial_out and
                        "nsh>" in serial_out):
                    break
                last_cdc = f"CDC prompt stale after reset on {port}"
            except (OSError, RuntimeError, TimeoutError) as exc:
                last_cdc = f"{type(exc).__name__}: {exc}"
            time.sleep(2)
        else:
            return False, last_cdc or "CDC did not return after reset"

        last_marker = ""
        while time.monotonic() < deadline:
            if reason := bootsel_failure(args):
                return False, reason

            try:
                read = mcp_tool(
                    args,
                    "file.read",
                    {"path": marker_path, "max_bytes": 256},
                    timeout=args.status_timeout,
                )
                if read.get("result", {}).get("isError"):
                    last_marker = "reset marker read returned isError"
                elif marker in mcp_tool_text(read):
                    break
                else:
                    last_marker = "reset marker did not persist across reboot"
            except (urllib.error.URLError, TimeoutError, json.JSONDecodeError,
                    http.client.IncompleteRead, OSError) as exc:
                last_marker = f"{type(exc).__name__}: {exc}"
            time.sleep(2)
        else:
            return False, last_marker or "reset marker read did not complete"

        last: dict[str, Any] = {}
        while time.monotonic() < deadline:
            if reason := bootsel_failure(args):
                return False, reason

            try:
                status = system_status(args)
                status_text = status_text_from_system_status(
                    {"result": {"structuredContent": status}}
                )
                parsed = parse_status(status_text or "")
                tools_count = int(status.get("visible_tools", 0))
                if args.min_mcp_tools > 0:
                    tools_count = mcp_tools_count(args)
            except (urllib.error.URLError, TimeoutError,
                    json.JSONDecodeError, http.client.IncompleteRead,
                    OSError, RuntimeError) as exc:
                last = {"error": f"{type(exc).__name__}: {exc}"}
                time.sleep(2)
                continue

            last = {
                "data_dir": status.get("data_dir"),
                "telegram_http": parsed.get("telegram_http"),
                "webserver_listening": parsed.get("webserver_listening"),
                "mcp_tools": tools_count,
            }

            if (status.get("data_dir") == "/mnt/sd0/fruitclaw" and
                    parsed.get("telegram_http") == 200 and
                    parsed.get("webserver_listening", False) and
                    tools_count >= args.min_mcp_tools):
                baseline = {
                    name: int(parsed.get(name, 0))
                    for name in COUNTER_BASELINE_KEYS
                }
                setattr(args, "_counter_baseline", baseline)
                log_event(args, "counter_baseline_reset", **baseline)
                log_event(args, "reset_smoke_ok", **last)
                return True, "ok"

            time.sleep(2)

        return False, f"post-reset health did not settle: {last}"
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError,
            http.client.IncompleteRead, OSError, RuntimeError) as exc:
        return False, f"reset smoke failed: {type(exc).__name__}: {exc}"


def check_once(args: argparse.Namespace, iteration: int) -> tuple[bool, str]:
    bootsel, boot_info = picotool_info(args)
    if bootsel:
        return False, "board is already in BOOTSEL: " + boot_info.splitlines()[0]

    ok, reason = mcp_ping_ok(args, iteration, timeout=args.command_timeout)
    if not ok:
        refreshed, refresh_reason = refresh_mcp_endpoint(
            args, f"check-{iteration}-ping"
        )
        if not refreshed:
            failure = f"MCP ping failed: {reason}; refresh={refresh_reason}"
            if tolerate_mcp_health_failure(args, "ping", failure):
                return True, failure
            return False, failure

    if args.check_docs and not getattr(args, "_docs_checked", False):
        ok, reason = check_static_docs(args)
        if not ok:
            refreshed, refresh_reason = refresh_mcp_endpoint(
                args, f"check-{iteration}-docs"
            )
            if not refreshed:
                failure = f"{reason}; refresh={refresh_reason}"
                if tolerate_mcp_health_failure(args, "docs", failure):
                    return True, failure
                return False, failure
            ok, reason = check_static_docs(args)
            if not ok:
                if tolerate_mcp_health_failure(args, "docs", reason):
                    return True, reason
                return False, reason
        setattr(args, "_docs_checked", True)
        log_event(args, "docs_check_ok")

    tools_count = getattr(args, "_mcp_tools_list_count", 0)
    if args.min_mcp_tools > 0:
        if not getattr(args, "_mcp_tools_checked", False):
            try:
                tools_count = mcp_tools_count(args)
            except (urllib.error.URLError, TimeoutError, json.JSONDecodeError,
                    http.client.IncompleteRead, OSError) as exc:
                refreshed, refresh_reason = refresh_mcp_endpoint(
                    args, f"check-{iteration}-tools"
                )
                if not refreshed:
                    failure = (
                        f"MCP tools/list failed: {type(exc).__name__}: {exc}; "
                        f"refresh={refresh_reason}"
                    )
                    if tolerate_mcp_health_failure(args, "tools", failure):
                        return True, failure
                    return False, failure
                try:
                    tools_count = mcp_tools_count(args)
                except (urllib.error.URLError, TimeoutError,
                        json.JSONDecodeError, http.client.IncompleteRead,
                        OSError) as retry_exc:
                    failure = (
                        "MCP tools/list failed after refresh: "
                        f"{type(retry_exc).__name__}: {retry_exc}"
                    )
                    if tolerate_mcp_health_failure(args, "tools", failure):
                        return True, failure
                    return False, failure

            if tools_count < args.min_mcp_tools:
                return False, (
                    f"MCP tools/list too small: {tools_count} < "
                    f"{args.min_mcp_tools}"
                )

            setattr(args, "_mcp_tools_checked", True)
            setattr(args, "_mcp_tools_list_count", tools_count)
            log_event(args, "mcp_tools_check_ok", tools=tools_count)

    try:
        status_resp = mcp_tool(args, "system.status", {}, timeout=args.status_timeout)
        status_text = status_text_from_system_status(status_resp)
        if status_text is None:
            status_resp = mcp_tool(
                args,
                "terminal.run",
                {"command": "fruitclaw status"},
                timeout=args.status_timeout,
            )
            status_text = extract_tool_text(status_resp)
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError,
            http.client.IncompleteRead, OSError) as exc:
        refreshed, refresh_reason = refresh_mcp_endpoint(
            args, f"check-{iteration}-status"
        )
        if not refreshed:
            failure = (
                "fruitclaw status through MCP failed: "
                f"{type(exc).__name__}: {exc}; refresh={refresh_reason}"
            )
            if tolerate_mcp_health_failure(args, "status", failure):
                return True, failure
            return False, failure
        try:
            status_resp = mcp_tool(args, "system.status", {},
                                   timeout=args.status_timeout)
            status_text = status_text_from_system_status(status_resp)
            if status_text is None:
                status_resp = mcp_tool(
                    args,
                    "terminal.run",
                    {"command": "fruitclaw status"},
                    timeout=args.status_timeout,
                )
                status_text = extract_tool_text(status_resp)
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError,
                http.client.IncompleteRead, OSError) as retry_exc:
            failure = (
                "fruitclaw status through MCP failed after refresh: "
                f"{type(retry_exc).__name__}: {retry_exc}"
            )
            if tolerate_mcp_health_failure(args, "status", failure):
                return True, failure
            return False, failure

    parsed = parse_status(status_text)
    if parsed.get("main_queue", 0) > args.max_queue_depth:
        return False, f"main queue depth too high: {parsed['main_queue']}"
    if parsed.get("agent_queue", 0) > args.max_queue_depth:
        return False, f"agent queue depth too high: {parsed['agent_queue']}"
    if parsed.get("telegram_poll_age_ms", 0) > args.max_telegram_poll_age_ms:
        return False, (
            "telegram poll stale: "
            f"{parsed['telegram_poll_age_ms']} ms"
        )
    telegram_fail_delta = counter_delta(args, parsed, "telegram_fails")
    if telegram_fail_delta > args.max_telegram_failures:
        return False, f"telegram failures exceeded: +{telegram_fail_delta}"
    router_fail_delta = counter_delta(args, parsed, "router_failed")
    if router_fail_delta > args.max_router_failures:
        return False, f"router failures exceeded: +{router_fail_delta}"
    agent_fail_delta = counter_delta(args, parsed, "agent_failures")
    if agent_fail_delta > args.max_agent_failures:
        return False, f"agent failures exceeded: +{agent_fail_delta}"
    agent_tool_fail_delta = counter_delta(args, parsed, "agent_tool_failures")
    if agent_tool_fail_delta > args.max_agent_tool_failures:
        return False, (
            "agent tool failures exceeded: "
            f"+{agent_tool_fail_delta}"
        )
    agent_reply_fail_delta = counter_delta(args, parsed, "agent_reply_failures")
    if agent_reply_fail_delta > args.max_agent_reply_failures:
        return False, (
            "agent reply failures exceeded: "
            f"+{agent_reply_fail_delta}"
        )
    scheduler_fail_delta = counter_delta(args, parsed, "scheduler_failures")
    if scheduler_fail_delta > args.max_scheduler_failures:
        return False, (
            "scheduler failures exceeded: "
            f"+{scheduler_fail_delta}"
        )
    recovery_fail_delta = counter_delta(args, parsed,
                                        "network_recovery_failures")
    if recovery_fail_delta > args.max_network_recovery_failures:
        return False, (
            "network recovery failures exceeded: "
            f"+{recovery_fail_delta}"
        )
    if (parsed.get("webserver_supervisor") == "started" and
            not parsed.get("webserver_listening", True)):
        return False, "webserver supervisor is not listening"
    webserver_exit_delta = counter_delta(args, parsed, "webserver_exits")
    if webserver_exit_delta > args.max_webserver_exits:
        return False, f"webserver exits exceeded: +{webserver_exit_delta}"
    scheduler_tick_age = parsed.get("scheduler_tick_age_ms", -1)
    if scheduler_tick_age > args.max_scheduler_tick_age_ms:
        scheduler_ticks = parsed.get("scheduler_ticks", 0)
        if iteration <= 2 or scheduler_ticks <= 1:
            log_event(args, "scheduler_tick_warmup",
                      iteration=iteration,
                      scheduler_ticks=scheduler_ticks,
                      scheduler_tick_age_ms=scheduler_tick_age)
        else:
            return False, f"scheduler tick stale: {scheduler_tick_age} ms"
    mcp_fail_delta = counter_delta(args, parsed, "mcp_failures")
    if mcp_fail_delta > args.max_mcp_failures:
        return False, f"MCP failures exceeded: +{mcp_fail_delta}"
    mcp_tool_fail_delta = counter_delta(args, parsed, "mcp_tool_failures")
    if mcp_tool_fail_delta > args.max_mcp_tool_failures:
        return False, (
            "MCP tool failures exceeded: "
            f"+{mcp_tool_fail_delta}"
        )
    if args.strict_tool_errors and parsed.get("last_tool_error") not in (None, "none"):
        return False, f"last tool error is set: {parsed['last_tool_error']}"
    if args.min_mcp_tools > 0:
        parsed["mcp_tools_list_count"] = tools_count

    if args.scheduler_smoke and not getattr(args, "_scheduler_smoke_done",
                                            False):
        log_event(args, "scheduler_smoke_start", iteration=iteration)
        ok, reason = run_scheduler_smoke(args)
        if not ok:
            log_event(args, "scheduler_smoke_failed", reason=reason)
            return False, reason
        setattr(args, "_scheduler_smoke_done", True)
        parsed["scheduler_smoke"] = "ok"
        log_event(args, "scheduler_smoke_ok")

    if args.reset_smoke and not getattr(args, "_reset_smoke_done", False):
        ok, reason = run_reset_smoke(args)
        if not ok:
            return False, reason
        setattr(args, "_reset_smoke_done", True)
        parsed = parse_status_from_system_status(args)
        if getattr(args, "_scheduler_smoke_done", False):
            parsed["scheduler_smoke"] = "ok"
        parsed["reset_smoke"] = "ok"

    if args.mcp_owner_smoke and not getattr(args, "_mcp_owner_smoke_done",
                                            False):
        log_event(args, "mcp_owner_smoke_start", iteration=iteration)
        ok, reason = run_mcp_owner_smoke(args)
        if not ok:
            log_event(args, "mcp_owner_smoke_failed", reason=reason)
            return False, reason
        setattr(args, "_mcp_owner_smoke_done", True)
        parsed = parse_status_from_system_status(args)
        parsed["mcp_owner_smoke"] = "ok"

    if args.telnet_cr_smoke and not getattr(args, "_telnet_cr_smoke_done",
                                            False):
        log_event(args, "telnet_cr_smoke_start", iteration=iteration)
        ok, reason = run_telnet_cr_smoke(args)
        if not ok:
            log_event(args, "telnet_cr_smoke_failed", reason=reason)
            return False, reason
        setattr(args, "_telnet_cr_smoke_done", True)
        parsed["telnet_cr_smoke"] = "ok"
        log_event(args, "telnet_cr_smoke_ok")

    if args.telegram_agent_smoke and not getattr(args,
                                                 "_telegram_agent_smoke_done",
                                                 False):
        log_event(args, "telegram_agent_smoke_start", iteration=iteration)
        ok, reason = run_telegram_agent_smoke(args)
        if not ok:
            log_event(args, "telegram_agent_smoke_failed", reason=reason)
            return False, reason
        setattr(args, "_telegram_agent_smoke_done", True)
        parsed = parse_status_from_system_status(args)
        parsed["telegram_agent_smoke"] = "ok"

    port = resolve_serial_port(args)
    if port is not None and serial_check_due(args, time.monotonic()):
        try:
            serial_out = serial_command(port, "echo fruitclaw-serial-ok",
                                        args.serial_timeout)
            if "fruitclaw-serial-ok" not in serial_out or "nsh>" not in serial_out:
                raise RuntimeError(f"serial prompt stale on {port}")
            parsed["serial_check"] = "ok"
            setattr(args, "_serial_check_failures", 0)
        except (OSError, RuntimeError, TimeoutError) as exc:
            failures = int(getattr(args, "_serial_check_failures", 0)) + 1
            setattr(args, "_serial_check_failures", failures)
            parsed["serial_check"] = f"failed:{failures}"
            log_event(args, "serial_check_failed", serial=port,
                      failures=failures,
                      max_failures=args.max_serial_failures,
                      error=f"{type(exc).__name__}: {exc}")
            if failures > args.max_serial_failures:
                return False, (
                    f"serial check failed on {port}: "
                    f"{type(exc).__name__}: {exc}"
                )
    elif args.require_serial:
        return False, "serial required but no serial port found"

    reset_mcp_health_failures(args)
    log_event(args, "check_ok", iteration=iteration, **parsed)
    return True, "ok"


def recover_to_bootsel(args: argparse.Namespace, reason: str) -> bool:
    log_event(args, "recover_start", reason=reason)

    bootsel, info = picotool_info(args)
    if bootsel:
        log_event(args, "recover_bootsel_already", info=info.splitlines()[0])
        return True

    if not args.no_picotool_force_reboot:
        rc, out = picotool_force_bootsel(args)
        log_event(args, "recover_picotool_force_reboot_done", rc=rc,
                  output_tail=out[-400:])
        ok, info = wait_for_bootsel(args, args.recovery_wait_sec)
        if ok:
            log_event(args, "recover_bootsel_ok", method="picotool-force",
                      info=info.splitlines()[0])
            return True

    try:
        mcp_tool(args, "terminal.run", {"command": "bootsel"}, timeout=4)
    except Exception as exc:  # Board usually disappears before HTTP completes.
        log_event(args, "recover_mcp_bootsel_sent", result=type(exc).__name__)
    ok, info = wait_for_bootsel(args, args.recovery_wait_sec)
    if ok:
        log_event(args, "recover_bootsel_ok", method="mcp", info=info.splitlines()[0])
        return True

    if not args.no_http_recovery:
        for url in http_bootsel_urls(args):
            try:
                result = http_bootsel(args, url)
                log_event(args, "recover_http_bootsel_sent", url=url,
                          result=result)
            except Exception as exc:
                log_event(args, "recover_http_bootsel_failed", url=url,
                          error=str(exc))
                continue

            ok, info = wait_for_bootsel(args, args.recovery_wait_sec)
            if ok:
                log_event(args, "recover_bootsel_ok", method="http",
                          info=info.splitlines()[0], url=url)
                return True

    if not args.no_telnet_recovery:
        try:
            out = telnet_command(args, "fruitclaw recover")
            log_event(args, "recover_telnet_fruitclaw_recover_sent",
                      output_tail=out[-240:])
        except Exception as exc:
            log_event(args, "recover_telnet_fruitclaw_recover_failed",
                      error=str(exc))
        ok, info = wait_for_bootsel(args, args.recovery_wait_sec)
        if ok:
            log_event(args, "recover_bootsel_ok", method="telnet",
                      info=info.splitlines()[0])
            return True

        try:
            out = telnet_command(args, "bootsel")
            log_event(args, "recover_telnet_bootsel_sent",
                      output_tail=out[-240:])
        except Exception as exc:
            log_event(args, "recover_telnet_bootsel_failed", error=str(exc))
        ok, info = wait_for_bootsel(args, args.recovery_wait_sec)
        if ok:
            log_event(args, "recover_bootsel_ok", method="telnet-bootsel",
                      info=info.splitlines()[0])
            return True

    port = resolve_serial_port(args)
    if port is not None:
        try:
            serial_send(port, "\x03", args.serial_timeout)
            time.sleep(0.5)
            log_event(args, "recover_serial_ctrl_c_sent", port=port)
            serial_command(port, "fruitclaw recover", args.serial_timeout)
            log_event(args, "recover_serial_fruitclaw_recover_sent")
        except Exception as exc:
            log_event(args, "recover_serial_fruitclaw_recover_failed",
                      error=str(exc))
        ok, info = wait_for_bootsel(args, args.recovery_wait_sec)
        if ok:
            log_event(args, "recover_bootsel_ok", method="serial-fruitclaw",
                      info=info.splitlines()[0])
            return True

        try:
            serial_command(port, "bootsel", args.serial_timeout)
        except Exception as exc:
            log_event(args, "recover_serial_bootsel_failed", error=str(exc))
        ok, info = wait_for_bootsel(args, args.recovery_wait_sec)
        if ok:
            log_event(args, "recover_bootsel_ok", method="serial",
                      info=info.splitlines()[0])
            return True

        try:
            serial_touch_1200(port, args.serial_timeout)
        except Exception as exc:
            log_event(args, "recover_1200_touch_failed", error=str(exc))
        ok, info = wait_for_bootsel(args, args.recovery_wait_sec)
        if ok:
            log_event(args, "recover_bootsel_ok", method="1200-baud",
                      info=info.splitlines()[0])
            return True

    if args.manual_bootsel_wait_sec > 0:
        log_event(args, "recover_manual_bootsel_wait_start",
                  timeout_sec=args.manual_bootsel_wait_sec)
        ok, info = wait_for_bootsel(args, args.manual_bootsel_wait_sec)
        if ok:
            log_event(args, "recover_bootsel_ok", method="manual-wait",
                      info=info.splitlines()[0])
            return True

    log_event(args, "recover_bootsel_failed", last_info=info)
    return False


def recover_and_flash(args: argparse.Namespace, reason: str) -> tuple[bool, int]:
    attempts = 0

    while args.recovery_attempts <= 0 or attempts < args.recovery_attempts:
        attempts += 1
        log_event(args, "recover_attempt", attempt=attempts,
                  max_attempts=args.recovery_attempts)
        if recover_to_bootsel(args, reason):
            if maybe_flash(args):
                return True, 0

            return False, 4

        if args.recovery_attempts > 0 and attempts >= args.recovery_attempts:
            break

        if args.recovery_retry_delay_sec > 0:
            time.sleep(args.recovery_retry_delay_sec)

    return False, 3


def maybe_flash(args: argparse.Namespace) -> bool:
    if not args.flash_uf2:
        return True

    if not os.path.exists(args.flash_uf2):
        log_event(args, "flash_missing", path=args.flash_uf2)
        return False

    rc, out = run_cmd([args.picotool, "load", "-v", "-x", args.flash_uf2],
                      args.flash_timeout)
    log_event(args, "flash_done", rc=rc, output_tail=out[-800:])
    if rc != 0:
        return False

    if not args.continue_after_flash:
        return True

    if args.provision_from_env:
        if not provision_from_env(args, "after-flash"):
            return False
        if args.provision_wait_for_mcp:
            ok, reason = wait_for_mcp(args, args.reset_smoke_timeout_sec)
            if ok:
                log_event(args, "flash_app_ok")
                return True

            log_event(args, "flash_app_after_provision_failed",
                      reason=reason)
            return False

        log_event(args, "flash_app_provisioned")
        return True

    ok, reason = wait_for_mcp(args, args.reset_smoke_timeout_sec)
    if ok:
        log_event(args, "flash_app_ok")
        return True

    bootsel, info = picotool_info(args)
    if bootsel:
        rc, out = run_cmd([args.picotool, "reboot"], args.command_timeout)
        log_event(args, "flash_plain_reboot", rc=rc,
                  output_tail=out[-400:],
                  bootsel_info=info.splitlines()[0] if info else "BOOTSEL")
        if rc != 0:
            return False
        ok, reason = wait_for_mcp(args, args.reset_smoke_timeout_sec)
        if ok:
            log_event(args, "flash_app_ok")
            return True

    log_event(args, "flash_app_failed", reason=reason)
    return False


def missing_requested_smokes(args: argparse.Namespace) -> list[str]:
    missing: list[str] = []

    if args.check_docs and not getattr(args, "_docs_checked", False):
        missing.append("docs")
    if args.min_mcp_tools > 0 and not getattr(args, "_mcp_tools_checked",
                                               False):
        missing.append("mcp-tools")
    if args.scheduler_smoke and not getattr(args, "_scheduler_smoke_done",
                                            False):
        missing.append("scheduler-smoke")
    if args.reset_smoke and not getattr(args, "_reset_smoke_done", False):
        missing.append("reset-smoke")
    if args.mcp_owner_smoke and not getattr(args, "_mcp_owner_smoke_done",
                                            False):
        missing.append("mcp-owner-smoke")
    if args.telnet_cr_smoke and not getattr(args, "_telnet_cr_smoke_done",
                                            False):
        missing.append("telnet-cr-smoke")
    if args.telegram_agent_smoke and not getattr(args,
                                                 "_telegram_agent_smoke_done",
                                                 False):
        missing.append("telegram-agent-smoke")
    if int(getattr(args, "_mcp_health_failures", 0)) > 0:
        missing.append("mcp-health-recovered")

    return missing


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device-ip", default=DEFAULT_DEVICE_IP)
    parser.add_argument("--mcp-url")
    parser.add_argument("--serial", default="auto",
                        help="serial port path, 'auto', or use --no-serial")
    parser.add_argument("--serial-glob", default=DEFAULT_SERIAL_GLOB)
    parser.add_argument("--no-serial", action="store_true")
    parser.add_argument("--require-serial", action="store_true")
    parser.add_argument("--rediscover-ip-on-failure",
                        action=argparse.BooleanOptionalAction, default=True,
                        help="when HTTP/MCP appears stale, query the board "
                        "over serial for the current wlan0 address before "
                        "recovering")
    parser.add_argument("--serial-ip-command", action="append",
                        default=["ifconfig wlan0"],
                        help="serial command used to discover the board IP; "
                        "may be repeated")
    parser.add_argument("--serial-ip-timeout-sec", type=float, default=8)
    parser.add_argument("--ip-rediscover-min-interval-sec", type=float,
                        default=10)
    parser.add_argument("--ip-rediscover-settle-sec", type=float, default=2)
    parser.add_argument("--current-endpoint-retry-sec", type=float, default=2)
    parser.add_argument("--serial-network-recover",
                        action=argparse.BooleanOptionalAction, default=True,
                        help="when MCP remains down but CDC works, run "
                        "fruitclaw wifi-up over serial before BOOTSEL "
                        "recovery")
    parser.add_argument("--serial-network-recover-timeout-sec", type=float,
                        default=180)
    parser.add_argument("--serial-check-interval-sec", type=float,
                        default=DEFAULT_SERIAL_CHECK_INTERVAL_SEC,
                        help="minimum seconds between NSH prompt probes; "
                        "0 checks every iteration, negative disables")
    parser.add_argument("--max-serial-failures", type=int, default=3,
                        help="consecutive CDC prompt probe failures tolerated "
                        "while MCP/web health is otherwise passing")
    parser.add_argument("--no-http-recovery", action="store_true")
    parser.add_argument("--http-bootsel-url", action="append")
    parser.add_argument("--http-bootsel-path", action="append",
                        default=list(DEFAULT_HTTP_BOOTSEL_PATHS))
    parser.add_argument("--no-telnet-recovery", action="store_true")
    parser.add_argument("--telnet-port", type=int, default=23)
    parser.add_argument("--telnet-timeout", type=float, default=5)
    parser.add_argument("--telnet-cr-smoke", action="store_true",
                        help="once per run, verify telnet Enter handling "
                        "with CR, LF, and CRLF line endings")
    parser.add_argument("--picotool", default="picotool")
    parser.add_argument("--no-picotool-force-reboot", action="store_true")
    parser.add_argument("--duration-sec", type=float, default=300)
    parser.add_argument("--interval-sec", type=float, default=60)
    parser.add_argument("--command-timeout", type=float, default=10)
    parser.add_argument("--status-timeout", type=float, default=25)
    parser.add_argument("--serial-timeout", type=float, default=4)
    parser.add_argument("--recovery-wait-sec", type=float, default=20)
    parser.add_argument("--recovery-attempts", type=int, default=1,
                        help="BOOTSEL recovery attempts; <=0 retries forever")
    parser.add_argument("--recovery-retry-delay-sec", type=float, default=5)
    parser.add_argument("--manual-bootsel-wait-sec", type=float, default=0,
                        help="after scripted recovery fails, keep polling "
                        "picotool for this many seconds before failing")
    parser.add_argument("--flash-timeout", type=float, default=60)
    parser.add_argument("--flash-uf2")
    parser.add_argument("--no-recover", action="store_true")
    parser.add_argument("--continue-after-flash", action="store_true")
    parser.add_argument("--max-queue-depth", type=int, default=4)
    parser.add_argument("--max-router-failures", type=int, default=0)
    parser.add_argument("--max-agent-failures", type=int, default=0)
    parser.add_argument("--max-agent-tool-failures", type=int, default=0)
    parser.add_argument("--max-agent-reply-failures", type=int, default=0)
    parser.add_argument("--max-scheduler-failures", type=int, default=0)
    parser.add_argument("--max-scheduler-tick-age-ms", type=int,
                        default=10000)
    parser.add_argument("--max-network-recovery-failures", type=int,
                        default=0)
    parser.add_argument("--max-webserver-exits", type=int, default=8)
    parser.add_argument("--max-mcp-failures", type=int, default=0)
    parser.add_argument("--max-mcp-tool-failures", type=int, default=0)
    parser.add_argument("--max-mcp-health-failures", type=int, default=3,
                        help="consecutive MCP/web reachability failures "
                        "allowed before recovery; this gives transient "
                        "network stalls time to clear")
    parser.add_argument("--min-mcp-tools", type=int, default=0,
                        help="fail if MCP tools/list returns fewer tools; "
                        "0 disables this check")
    parser.add_argument("--check-docs", "--check-wiki", dest="check_docs",
                        action="store_true",
                        help="also verify static GET /index.html and "
                        "/docs/index.json")
    parser.add_argument("--scheduler-smoke", action="store_true",
                        help="once per run, add a short one-shot schedule, "
                        "wait for it to fire, then remove it")
    parser.add_argument("--scheduler-smoke-after-sec", type=int, default=3)
    parser.add_argument("--scheduler-smoke-timeout-sec", type=float,
                        default=90)
    parser.add_argument("--reset-smoke", action="store_true",
                        help="once per run, write a persistent marker, issue "
                        "a normal CDC reboot, then verify MCP/CDC/SD health")
    parser.add_argument("--reset-smoke-timeout-sec", type=float, default=180)
    parser.add_argument("--mcp-owner-smoke", action="store_true",
                        help="once per run, verify MCP owner tools: Berry, "
                        "scheduler, terminal, device, NeoPixels, memory")
    parser.add_argument("--mcp-owner-smoke-timeout-sec", type=float,
                        default=45)
    parser.add_argument("--mcp-owner-smoke-require-telegram-notify",
                        action=argparse.BooleanOptionalAction,
                        default=True,
                        help="require MCP tool-call Telegram notifications "
                        "to drain after the owner-tool smoke")
    parser.add_argument("--mcp-owner-smoke-notify-timeout-sec", type=float,
                        default=90)
    parser.add_argument("--telegram-agent-smoke", action="store_true",
                        help="once per run, inject a Telegram event and "
                        "verify DeepSeek tool call plus final reply")
    parser.add_argument("--telegram-agent-smoke-prompt",
                        default="Use the time.now tool now. Reply in one "
                                "short sentence with FruitClaw time tool ok.")
    parser.add_argument("--telegram-agent-smoke-timeout-sec", type=float,
                        default=120)
    parser.add_argument("--telegram-agent-smoke-serial-timeout-sec",
                        type=float, default=12)
    parser.add_argument("--max-telegram-poll-age-ms", type=int, default=120000)
    parser.add_argument("--max-telegram-failures", type=int, default=0)
    parser.add_argument("--provision-from-env", action="store_true",
                        help="after startup/recovery, restore volatile "
                        "FruitClaw Wi-Fi/secrets from FRUITCLAW_* env vars")
    parser.add_argument("--provision-wifi-up",
                        action=argparse.BooleanOptionalAction, default=True,
                        help="run fruitclaw wifi-up after provisioning")
    parser.add_argument("--provision-wait-for-mcp",
                        action=argparse.BooleanOptionalAction, default=True,
                        help="after provisioning, wait for MCP to come back")
    parser.add_argument("--provision-serial-wait-sec", type=float,
                        default=45)
    parser.add_argument("--provision-command-timeout", type=float,
                        default=30)
    parser.add_argument("--provision-wifi-timeout", type=float,
                        default=90)
    parser.add_argument("--strict-tool-errors", action="store_true")
    parser.add_argument("--log")
    args = parser.parse_args()

    args._mcp_url_explicit = args.mcp_url is not None
    if args.mcp_url is None:
        args.mcp_url = f"http://{args.device_ip}/mcp"

    deadline = None if args.duration_sec <= 0 else time.monotonic() + args.duration_sec
    iteration = 0
    log_event(args, "soak_start", mcp_url=args.mcp_url,
              serial=resolve_serial_port(args) or "-",
              duration_sec=args.duration_sec,
              recovery_attempts=args.recovery_attempts,
              picotool_force_reboot=not args.no_picotool_force_reboot,
              http_bootsel_urls=http_bootsel_urls(args),
              telnet="disabled" if args.no_telnet_recovery else
              f"{args.device_ip}:{args.telnet_port}")

    if not provision_from_env(args, "startup"):
        return 6
    if args.provision_from_env and args.provision_wait_for_mcp:
        ok, reason = wait_for_mcp(args, args.reset_smoke_timeout_sec)
        if not ok:
            log_event(args, "startup_after_provision_failed", reason=reason)
            return 6

    while deadline is None or time.monotonic() < deadline:
        iteration += 1
        ok, reason = check_once(args, iteration)
        if not ok:
            log_event(args, "check_failed", iteration=iteration, reason=reason)
            if args.no_recover:
                return 2
            recovered, exitcode = recover_and_flash(args, reason)
            if not recovered:
                return exitcode
            if not (args.flash_uf2 and args.continue_after_flash):
                return 0 if args.flash_uf2 else 2
            time.sleep(args.interval_sec)
            continue

        if deadline is None:
            sleep_for = args.interval_sec
        else:
            sleep_for = min(args.interval_sec, max(0, deadline - time.monotonic()))
        if sleep_for > 0:
            time.sleep(sleep_for)

    missing = missing_requested_smokes(args)
    if missing:
        log_event(args, "soak_incomplete", iterations=iteration,
                  missing=missing)
        return 5

    log_event(args, "soak_passed", iterations=iteration)
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--_serial-worker":
        sys.exit(_serial_worker_main(sys.argv))
    sys.exit(main())
