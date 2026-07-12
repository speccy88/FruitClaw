#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=scripts/lib/common.sh
source "$SCRIPT_DIR/lib/common.sh"
# shellcheck source=scripts/lib/hardware.sh
source "$SCRIPT_DIR/lib/hardware.sh"

ROOT=$(nr_root)
PROFILE=
SERIAL=
UF2=
USE_CONTAINER=0
NO_BUILD=0
FLASH_ONLY=0
FORCE_BOARD=0
SERIAL_TIMEOUT=45
BOOTSEL_TIMEOUT=630
BACKSTOP_ARMED=0

usage() {
  cat <<'EOF'
Usage: scripts/test-profile.sh <profile-id> [options]

The test path always uses a generated --dev-bootsel image.  On every exit,
including a failed smoke test, it actively requests BOOTSEL and confirms the
result with `picotool info -a`; the 10-minute image backstop is the last resort.

Options:
  --serial PATH          Select the USB CDC serial device.
  --uf2 PATH             Use an existing *-dev-bootsel.uf2 artifact.
  --no-build             Select an existing development artifact from dist/.
  --container            Build with the pinned container.
  --flash-only           Skip NSH smoke checks after flashing.
  --serial-timeout SEC   Wait for USB CDC (default: 45).
  --bootsel-timeout SEC  Wait for development backstop (default: 630).
  --force-board          Ignore conflicting board metadata from old firmware.
EOF
}

while (($#)); do
  case "$1" in
    --serial)
      (($# >= 2)) || nr_die "--serial requires a path"
      SERIAL=$2
      shift
      ;;
    --uf2)
      (($# >= 2)) || nr_die "--uf2 requires a path"
      UF2=$2
      shift
      ;;
    --serial-timeout)
      (($# >= 2)) || nr_die "--serial-timeout requires seconds"
      SERIAL_TIMEOUT=$2
      shift
      ;;
    --bootsel-timeout)
      (($# >= 2)) || nr_die "--bootsel-timeout requires seconds"
      BOOTSEL_TIMEOUT=$2
      shift
      ;;
    --container) USE_CONTAINER=1 ;;
    --no-build) NO_BUILD=1 ;;
    --flash-only) FLASH_ONLY=1 ;;
    --force-board) FORCE_BOARD=1 ;;
    -h|--help) usage; exit 0 ;;
    --*) nr_die "unknown option: $1" ;;
    *)
      [[ -z $PROFILE ]] || nr_die "only one profile may be tested"
      PROFILE=$1
      ;;
  esac
  shift
done

[[ -n $PROFILE ]] || { usage >&2; exit 2; }
[[ $SERIAL_TIMEOUT =~ ^[1-9][0-9]*$ ]] || nr_die "invalid serial timeout"
[[ $BOOTSEL_TIMEOUT =~ ^[1-9][0-9]*$ ]] || nr_die "invalid BOOTSEL timeout"
nr_profile_field "$ROOT" "$PROFILE" id >/dev/null || nr_die "unknown profile: $PROFILE"

RETURN_SERIAL=$SERIAL
return_to_bootsel() {
  local status=$?
  trap - EXIT INT TERM
  nr_note "returning connected board to BOOTSEL"
  if ! nr_recover_bootsel "$RETURN_SERIAL" 30; then
    if ((BACKSTOP_ARMED)); then
      nr_note "active recovery failed; waiting up to ${BOOTSEL_TIMEOUT}s for the development backstop"
    fi
    if ((BACKSTOP_ARMED == 0)) || ! nr_wait_bootsel "$BOOTSEL_TIMEOUT"; then
      printf 'error: board did not return to BOOTSEL\n' >&2
      status=1
    fi
  fi
  if nr_confirm_rp2350_bootsel >/dev/null; then
    nr_note "BOOTSEL confirmed with picotool info -a"
  else
    printf 'error: BOOTSEL confirmation failed\n' >&2
    status=1
  fi
  exit "$status"
}
trap return_to_bootsel EXIT INT TERM

FLASH_ARGS=("$PROFILE" --dev-bootsel)
if [[ -n $UF2 ]]; then
  [[ $(basename -- "$UF2") == *-dev-bootsel.uf2 ]] || \
    nr_die "test-profile only accepts development BOOTSEL-recovery artifacts"
  FLASH_ARGS+=(--uf2 "$UF2")
elif ((NO_BUILD == 0)); then
  FLASH_ARGS+=(--build)
  ((USE_CONTAINER)) && FLASH_ARGS+=(--container)
fi
[[ -n $SERIAL ]] && FLASH_ARGS+=(--serial "$SERIAL")
((FORCE_BOARD)) && FLASH_ARGS+=(--force-board)
"$SCRIPT_DIR/flash-profile.sh" "${FLASH_ARGS[@]}"
BACKSTOP_ARMED=1

((FLASH_ONLY == 0)) || exit 0

# Active recovery from the previous test can leave the one-shot BOOTSEL
# scratch marker set.  A development image consumes that marker after the
# flash and returns to BOOTSEL once instead of enumerating its CDC console.
# Detect that expected state immediately and continue the same test run.

sleep 1
if nr_in_bootsel; then
  nr_note "development BOOTSEL marker consumed; rebooting the application"
  picotool reboot
fi

nr_note "waiting for the USB CDC NSH console"
START=$SECONDS
while ((SECONDS - START < SERIAL_TIMEOUT)); do
  RETURN_SERIAL=$(nr_pick_serial "$SERIAL" 2>/dev/null || true)
  [[ -n $RETURN_SERIAL ]] && break
  sleep 1
done
[[ -n $RETURN_SERIAL ]] || nr_die "USB CDC serial console did not enumerate"

EXPECTED=(berry vi i2c)
case "$PROFILE" in
  *-network|fruit-jam-full|fruit-jam-full-fruitclaw)
    EXPECTED+=(ping telnetd ftpd wget wapi renew)
    ;;
esac
case "$PROFILE" in
  fruit-jam-trmnl) EXPECTED=(trmnl) ;;
  fruit-jam-doom) EXPECTED+=(nxdoom) ;;
  fruit-jam-full) EXPECTED+=(neopixels irdump) ;;
  fruit-jam-full-fruitclaw) EXPECTED+=(fruitclaw neopixels irdump) ;;
esac

EXPECTED_CSV=$(IFS=,; printf '%s' "${EXPECTED[*]}")
nr_python - "$RETURN_SERIAL" "$EXPECTED_CSV" "$PROFILE" <<'PY'
import sys
import time

try:
    import serial
except ImportError as error:
    raise SystemExit(f"pyserial is required for hardware smoke tests: {error}")

port = sys.argv[1]
expected = [item for item in sys.argv[2].split(",") if item]
profile = sys.argv[3]

def collect(device: serial.Serial, seconds: float) -> bytes:
    deadline = time.monotonic() + seconds
    output = bytearray()
    while time.monotonic() < deadline:
        chunk = device.read(device.in_waiting or 1)
        if chunk:
            output.extend(chunk)
        else:
            time.sleep(0.02)
    return bytes(output)

def collect_until(device: serial.Serial, marker: bytes, seconds: float) -> bytes:
    deadline = time.monotonic() + seconds
    output = bytearray()
    while time.monotonic() < deadline:
        chunk = device.read(device.in_waiting or 1)
        if chunk:
            output.extend(chunk)
            if marker in output:
                break
        else:
            time.sleep(0.02)
    return bytes(output)

def collect_until_quiet(device: serial.Serial, seconds: float = 8.0,
                        quiet: float = 0.75) -> bytes:
    deadline = time.monotonic() + seconds
    quiet_deadline = time.monotonic() + quiet
    output = bytearray()
    while time.monotonic() < deadline:
        chunk = device.read(device.in_waiting or 1)
        if chunk:
            output.extend(chunk)
            quiet_deadline = time.monotonic() + quiet
        elif time.monotonic() >= quiet_deadline:
            break
        else:
            time.sleep(0.02)
    return bytes(output)

def send_paced(device: serial.Serial, data: bytes) -> None:
    for byte in data:
        written = device.write(bytes((byte,)))
        if written != 1:
            raise SystemExit("short serial write during interactive smoke test")
        device.flush()
        time.sleep(0.005)

with serial.Serial(port, 115200, timeout=0.05, write_timeout=1) as device:
    device.dtr = True
    device.reset_input_buffer()
    device.write(b"\x03\r\nhelp\r\n")
    device.flush()
    transcript = collect(device, 4.0)
    lower = transcript.lower()
    if b"nsh>" not in lower:
        sys.stderr.buffer.write(transcript)
        raise SystemExit("NSH prompt not observed")
    missing = [name for name in expected if name.encode() not in lower]
    if missing:
        sys.stderr.buffer.write(transcript)
        raise SystemExit("missing expected commands: " + ", ".join(missing))

    device.write(b"uname -a\r\nls /dev\r\necho __NSH_COMMANDS_OK__\r\n")
    device.flush()
    transcript += collect(device, 4.0)
    if b"__NSH_COMMANDS_OK__" not in transcript:
        sys.stderr.buffer.write(transcript)
        raise SystemExit("NSH smoke commands did not complete")

    # Exercise the serial echo/output race that used to corrupt the CDC TX
    # ring when several output-producing commands were entered back-to-back.
    device.write(b"ls /\r" * 20 + b"echo __NSH_BURST_OK__\r")
    device.flush()
    burst_marker = b"\r\n__NSH_BURST_OK__\r\nnsh>"
    burst = collect_until(device, burst_marker, 6.0)
    transcript += burst
    if burst_marker not in burst:
        sys.stderr.buffer.write(transcript)
        raise SystemExit("back-to-back NSH output commands stalled the CDC console")

    if "berry" in expected:
        device.write(b'berry -e "print(6*7)"\r\n')
        device.flush()
        batch = collect_until(device, b"nsh>", 3.0)
        transcript += batch
        if b"42\r\n" not in batch:
            sys.stderr.buffer.write(transcript)
            raise SystemExit("Berry batch execution failed")

        device.write(b'berry -e "print(input(\'Input: \'))"\r\n')
        device.flush()
        input_start = collect_until(device, b"Input: ", 2.0)
        transcript += input_start
        if b"Input: " not in input_start:
            sys.stderr.buffer.write(transcript)
            raise SystemExit("Berry input() did not prompt")

        send_paced(device, b"BERRY_INPUT_OK\r")
        input_result = collect_until(device, b"nsh>", 5.0)
        transcript += input_result
        if b"BERRY_INPUT_OK" not in input_result or b"nsh>" not in input_result.lower():
            sys.stderr.buffer.write(transcript)
            raise SystemExit("Berry input() did not accept CR-only input")

        device.write(b"\x1b[A")
        device.flush()
        history_probe = collect(device, 1.0)
        transcript += history_probe
        if b"BERRY_INPUT_OK" in history_probe:
            sys.stderr.buffer.write(transcript)
            raise SystemExit("Berry input leaked into shared NSH history")

        device.write(b"\x1b[B")
        device.flush()
        transcript += collect(device, 0.5)

        device.write(b"berry\r\n")
        device.flush()
        repl_start = collect_until(device, b"\r\n> ", 3.0)
        transcript += repl_start
        berry_prompts = sum(
            line.startswith(b"> ")
            for line in repl_start.replace(b"\r", b"").split(b"\n")
        )
        if b"Berry 1.1.0" not in repl_start or berry_prompts != 1:
            sys.stderr.buffer.write(transcript)
            raise SystemExit("Berry REPL did not start with exactly one prompt")

        send_paced(device, b"he\t")
        completion_probe = collect(device, 1.0)
        transcript += completion_probe
        if (b"nsh>" in completion_probe.lower() or
                b"hexdump" in completion_probe.lower()):
            sys.stderr.buffer.write(transcript)
            raise SystemExit("Berry REPL used shared NSH tab completion")

        send_paced(device, b"\x7f\x7f\x7f" b"6*7\r")
        repl_result = collect_until(device, b"\r\n> ", 3.0)
        transcript += repl_result
        if b"42\r\n" not in repl_result:
            sys.stderr.buffer.write(transcript)
            raise SystemExit("Berry REPL did not accept CR-only input")
        if profile == "fruit-jam-minimal" and b"6*7" not in repl_result:
            sys.stderr.buffer.write(transcript)
            raise SystemExit("Berry REPL input was not echoed")

        device.write(b"\x03")
        device.flush()
        interrupted = collect_until(device, b"nsh>", 2.0)
        transcript += interrupted
        if b"nsh>" not in interrupted.lower():
            sys.stderr.buffer.write(transcript)
            raise SystemExit("Ctrl+C did not return from Berry to NSH")

    if "vi" in expected:
        def send_vi(data: bytes) -> None:
            for byte in data:
                written = device.write(bytes((byte,)))
                if written != 1:
                    raise SystemExit("short serial write during vi smoke test")
                device.flush()
                time.sleep(0.03)

        def start_vi(path: bytes) -> bytes:
            device.write(b"vi " + path + b"\r")
            device.flush()
            output = collect_until(device, b"\x1b[24;66H", 8.0)
            if b"\x1b[24;66H" not in output:
                raise SystemExit("vi initial redraw did not complete")
            return output + collect_until_quiet(
                device, seconds=2.0, quiet=0.20)

        def leave_insert_mode() -> bytes:
            device.write(b"\x1b")
            device.flush()
            time.sleep(0.08)
            return collect(device, 0.10)

        def enter_insert_mode() -> bytes:
            device.write(b"i")
            device.flush()
            output = collect_until(device, b"--INSERT--", 2.0)
            if b"--INSERT--" not in output:
                raise SystemExit("vi did not enter insert mode")
            return output

        # Type before the initial redraw completes.  The terminal-size query
        # must preserve these bytes instead of consuming them as a response.
        device.write(b"vi /tmp/vi-early.txt\riEARLY")
        device.flush()
        vi_early = collect_until(device, b"\x1b[24;66H", 8.0)
        vi_early += collect_until_quiet(device, seconds=2.0, quiet=0.20)
        transcript += vi_early
        if b"EARLY" not in vi_early or b"--INSERT--" not in vi_early:
            sys.stderr.buffer.write(transcript)
            raise SystemExit("vi startup discarded early input")

        transcript += leave_insert_mode()
        send_vi(b":wq\r")
        vi_early_quit = collect_until(device, b"nsh>", 4.0)
        transcript += vi_early_quit
        if b"nsh>" not in vi_early_quit.lower():
            sys.stderr.buffer.write(transcript)
            raise SystemExit("vi could not save early input")

        device.write(b"cat /tmp/vi-early.txt\r")
        device.flush()
        vi_early_file = collect_until(device, b"nsh>", 3.0)
        transcript += vi_early_file
        if b"EARLY" not in vi_early_file:
            sys.stderr.buffer.write(transcript)
            raise SystemExit("vi did not persist early input")

        # CR-only Enter must insert a newline, save, and execute both :w and
        # :q.  This is the newline form sent by the Fruit Jam USB console.
        transcript += start_vi(b"/tmp/vi-cr.txt")
        transcript += enter_insert_mode()
        send_vi(b"CR_ONE\rCR_TWO")
        transcript += leave_insert_mode()
        send_vi(b":w\r")
        transcript += collect_until_quiet(device)
        send_vi(b":q\r")
        vi_quit = collect_until(device, b"nsh>", 4.0)
        transcript += vi_quit
        if b"nsh>" not in vi_quit.lower():
            sys.stderr.buffer.write(transcript)
            raise SystemExit("vi :q did not accept CR-only Enter")

        device.write(b"cat /tmp/vi-cr.txt\recho __VI_CR_OK__\r")
        device.flush()
        vi_cr = collect_until(device, b"__VI_CR_OK__\r\nnsh>", 4.0)
        transcript += vi_cr
        if b"CR_ONE\r\nCR_TWO" not in vi_cr:
            sys.stderr.buffer.write(transcript)
            raise SystemExit("vi :w did not preserve CR-only inserted lines")

        # Discarding a modified buffer must quit without changing the file.
        transcript += start_vi(b"/tmp/vi-cr.txt")
        transcript += enter_insert_mode()
        send_vi(b"DISCARD_ME")
        transcript += leave_insert_mode()
        send_vi(b":q!\r")
        vi_discard = collect_until(device, b"nsh>", 4.0)
        transcript += vi_discard
        if b"nsh>" not in vi_discard.lower():
            sys.stderr.buffer.write(transcript)
            raise SystemExit("vi :q! did not quit")

        device.write(b"cat /tmp/vi-cr.txt\recho __VI_DISCARD_OK__\r")
        device.flush()
        vi_discard_check = collect_until(
            device, b"__VI_DISCARD_OK__\r\nnsh>", 4.0)
        transcript += vi_discard_check
        if (b"CR_ONE\r\nCR_TWO" not in vi_discard_check or
                b"DISCARD_ME" in vi_discard_check):
            sys.stderr.buffer.write(transcript)
            raise SystemExit("vi :q! changed the saved file")

        # LF and CRLF are valid terminal Enter encodings too.  CRLF must be
        # normalized to one newline rather than inserting a blank line.
        for suffix, newline in ((b"lf", b"\n"), (b"crlf", b"\r\n")):
            path = b"/tmp/vi-" + suffix + b".txt"
            transcript += start_vi(path)
            transcript += enter_insert_mode()
            send_vi(b"ONE" + newline + b"TWO")
            transcript += leave_insert_mode()
            send_vi(b":wq" + newline)
            vi_wq = collect_until(device, b"nsh>", 4.0)
            transcript += vi_wq
            if b"nsh>" not in vi_wq.lower():
                sys.stderr.buffer.write(transcript)
                raise SystemExit(
                    f"vi :wq did not accept {suffix.decode()} Enter")

            device.write(b"cat " + path + b"\recho __VI_EOL_OK__\r")
            device.flush()
            vi_eol = collect_until(device, b"__VI_EOL_OK__\r\nnsh>", 4.0)
            transcript += vi_eol
            if b"ONE\r\nTWO" not in vi_eol or b"ONE\r\n\r\nTWO" in vi_eol:
                sys.stderr.buffer.write(transcript)
                raise SystemExit(
                    f"vi corrupted {suffix.decode()} inserted lines")

        # :wq on a new, unmodified named buffer must still create the empty
        # file.  The old editor silently skipped this write.
        transcript += start_vi(b"/tmp/vi-empty.txt")
        send_vi(b":wq\r")
        vi_empty_quit = collect_until(device, b"nsh>", 4.0)
        transcript += vi_empty_quit
        device.write(b"ls /tmp\r")
        device.flush()
        vi_empty = collect_until(device, b"nsh>", 3.0)
        transcript += vi_empty
        vi_empty_lines = {
            line.strip()
            for line in vi_empty.replace(b"\r", b"").split(b"\n")
        }
        if b"vi-empty.txt" not in vi_empty_lines:
            sys.stderr.buffer.write(transcript)
            raise SystemExit("vi did not create a new empty named file")

        # The empty-buffer command gate must allow both Z keystrokes through
        # so an existing empty file can be closed without using colon mode.
        transcript += start_vi(b"/tmp/vi-empty.txt")
        send_vi(b"ZZ")
        vi_zz_empty = collect_until(device, b"nsh>", 4.0)
        transcript += vi_zz_empty
        if b"nsh>" not in vi_zz_empty.lower():
            sys.stderr.buffer.write(transcript)
            raise SystemExit("vi ignored ZZ on an empty buffer")

        # ZZ on an unmodified read-only file is quit-only.  It must not try
        # to rewrite the file and then strand the user on a save error.
        vi_readonly_start = start_vi(b"/etc/init.d/rcS")
        transcript += vi_readonly_start
        if b"rp2350watchdog" not in vi_readonly_start:
            sys.stderr.buffer.write(transcript)
            raise SystemExit("vi did not load the read-only ZZ test file")

        send_vi(b"ZZ")
        vi_zz_unmodified = collect_until(device, b"nsh>", 4.0)
        transcript += vi_zz_unmodified
        if b"nsh>" not in vi_zz_unmodified.lower():
            sys.stderr.buffer.write(transcript)
            raise SystemExit("vi ZZ tried to rewrite an unmodified file")

        # A cursor key split across USB packets must remain one key.  Then ZZ
        # must save and quit without a trailing Enter.
        transcript += start_vi(b"/tmp/vi-arrow.txt")
        transcript += enter_insert_mode()
        send_vi(b"12")
        for byte in b"\x1b[D":
            device.write(bytes((byte,)))
            device.flush()
            time.sleep(0.005)
        send_vi(b"X")
        transcript += leave_insert_mode()
        send_vi(b"ZZ")
        vi_zz = collect_until(device, b"nsh>", 4.0)
        transcript += vi_zz
        if b"nsh>" not in vi_zz.lower():
            sys.stderr.buffer.write(transcript)
            raise SystemExit("vi ZZ did not save and quit")

        device.write(b"cat /tmp/vi-arrow.txt\recho __VI_ARROW_OK__\r")
        device.flush()
        vi_arrow = collect_until(device, b"__VI_ARROW_OK__\r\nnsh>", 4.0)
        transcript += vi_arrow
        if b"1X2" not in vi_arrow:
            sys.stderr.buffer.write(transcript)
            raise SystemExit("vi lost a fragmented cursor-key sequence")

        # Ctrl+C is an emergency exit.  Confirm the following NSH command is
        # still echoed and executed, proving terminal state was restored.
        transcript += start_vi(b"/tmp/vi-interrupt.txt")
        device.write(b"\x03")
        device.flush()
        vi_interrupt = collect_until(device, b"nsh>", 3.0)
        transcript += vi_interrupt
        device.write(b"echo __VI_POST_CTRL_C__\r")
        device.flush()
        vi_post_interrupt = collect_until(
            device, b"__VI_POST_CTRL_C__\r\nnsh>", 3.0)
        transcript += vi_post_interrupt
        if (b"echo __VI_POST_CTRL_C__" not in vi_post_interrupt or
                vi_post_interrupt.count(b"__VI_POST_CTRL_C__") < 2 or
                b"__VI_POST_CTRL_C__\r\nnsh>" not in vi_post_interrupt):
            sys.stderr.buffer.write(transcript)
            raise SystemExit("vi Ctrl+C left the terminal unusable")

    sys.stdout.buffer.write(transcript)
PY

if [[ $PROFILE == *-network || $PROFILE == fruit-jam-full* || $PROFILE == fruit-jam-trmnl ]]; then
  nr_python - "$RETURN_SERIAL" <<'PY'
import sys
import time
import serial

with serial.Serial(sys.argv[1], 115200, timeout=0.1, write_timeout=1) as device:
    device.dtr = True
    device.write(b"\r\nifconfig\r\n")
    device.flush()
    deadline = time.monotonic() + 3
    output = bytearray()
    while time.monotonic() < deadline:
        output.extend(device.read(device.in_waiting or 1))
    sys.stdout.buffer.write(output)
    if b"ifconfig" not in output.lower():
        raise SystemExit("network smoke command did not execute")
PY
fi

nr_note "profile smoke test passed: ${PROFILE}"
