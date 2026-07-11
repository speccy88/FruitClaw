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
nr_python - "$RETURN_SERIAL" "$EXPECTED_CSV" <<'PY'
import sys
import time

try:
    import serial
except ImportError as error:
    raise SystemExit(f"pyserial is required for hardware smoke tests: {error}")

port = sys.argv[1]
expected = [item for item in sys.argv[2].split(",") if item]

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

    device.write(b"uname -a\r\nls /dev\r\n")
    device.write(b"sleep 5\r\n")
    device.flush()
    time.sleep(0.3)
    device.write(b"\x03\r\necho __CTRL_C_OK__\r\n")
    device.flush()
    transcript += collect(device, 4.0)
    if b"__CTRL_C_OK__" not in transcript:
        sys.stderr.buffer.write(transcript)
        raise SystemExit("Ctrl+C did not return control to NSH")
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
