# Multiverse submission artifacts

Chrono Trigger needs a paid Square Enix APK with no redistribution permission, so it
ships through PortMaster **Multiverse**, not the main `PortMaster-New` repo.

> **Target repo: `PortsMaster-MV/PortMaster-MV-New`.**
> The older `PortsMaster-MV/PortMaster-Multiverse` is **archived** (last push 2024-01-17)
> and cannot accept a PR. MV-New uses the standard `ports/<name>/` source layout and
> carries the full PortMaster-New toolchain (`tools/build_release.py`, `build_data.py`,
> `prepare_repo.sh`) — it is **not** the root-level-zip layout the archived repo used.

## Layout

The submission is a `ports/ct/` **source directory**, not a hand-assembled zip. CI builds
the zip from it.

| MV-New repo path | from here | notes |
| --- | --- | --- |
| `ports/ct/port.json` | `portmaster/pkg/port.json` | **`name` must be `"ct.zip"`** — see below |
| `ports/ct/Chrono Trigger.sh` | `portmaster/pkg/Chrono Trigger.sh` | drop the `# PORTMASTER:` header line (upstream ports carry none) |
| `ports/ct/gameinfo.xml` | `portmaster/pkg/gameinfo.xml` | required; its `<image>` must match the screenshot filename |
| `ports/ct/README.md` | `ct.md` | the asset-extraction guide shown to users |
| `ports/ct/screenshot.png` | **TODO — the one hard gate** | see below |
| `ports/ct/ct/...` | the built `ct`, `font.ttf`, `libs.aarch64/`, `licenses/` | gitignored here, so they must be committed in the fork |

## Rules worth knowing before you package

- **`port.json` `name` must be `"ct.zip"`.** `build_release.py` forces
  `name = <port_dir>.zip` for any port added after 2024-01-26 and marks the port *broken*
  otherwise, which drops it from the build and fails PR CI. Our committed `port.json` says
  `"ct_pm.zip"` because that is the self-host zip name — change it in the fork, not here.
  Keep the port directory and gamedir as `ct`.
- **The screenshot ships INSIDE the zip** at `ports/ct/screenshot.{png,jpg}` and is a
  `REQUIRED_FILES` error if missing. This reverses the archived repo's convention, where
  the screenshot lived at repo level and `port.json` used `image: null` — do not follow
  the old advice. Save it as `.png` to match `gameinfo.xml`'s `<image>` path.
- `build_release.py` performs **no dimension or aspect-ratio check**. The 4:3 ≥640×480
  gameplay convention is human review preference, not a tool gate.
- `package.sh`'s whitelist staging is what structurally prevents a commercial-content
  leak, and it has **no equivalent** on the repo-layout path. Run `git status` in the fork
  before opening the PR and confirm no game data is staged.

## Pre-submission checklist

1. Confirm the current inclusion process with the PortMaster crew on Discord. MV-New has
   no `CONTRIBUTING` file and a 168-byte README, so the human gate is still the only
   documented one. Ask specifically whether MV-New accepts direct PRs.
2. **Capture the gameplay screenshot** (the one hard blocker). It is not device-locked:
   `screen_width`/`screen_height` in `config.txt` plus `CT_WINDOWED=1` will render a real
   640×480 adaptive-camera frame on any device, and `CT_CAPTURE=1` dumps it. It does need
   a short play session to reach a field or battle frame — capture fires at frame 60 then
   every 300, with no on-demand hotkey.
3. Multi-CFW smoke test (AmberELEC / ArkOS / ROCKNIX / muOS). Note that every device we
   own runs Knulli, so exactly one CFW has ever been exercised; a `#testing-n-dev` thread
   is the realistic way to cover the rest. The untested surface is `$DEVICE_ARCH` from an
   older `control.txt`, the host-provided SDL2/freetype2/libGLESv2/libEGL, and the
   zram/cpufreq block in the launcher — *not* glibc (the builder is pinned at 2.31
   precisely because it is the lowest common denominator).
4. Fork, disable Actions on the fork, run `prepare_repo.sh` to prime `releases/`, add the
   port directory, then run `build_release.py --do-check` until clean. Skipping the priming
   step mis-dates the port and can mask the strict-name check.
