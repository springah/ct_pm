# Chrono Trigger

A native aarch64 port of the **Android** version of Chrono Trigger, run through the
`ct_nx` loader (a sibling of the Switch port). cocos2d-x engine on SDL2 + GLES2, with
the cutscenes (FMV) played through a bundled minimal FFmpeg.

**No game data is included.** You must supply the files yourself from a legally-owned
copy of the **Chrono Trigger Android app, version 2.1.5**.

## Extract Assets

You need three things out of your own Chrono Trigger **v2.1.5** Android APK:
`libchrono.so`, `libc++_shared.so`, and the whole `assets/` folder.

1. Get the **Chrono Trigger v2.1.5** APK from your own purchased copy — e.g. back it
   up from a device you've installed it on (any "APK extractor" app), or from your
   Google Play library. You'll end up with a file like `chrono-trigger.apk`.
2. An APK is just a ZIP archive. Open it with any archive tool (rename it to
   `.zip` first if your tool needs that).
3. From inside the extracted APK, copy these into your `ports/ct/` folder:
   - `lib/arm64-v8a/libchrono.so`     →  `ports/ct/libchrono.so`
   - `lib/arm64-v8a/libc++_shared.so` →  `ports/ct/libc++_shared.so`
   - the entire `assets/` folder       →  `ports/ct/assets/`
4. Launch **Chrono Trigger** from the **Ports** menu. On first run it checks for
   these files and tells you on-screen if any are still missing.

> ⚠️ The version must be **v2.1.5** — other versions have a different `libchrono.so`
> and will not boot correctly.

## Controls

| Button | Action |
| --- | --- |
| D-Pad / Left Stick | Move • menu navigation |
| A | Confirm • talk • examine |
| B | Cancel • hold to dash |
| X | Open Menu |
| Y | Character switch • Time Gauge |
| Start | Pause |
| L / R | Page • Time-period cursor |
| **Start + Select** | Quit the game |

## Thanks

- **Square Enix** — for *Chrono Trigger*, one of the greatest RPGs ever made.
- The **ct_nx** loader project, on which this port is built.
- The **PortMaster** community and toolchain.
- **FFmpeg** (LGPL) — bundled, decode-only, for cutscene playback.
- **Caveras** — the *ChronoType* SNES font recreation (CC BY-NC-SA), used for in-game text.

## Compile

The loader and glue are open source (MIT) at **github.com/springah/ct_pm**. The game
data is never distributed — it comes from your own APK as above.

```
# in the PortMaster aarch64 builder image:
bash portmaster/ffmpeg-build.sh     # build the bundled decode-only FFmpeg
bash portmaster/build.sh            # build the aarch64 `ct` binary (links FMV)
bash portmaster/package.sh          # assemble ct.zip
```

See `portmaster/NOTES.md` in the repo for the full build/recompile details.
