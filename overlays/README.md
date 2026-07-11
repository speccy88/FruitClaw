# Project overlays

These trees contain new NuttX RP2350-owned files which do not exist at the
locked Apache commits: Fruit Jam and Pico 2 W board support, RP2350 drivers,
hardware utilities, TRMNL, FruitClaw, and related documentation/assets.

Materialization copies `nuttx/` and `apps/` into their matching staged source
trees after the ordered upstream patches have applied.  Profile defconfigs
remain under `profiles/` so each release configuration is independently
reviewable.

Do not store generated objects, downloaded dependencies, credentials, UF2s,
or release binaries here.
