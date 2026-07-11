# NuttX RP2350

NuttX RP2350 is a thin, reproducible integration repository for two RP2350
boards:

- Adafruit Fruit Jam (`RP2350B`, PSRAM, ESP32-C6 ESP-Hosted networking)
- Raspberry Pi Pico 2 W (`RP2350A`, no PSRAM, Infineon CYW43439 networking)

The repository does not vendor Apache NuttX source trees.  `nuttx/` and
`apps/` are immutable submodule pins to `apache/nuttx` and
`apache/nuttx-apps`.  Project changes live in ordered patch stacks,
project-owned overlays, and eight independent profile defconfigs.

FruitClaw remains the name of the optional operator application in the
`fruit-jam-full-fruitclaw` profile; it is no longer the project name.

## Repository model

```text
nuttx/ and apps/              clean pinned Apache submodules
patches/{nuttx,apps}/series   ordered modifications to upstream files
overlays/{nuttx,apps}/        project-owned boards, drivers, and apps
profiles/manifest.json        canonical eight-profile release matrix
profiles/<id>/defconfig       complete independent NuttX configurations
sources.lock.json             source, dependency, toolchain, and image pins
build/                        ignored per-profile staging trees
dist/                         ignored build and release output
```

Builds never configure or patch the checked-out submodules.  Each profile is
materialized from the exact locked commits into `build/work/<profile>/`, then
patched, overlaid, configured, and built there.  A normal build therefore
leaves `git status` clean.

## Release profiles

| Profile | Board | Purpose |
| --- | --- | --- |
| `fruit-jam-minimal` | Fruit Jam | USB CDC NSH, Berry, vi, readline/history, Ctrl+C, I2C, watchdog; no network or PSRAM |
| `pico-2-w-minimal` | Pico 2 W | Same minimum contract on RP2350A |
| `fruit-jam-network` | Fruit Jam | Minimum plus ESP32-C6 ESP-Hosted `wlan0` and low-footprint network services |
| `pico-2-w-network` | Pico 2 W | Minimum plus Infineon CYW43439 `wlan0` and the same services |
| `fruit-jam-trmnl` | Fruit Jam | Working TRMNL client, ESP-Hosted, PSRAM, and 800x480 Y2 DVI output |
| `fruit-jam-doom` | Fruit Jam | Focused NXDoom image with PSRAM, DVI, SD, USB keyboard/mouse, and I2S audio |
| `fruit-jam-full` | Fruit Jam | Network plus the complete Fruit Jam driver and utility surface |
| `fruit-jam-full-fruitclaw` | Fruit Jam | Full image plus FruitClaw boot autostart |

Network profiles include DHCP, DNS, `ifconfig`, `wapi`, `renew`, ping, wget,
NTP, netcat, telnet client/server, FTP client/server, and lightweight HTTPD.
Wi-Fi credentials are runtime data and must never be stored in defconfigs.

The Pico 2 W USB console baseline has been exercised on hardware.  Pico 2 W
Wi-Fi/services and NXDoom mouse/audio remain explicit hardware-validation
milestones; their presence in the build matrix is not a claim that those live
tests are complete.

## Build

Prerequisites are Git, Python 3, an Arm GNU toolchain, NuttX
`kconfig-frontends`, and `genromfs`.  Docker can provide the canonical release
environment.

```sh
git clone --recurse-submodules \
  https://github.com/speccy88/NuttX-RP2350.git
cd NuttX-RP2350
./scripts/bootstrap.sh
./scripts/build-profile.sh fruit-jam-minimal
```

Build every release profile:

```sh
./scripts/build-all.sh
```

The profile manifest is the sole release matrix.  Do not duplicate profile
lists in scripts or workflows.

## Flash and connected testing

Release images retain intentional recovery commands:

- NSH `bootsel`
- 1200-baud USB CDC BOOTSEL touch
- host-driven `picotool reboot -u -f`

Automatic watchdog-to-BOOTSEL recovery is forbidden in canonical release
defconfigs.  Release watchdog expiry performs a normal reset back into NuttX.

```sh
./scripts/flash-profile.sh fruit-jam-minimal
./scripts/test-profile.sh fruit-jam-minimal
```

Connected test runs use a generated development-only override:

```sh
./scripts/build-profile.sh fruit-jam-minimal --dev-bootsel
```

The override enables BOOTSEL recovery and a ten-minute BOOTSEL backstop only
inside the ignored staging tree.  Test tooling always attempts to return the
board to BOOTSEL before exiting and confirms that state with
`picotool info -a`.

## Releases

Release tags use SemVer.  Until the hardware matrix is complete, publish only
prereleases such as `v0.1.0-rc.1`.

Pushing a tag starts `.github/workflows/release.yml`; it performs two clean
digest-pinned container builds, compares each UF2 byte for byte, packages the
complete matrix, and creates the prerelease only if every asset is present.

```sh
git tag v0.1.0-rc.1
git push origin v0.1.0-rc.1
```

Every release is atomic: eight RP2350 UF2s, the matching ESP32-C6
ESP-Hosted-MCU binary, `SHA256SUMS.txt`, and `build-manifest.json`.  A missing
profile fails the release; assets under an existing tag are never replaced.

The manifest records both Apache pins, wrapper/tag SHA, profile and patch
hashes, toolchain and container identity, ESP-Hosted-MCU pin, sizes, and
SHA-256 values.

## Updating Apache upstreams

Pins never float during builds.  Test candidate commits explicitly:

```sh
./scripts/update-upstreams.sh \
  --nuttx f27749b1b7339676bde10ec6852c9a7082fbaee3 \
  --apps c785d40af7b354a5ee312ad41e174e07875866da
```

The update is accepted only if both ordered patch stacks apply, all profile
policies pass, and all eight isolated builds succeed.  Gitlinks and
`sources.lock.json` must move together in one reviewed change.

## Project documentation

- `docs/FRUITCLAW_WIFI_RELIABILITY.md` records FruitClaw/ESP-Hosted reliability work.
- `docs/FRUITCLAW_ISSUES.md` records known FruitClaw component issues.
- Board-specific documentation is installed through `overlays/nuttx/`.
- Historical FruitClaw tags and releases remain available after the repository rename.

## License

Project patches and overlays follow the license headers of their respective
Apache NuttX, Apache NuttX Apps, Berry, NXDoom, and component sources.  See
the source files and upstream projects for details.
