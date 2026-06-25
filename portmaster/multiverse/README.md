# Multiverse submission artifacts

Chrono Trigger needs a paid Square Enix APK with no redistribution permission, so it
ships through the **PortMaster Multiverse** repo (`PortsMaster-MV/PortMaster-Multiverse`),
not the main `PortMaster-New` repo.

The MV repo does NOT use the `ports/<name>/` source layout. It stores, at the repo root:

| MV repo path | from here | how to produce |
| --- | --- | --- |
| `ct.zip` | — | `portmaster/package.sh` (built PortMaster zip) |
| `markdown/ct.md` | `ct.md` | the asset-extraction guide shown to users |
| `images/ct.screenshot.jpg` | **TODO** | a 4:3 ≥640×480 **gameplay** JPG — *not captured yet*; grab it from a 4:3 device (RG40XXV / RG35XX SP) during tomorrow's testing |

Notes confirmed against real MV ports (e.g. `smworld.zip`):
- `port.json` is `version 2`, `name "ct.zip"`, `rtr false`, `runtime null`. MV ports use
  `image: null` (the screenshot is the repo-level `images/*.screenshot.jpg`, **not** inside
  the zip), and there is **no `gameinfo.xml` inside the zip** — PortMaster generates the
  EmulationStation metadata itself. (`gameinfo.xml` here is kept only for completeness /
  a possible main-repo cross-list; it is not part of the MV submission.)
- The MV repo automates releases via GitHub Actions (`SOURCE_SETUP.txt` sets
  `REPO_PREFIX=pmmv`; `tools/`). The official-inclusion process is marked TBD in the MV
  repo README — confirm the current submission steps with the PortMaster crew on Discord
  before opening the PR.

Pre-submission checklist (still open): re-capture the screenshot on a 4:3 device if you
want the true 4:3 look; multi-CFW testing (AmberELEC/ArkOS/ROCKNIX/muOS) at 640×480 + a
`#testing-n-dev` Discord thread.
