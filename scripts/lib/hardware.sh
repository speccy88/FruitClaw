#!/usr/bin/env bash

# Hardware helpers shared by flash-profile.sh and test-profile.sh.

nr_bootsel_info() {
  picotool info -a 2>/dev/null
}

nr_in_bootsel() {
  nr_bootsel_info >/dev/null 2>&1
}

nr_wait_bootsel() {
  local timeout=${1:-30}
  local start=$SECONDS
  while ((SECONDS - start < timeout)); do
    if nr_in_bootsel; then
      return 0
    fi
    sleep 1
  done
  return 1
}

nr_serial_candidates() {
  nr_python <<'PY'
import glob

patterns = (
    "/dev/cu.usbmodem*",
    "/dev/tty.usbmodem*",
    "/dev/ttyACM*",
)
for pattern in patterns:
    for path in sorted(glob.glob(pattern)):
        print(path)
PY
}

nr_pick_serial() {
  local requested=${1:-}
  local candidate

  if [[ -n $requested ]]; then
    [[ -e $requested ]] || nr_die "serial device not found: ${requested}"
    printf '%s\n' "$requested"
    return
  fi

  while IFS= read -r candidate; do
    printf '%s\n' "$candidate"
    return
  done < <(nr_serial_candidates)
  return 1
}

nr_serial_bootsel_command() {
  local port=$1
  nr_python - "$port" <<'PY'
import sys
import time

try:
    import serial
except ImportError as error:
    raise SystemExit(f"pyserial unavailable: {error}")

with serial.Serial(sys.argv[1], 115200, timeout=0.25, write_timeout=1) as device:
    device.dtr = True
    device.write(b"\r\nbootsel\r\n")
    device.flush()
    time.sleep(0.2)
PY
}

nr_serial_touch_1200() {
  local port=$1
  nr_python - "$port" <<'PY'
import sys
import time

try:
    import serial
except ImportError as error:
    raise SystemExit(f"pyserial unavailable: {error}")

device = serial.Serial(sys.argv[1], 1200, timeout=0.1, write_timeout=1)
device.dtr = True
time.sleep(0.1)
device.dtr = False
device.close()
PY
}

nr_recover_bootsel() {
  local requested_port=${1:-}
  local timeout=${2:-30}
  local port=

  nr_require_tool picotool
  if nr_in_bootsel; then
    return 0
  fi

  nr_note "requesting BOOTSEL through picotool"
  picotool reboot -f -u >/dev/null 2>&1 || true
  nr_wait_bootsel 5 && return 0

  port=$(nr_pick_serial "$requested_port" 2>/dev/null || true)
  if [[ -n $port ]]; then
    nr_note "requesting BOOTSEL through NSH on ${port}"
    nr_serial_bootsel_command "$port" >/dev/null 2>&1 || true
    nr_wait_bootsel 5 && return 0

    nr_note "requesting BOOTSEL with 1200-baud CDC touch on ${port}"
    nr_serial_touch_1200 "$port" >/dev/null 2>&1 || true
    nr_wait_bootsel "$timeout" && return 0
  fi

  return 1
}

nr_confirm_rp2350_bootsel() {
  local info
  info=$(nr_bootsel_info) || return 1
  grep -E -q 'RP2350|rp2350' <<< "$info" || {
    printf '%s\n' "$info" >&2
    nr_die "connected BOOTSEL device is not identified as an RP2350"
  }
  printf '%s\n' "$info"
}
