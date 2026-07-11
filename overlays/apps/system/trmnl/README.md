# TRMNL Display Client

`trmnl` is a small native NuttX display client for the Adafruit Fruit Jam
RP2350 TRMNL profile. It reads runtime credentials from `device.json`, calls the
TRMNL `/api/display` endpoint, downloads the returned PNG, decodes it, and
renders it to `/dev/fb0` when the framebuffer is `800x480` `FB_FMT_Y2`.

Runtime files live under `/mnt/sd0/trmnl` when available, then
`/flash/trmnl` when a board flash filesystem is mounted, with `/data/trmnl` as
the volatile fallback. Reusable builds should keep credentials in runtime
storage instead of compiled config.

Example `device.json`:

```json
{
  "TrmnlId": "AA:BB:CC:DD:EE:FF",
  "TrmnlToken": "runtime-token",
  "TrmnlApiUrl": "https://usetrmnl.com/api",
  "ImageFormat": "png"
}
```

For one-off bench images, `CONFIG_SYSTEM_TRMNL_DEFAULT_ID`,
`CONFIG_SYSTEM_TRMNL_DEFAULT_TOKEN`, `CONFIG_SYSTEM_TRMNL_WIFI_DEFAULT_SSID`,
and `CONFIG_SYSTEM_TRMNL_WIFI_DEFAULT_PSK` can be set in the local `.config`.
Those compiled fallbacks are only used when SD/runtime files are absent and
should stay empty in reusable community defconfigs.

Commands:

- `trmnl config`
- `trmnl status`
- `trmnl storage`
- `trmnl set-device <id> <token> [api-url] [image-format]`
- `trmnl set-id <id>`
- `trmnl set-token <token>`
- `trmnl set-api <api-url>`
- `trmnl set-image-format png`
- `trmnl set-wifi <ssid> <password> [key_mgmt] [cipher]`
- `trmnl test-pattern`
- `trmnl once`
- `trmnl start`
- `trmnl boot`
- `trmnl clear-cache`
- `trmnl selftest`

`trmnl boot` is intended for the Fruit Jam `trmnl` board profile. It starts
ESP-Hosted Wi-Fi, configures `wlan0` from `wifi.conf`, renews DHCP, and then
enters the refresh loop. Boot-time Wi-Fi setup is retried briefly to absorb
early rcS/ESP-hosted races, and each finite fetch/render pass is guarded by the
Fruit Jam bootguard. The Wi-Fi config supports:

```text
ssid=your-ssid
password=your-password
key_mgmt=3
cipher=2
```

Provisioning from NSH:

```sh
trmnl storage
trmnl set-device AA:BB:CC:DD:EE:FF your-token https://usetrmnl.com/api png
trmnl set-wifi your-ssid your-password
trmnl config
```

`trmnl config` and `trmnl status` do not print the token or Wi-Fi password.
If neither SD nor `/flash` is mounted, provisioning writes to `/data/trmnl` and
prints a warning because that tmpfs-backed fallback does not survive reset.

The TRMNL app already treats `/flash/trmnl` as the preferred on-board fallback,
but the current Fruit Jam RP2350 board layer still needs a real flash MTD
partition and mount at `/flash` before that path is reset-persistent.
