# Pixel Demaster asset tool (ct_pm)

Re-skin the **Chrono Trigger** mobile/PortMaster build with community pixel-art
mods (e.g. **Pixel Demaster**) by repacking the game's `resources.bin` archive.

**Ships no game assets.** You supply your own `resources.bin` (from your own
Chrono Trigger v2.1.5 Android APK) **and** your own mod download. This tool only
reads, transcodes, and rewrites *your* files — same user-supplied model as the
rest of ct_pm.

## Quick start
```sh
# inspect: list every entry in your archive -> entries.txt
python3 ctarc.py /path/to/resources.bin

# build: layer one or more .ctp packs onto your archive (later layers win)
python3 apply_pixeldemaster.py resources.bin resources.modded.bin \
    CTPD-Main.ctp CTPD-DefaultUIBattleGauge.ctp CTPD-SNES.ctp \
    CTPD-ArtIconsSolid.ctp CTPD-InteractionsOn.ctp CTPD-InventoryColor.ctp
```
Then copy `resources.modded.bin` onto the device as `assets/resources.bin`
(keep a backup of the original). To revert, restore the backup.

## How it works
A Pixel Demaster `.ctp` is just a ZIP of replacement files keyed by the **same
paths the game uses** (`Game/chara/png/c000_0.png`, `Extension/title.png`, …) —
not encrypted, no CT_Explore needed. The tool extracts the PNGs whose names exist
in your `resources.bin`, swaps them in, and rewrites the archive.

`resources.bin` format (`ARC1` / `DetchmanResource`, fully reverse-engineered,
v2.1.5):
- **Cipher:** per-byte XOR stream keyed by the `rand()` LCG — `seed = 0x19000000 +
  blob_file_offset`, `seed = seed*0x41C64E6D + 0x3039`, `key = seed >> 24`
  (symmetric; encrypt == decrypt).
- **Header** (16 B, XOR seed = base): `magic "ARC1"`, `u32 totalLen` (== file
  length), `u32 tocOffset`, `u32 tocSize`.
- **Each entry / the TOC:** `[BE u32 uncompressed_size]` + a **gzip** stream. The
  engine inflates with `inflateInit2_(windowBits=47)` (auto zlib/gzip detect), so
  entries **must be gzip-wrapped — raw deflate is rejected** (this is the one
  non-obvious gotcha).
- **TOC** (gzip, at `tocOffset`): `u32 fileCount` + `fileCount × {i32 nameOffset,
  i32 pos, i32 size}` (name-sorted for binary search) + a pooled name-string blob.
- Entry `pos` values are absolute `SEEK_SET` file offsets.

## Files
- `ctarc.py` — reader (open / parse TOC / inflate / dump `entries.txt`)
- `ctrepack.py` — writer (re-deflate as gzip + XOR + rebuild TOC; round-trip
  verified byte-identical decode)
- `apply_pixeldemaster.py` — layered `.ctp` driver
- `--with-text` applies the mod's `Localize/*/msg/*.txt` files too (changes text
  layout for the SNES font — off by default; test before relying on it)

## Credits

This tool stands on the Chrono Trigger PC modding community's work:
- **River Nyxx** — [CT_Explore](https://rivernyxx.com), which established the
  `resources.bin` workflow and the `.ctp` patch format this tool reads.
- **Shiryu** — [Pixel Demaster](https://www.nexusmods.com/chronotrigger/mods/8),
  the pixel-art pack this is built to apply (Pixel Demaster in turn credits Caveras,
  Jackster, and Primogenitor for its assets — see its Nexus page).

This tool ships none of their work; it only lets you apply mods you download yourself.

## Notes / caveats
- **Version-locked to CT mobile v2.1.5** (the seed/offsets are version-specific).
  Re-run against whatever `resources.bin` your target actually uses.
- Pixel Demaster replaces the normal RGBA `Game/chara/png/` textures; it does **not**
  touch the paletted `Game/chara/dat/` in-battle sprites (index-map + per-asset
  BGR555 palette, animated at runtime) — so palette effects are untouched.
- RGBA replacements use ~4× the texture RAM of the originals (8-bit indexed). Fine
  in practice, but worth watching for OOM on heavy scenes on low-RAM handhelds.
- The mod's own font pack is skipped — ct_pm renders text through its own font path.
