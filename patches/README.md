# Patch stacks

`nuttx/series` and `apps/series` define the only valid patch order.  Each
patch changes files already owned by the corresponding Apache repository.
Project-owned new files belong under `../overlays/`, not in patches.

The stacks are based on the exact commits in `sources.lock.json` and the two
submodule gitlinks.  Builds apply them to disposable staging trees; never
apply them to the checked-out submodules.

Excluded on purpose:

- Apache `.github` workflow changes
- downloaded or unpacked Berry source and archives
- generated NuttX/App build products
- generated Fruit Jam examples ROMFS C data
- redundant NXDoom documentation already supplied upstream

Use `scripts/update-upstreams.sh` for a pin change.  It must validate every
patch and all eight profiles before updating either gitlink or the lock file.
