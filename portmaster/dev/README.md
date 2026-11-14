# portmaster/dev

Bring-up and playtest helpers. None of these are built by `portmaster/build.sh`,
packaged by `portmaster/package.sh`, or compiled in CI.

| File | What it is |
| --- | --- |
| `harness_load.c` | Maps and relocates the two game `.so` files and reports unresolved imports. Runs no guest code. |
| `harness_stubs.c` | Placeholder app-layer functions so `harness_load.c` links against the full import table. |
| `movie_probe.c` | Headless FMV decode test against the bundled FFmpeg; needs a real `.dat`. |
| `ctmem_watch.sh` | Samples the game's memory and swap on the device during a playtest and watches for an OOM kill. |
| `ctleak_watch.sh` | Tracks RSS growth, map, fd and thread counts to localise a leak. |

Each file's header comment has its build or run line. The harnesses and the
probe need your own game files, so they only run on a device or in the builder
image with those files mounted.
