#!/usr/bin/env python3
"""Validate the independent RP2350 profile manifest and release defconfigs."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


EXPECTED_PROFILES = (
    "fruit-jam-minimal",
    "pico-2-w-minimal",
    "fruit-jam-network",
    "pico-2-w-network",
    "fruit-jam-trmnl",
    "fruit-jam-doom",
    "fruit-jam-full",
    "fruit-jam-full-fruitclaw",
)

MINIMUM_CONTRACT = {
    "fruit-jam-minimal",
    "pico-2-w-minimal",
    "fruit-jam-network",
    "pico-2-w-network",
    "fruit-jam-doom",
    "fruit-jam-full",
    "fruit-jam-full-fruitclaw",
}

NETWORK_UTILITIES_CONTRACT = {
    "fruit-jam-network",
    "pico-2-w-network",
    "fruit-jam-full",
    "fruit-jam-full-fruitclaw",
}

COMMON_REQUIRED = (
    "USBDEV",
    "CDCACM",
    "CDCACM_CONSOLE",
    "NSH_READLINE",
    "READLINE_TABCOMPLETION",
    "READLINE_CMD_HISTORY",
    "INTERPRETERS_BERRY",
    "SYSTEM_VI",
    "SIG_DEFAULT",
    "SYSTEM_I2CTOOL",
    "RP23XX_I2C",
    "WATCHDOG",
)

NETWORK_REQUIRED = (
    "NET",
    "NET_IPv4",
    "NET_TCP",
    "NET_UDP",
    "NETDB_DNSCLIENT",
    "NETUTILS_DHCPC",
    "NETUTILS_FTPC",
    "NETUTILS_FTPD",
    "NETUTILS_NETCAT",
    "NETUTILS_NTPCLIENT",
    "NETUTILS_TELNETD",
    "NETUTILS_WEBCLIENT",
    "NETUTILS_WEBSERVER",
    "SYSTEM_PING",
    "SYSTEM_TELNETD",
    "WIRELESS_WAPI",
)

AUTOMATIC_BOOTSEL_SYMBOLS = (
    "RP23XX_AUTO_BOOTSEL_ON_BOOT",
    "RP23XX_PICO_BOOTSEL_ON_WATCHDOG",
    "RP23XX_PICO_AUTO_BOOTSEL",
    "SYSTEM_RP2350WATCHDOG_BOOTSEL_RECOVERY",
    "ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD",
    "FRUITCLAW_GUARD_BOOTSEL_RECOVERY",
    "SYSTEM_BOOTGUARD",
    "SYSTEM_TRMNL_BOOT_GUARD",
)

CREDENTIAL_RE = re.compile(
    r"(?:SSID|PASSPHRASE|PASSWORD|PASSWD|(?:^|_)PSK(?:_|$)|WIFI.*KEY|WAPI.*KEY)",
    re.IGNORECASE,
)

SEMVER_TAG_RE = re.compile(
    r"^v(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)"
    r"(?:-(?P<prerelease>[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$"
)


def parse_config(path: Path) -> dict[str, str | None]:
    values: dict[str, str | None] = {}
    assignment = re.compile(r"^CONFIG_([A-Za-z0-9_]+)=(.*)$")
    disabled = re.compile(r"^# CONFIG_([A-Za-z0-9_]+) is not set$")
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        match = assignment.match(line)
        if match:
            values[match.group(1)] = match.group(2)
            continue
        match = disabled.match(line)
        if match:
            values[match.group(1)] = None
    return values


def enabled(config: dict[str, str | None], symbol: str) -> bool:
    return config.get(symbol) == "y"


def add_error(errors: list[str], profile: str, message: str) -> None:
    errors.append(f"{profile}: {message}")


def check_release_policy(
    profile: str, config: dict[str, str | None], errors: list[str]
) -> None:
    if not enabled(config, "WATCHDOG"):
        add_error(errors, profile, "release profiles must enable CONFIG_WATCHDOG")

    for symbol in ("FS_ROMFS", "ETC_ROMFS"):
        if not enabled(config, symbol):
            add_error(
                errors,
                profile,
                f"release profile requires CONFIG_{symbol}=y for board rcS startup",
            )

    for symbol in AUTOMATIC_BOOTSEL_SYMBOLS:
        if enabled(config, symbol):
            add_error(
                errors,
                profile,
                f"release policy forbids automatic BOOTSEL via CONFIG_{symbol}",
            )

    max_uptime = config.get("FRUITCLAW_MAX_UPTIME_GUARD_MS")
    if max_uptime not in (None, "0"):
        add_error(
            errors,
            profile,
            "CONFIG_FRUITCLAW_MAX_UPTIME_GUARD_MS must be 0 in release images",
        )

    dev_bootsel_ms = config.get("SYSTEM_RP2350WATCHDOG_DEV_BOOTSEL_MS")
    if dev_bootsel_ms not in (None, "0"):
        add_error(
            errors,
            profile,
            "CONFIG_SYSTEM_RP2350WATCHDOG_DEV_BOOTSEL_MS must be 0 in release images",
        )

    if enabled(config, "SYSTEM_RP2350WATCHDOG"):
        try:
            timeout_ms = int(config.get("SYSTEM_RP2350WATCHDOG_TIMEOUT_MS") or "0")
            ping_ms = int(config.get("SYSTEM_RP2350WATCHDOG_PING_MS") or "0")
        except ValueError:
            add_error(errors, profile, "watchdog timeout/feed values must be integers")
        else:
            if not 1000 <= timeout_ms <= 16000:
                add_error(
                    errors,
                    profile,
                    "RP23xx watchdog timeout must be between 1000 and 16000 ms",
                )
            if not 100 <= ping_ms < timeout_ms:
                add_error(
                    errors,
                    profile,
                    "watchdog feed period must be positive and below its timeout",
                )


def valid_semver_tag(tag: str) -> bool:
    match = SEMVER_TAG_RE.fullmatch(tag)
    if not match:
        return False
    prerelease = match.group("prerelease")
    if prerelease is None:
        return True
    return all(
        not (identifier.isdigit() and len(identifier) > 1 and identifier[0] == "0")
        for identifier in prerelease.split(".")
    )


def check_feature_contract(
    profile: str, config: dict[str, str | None], errors: list[str]
) -> None:
    if profile == "fruit-jam-full-fruitclaw":
        if enabled(config, "SYSTEM_RP2350WATCHDOG"):
            add_error(
                errors,
                profile,
                "FruitClaw owns watchdog feeding; generic RP2350 watchdog must be disabled",
            )
    elif not enabled(config, "SYSTEM_RP2350WATCHDOG"):
        add_error(
            errors,
            profile,
            "release contract requires CONFIG_SYSTEM_RP2350WATCHDOG=y",
        )

    if profile in MINIMUM_CONTRACT:
        for symbol in COMMON_REQUIRED:
            if not enabled(config, symbol):
                add_error(errors, profile, f"minimum contract requires CONFIG_{symbol}=y")

    if profile in NETWORK_UTILITIES_CONTRACT:
        for symbol in NETWORK_REQUIRED:
            if not enabled(config, symbol):
                add_error(errors, profile, f"network contract requires CONFIG_{symbol}=y")
        for symbol, value in config.items():
            if "MQTT" in symbol and value == "y":
                add_error(errors, profile, f"low-footprint network profile enables CONFIG_{symbol}")

    if profile == "fruit-jam-network" and not enabled(config, "ESP_HOSTED"):
        add_error(errors, profile, "requires CONFIG_ESP_HOSTED=y")
    if profile == "pico-2-w-network" and not enabled(
        config, "RP23XX_INFINEON_CYW43439"
    ):
        add_error(errors, profile, "requires CONFIG_RP23XX_INFINEON_CYW43439=y")
    if profile == "pico-2-w-minimal" and enabled(
        config, "RP23XX_INFINEON_CYW43439"
    ):
        add_error(errors, profile, "minimal profile must leave CYW43439 inactive")

    if profile.endswith("-minimal"):
        for symbol in ("NET", "ESP_HOSTED", "RP23XX_PSRAM", "USBHOST"):
            if enabled(config, symbol):
                add_error(errors, profile, f"minimal profile must not enable CONFIG_{symbol}")

    if profile == "fruit-jam-doom":
        if not any(enabled(config, symbol) for symbol in ("GAMES_NXDOOM", "NXDOOM")):
            add_error(errors, profile, "Doom profile must enable NXDoom")
        if enabled(config, "NET"):
            add_error(errors, profile, "focused Doom profile must not enable networking")

    if profile == "fruit-jam-trmnl" and not any(
        enabled(config, symbol) for symbol in ("SYSTEM_TRMNL", "EXAMPLES_TRMNL")
    ):
        add_error(errors, profile, "TRMNL profile must enable the TRMNL client")

    if profile == "fruit-jam-full-fruitclaw" and not enabled(
        config, "SYSTEM_FRUITCLAW"
    ):
        add_error(errors, profile, "FruitClaw profile must enable CONFIG_SYSTEM_FRUITCLAW")
    if profile == "fruit-jam-full" and enabled(config, "SYSTEM_FRUITCLAW"):
        add_error(errors, profile, "full profile must not include FruitClaw")


def check_credentials(
    profile: str, config: dict[str, str | None], errors: list[str]
) -> None:
    for symbol, value in config.items():
        if (
            not CREDENTIAL_RE.search(symbol)
            or value in (None, "", '""', "0")
            or not (value.startswith('"') and value.endswith('"'))
        ):
            continue
        add_error(
            errors,
            profile,
            f"possible hardcoded credential in CONFIG_{symbol}; configure it at runtime",
        )


def validate_one_config(profile: str, path: Path, errors: list[str]) -> None:
    if not path.is_file():
        add_error(errors, profile, f"defconfig not found: {path}")
        return
    config = parse_config(path)
    check_release_policy(profile, config, errors)
    check_feature_contract(profile, config, errors)
    check_credentials(profile, config, errors)


def load_manifest(root: Path) -> dict:
    path = root / "profiles" / "manifest.json"
    try:
        with path.open("r", encoding="utf-8") as stream:
            return json.load(stream)
    except (OSError, json.JSONDecodeError) as error:
        raise SystemExit(f"error: cannot load {path}: {error}") from error


def validate_manifest(root: Path, manifest: dict, errors: list[str]) -> None:
    if manifest.get("schema_version") != 1:
        errors.append("manifest: schema_version must be 1")

    profiles = manifest.get("profiles")
    if not isinstance(profiles, list):
        errors.append("manifest: profiles must be an array")
        return

    ids = [profile.get("id") for profile in profiles if isinstance(profile, dict)]
    if len(ids) != len(set(ids)):
        errors.append("manifest: profile ids must be unique")
    if set(ids) != set(EXPECTED_PROFILES):
        missing = sorted(set(EXPECTED_PROFILES) - set(ids))
        extra = sorted(set(ids) - set(EXPECTED_PROFILES))
        errors.append(f"manifest: profile set mismatch; missing={missing}, extra={extra}")

    atomic = manifest.get("release", {}).get("atomic_profiles")
    if atomic != list(EXPECTED_PROFILES):
        errors.append(
            "manifest: release.atomic_profiles must list the eight profiles in canonical order"
        )

    for profile in profiles:
        if not isinstance(profile, dict) or not isinstance(profile.get("id"), str):
            errors.append("manifest: every profile must be an object with a string id")
            continue
        profile_id = profile["id"]
        expected_defconfig = f"profiles/{profile_id}/defconfig"
        if profile.get("defconfig") != expected_defconfig:
            add_error(
                errors,
                profile_id,
                f"defconfig must be the independent canonical file {expected_defconfig}",
            )
        if profile.get("artifact_slug") != profile_id:
            add_error(errors, profile_id, "artifact_slug must equal the stable profile id")
        if profile.get("board_config") != profile_id:
            add_error(errors, profile_id, "board_config must equal the stable profile id")

        destination = profile.get("defconfig_destination")
        if (
            not isinstance(destination, str)
            or destination.startswith("/")
            or ".." in Path(destination).parts
            or not destination.endswith(f"/configs/{profile_id}/defconfig")
        ):
            add_error(errors, profile_id, "unsafe or inconsistent defconfig_destination")

        networking = profile.get("networking")
        if networking not in {"none", "esp-hosted-mcu", "cyw43439"}:
            add_error(errors, profile_id, f"invalid networking value: {networking!r}")
        expected_esp = networking == "esp-hosted-mcu"
        if profile.get("esp32c6_dependency") is not expected_esp:
            add_error(
                errors,
                profile_id,
                "esp32c6_dependency must exactly match esp-hosted-mcu networking",
            )

        defconfig = root / expected_defconfig
        validate_one_config(profile_id, defconfig, errors)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root", type=Path, default=Path(__file__).resolve().parents[1]
    )
    parser.add_argument(
        "--list", action="store_true", help="print profile ids as a JSON array"
    )
    parser.add_argument(
        "--resolved-config",
        nargs=2,
        metavar=("PROFILE", "PATH"),
        help="validate one post-olddefconfig release configuration",
    )
    parser.add_argument(
        "--release-tag",
        help="enforce prerelease/stable hardware-validation gating for a tag",
    )
    args = parser.parse_args()

    root = args.root.resolve()
    if args.resolved_config:
        profile, config_path = args.resolved_config
        errors: list[str] = []
        if profile not in EXPECTED_PROFILES:
            errors.append(f"unknown profile: {profile}")
        validate_one_config(profile, Path(config_path), errors)
        if errors:
            for error in errors:
                print(f"error: {error}", file=sys.stderr)
            return 1
        print(f"release policy passed: {profile}")
        return 0

    manifest = load_manifest(root)
    if args.release_tag:
        tag = args.release_tag
        if not valid_semver_tag(tag):
            print(f"error: invalid SemVer release tag: {tag}", file=sys.stderr)
            return 1
        if "-" not in tag:
            incomplete = []
            for profile in manifest.get("profiles", []):
                validation = profile.get("validation", {})
                if (
                    validation.get("build") != "validated"
                    or validation.get("hardware") != "validated"
                ):
                    incomplete.append(profile.get("id", "unknown"))
            if incomplete:
                print(
                    "error: stable release blocked until build and hardware validation "
                    "are marked validated for: " + ", ".join(incomplete),
                    file=sys.stderr,
                )
                return 1
    if args.list:
        print(json.dumps([profile["id"] for profile in manifest.get("profiles", [])]))
        return 0

    errors = []
    validate_manifest(root, manifest, errors)
    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1

    print(f"validated {len(EXPECTED_PROFILES)} independent release profiles")
    return 0


if __name__ == "__main__":
    sys.exit(main())
