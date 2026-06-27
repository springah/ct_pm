#!/usr/bin/env python3
"""Apply Pixel Demaster (or any CT_Explore .ctp) PNG payloads to an Android
Chrono Trigger resources.bin. Both inputs are USER-SUPPLIED (your own game dump
+ your own downloaded mod) — nothing here ships SQEX assets.

Usage:
  python3 apply_pixeldemaster.py <resources.bin> <out.bin> <layer1.ctp> [layer2.ctp ...] [--with-text]

Layers are applied in order; later .ctp files override earlier ones on name
conflicts (so pass Main first, then overlays: UI / button-prompts / icons).
Each .ctp is just a zip of replacement files keyed by the SAME paths the game
uses; only PNGs whose name exists in resources.bin are applied.

  --with-text  also apply Localize/*/msg/*.txt files (changes text layout; off by
               default = PNG visuals only, lowest risk).

Example (the shipped springah set):
  python3 apply_pixeldemaster.py resources.bin out.bin \
      CTPD-Main.ctp CTPD-DefaultUIBattleGauge.ctp CTPD-SNES.ctp \
      CTPD-ArtIconsSolid.ctp CTPD-InteractionsOn.ctp CTPD-InventoryColor.ctp
"""
import zipfile, sys, os, ctarc, ctrepack
PNG=bytes.fromhex("89504e47")
def main():
    args=[x for x in sys.argv[1:] if not x.startswith("--")]
    with_text="--with-text" in sys.argv
    if len(args)<3: print(__doc__); sys.exit(1)
    src,out,ctps=args[0],args[1],args[2:]
    _,toc,_=ctarc.open_arc(src); c,ents=ctarc.parse_toc(toc)
    andr={n for n,_,_,_ in ents}
    ov={}
    for ctp in ctps:
        z=zipfile.ZipFile(ctp); added=0
        for n in z.namelist():
            if n.endswith("/") or n not in andr: continue
            d=z.read(n)
            if d[:4]==PNG or with_text: ov[n]=d; added+=1
        print("  %-40s -> %d overrides"%(os.path.basename(ctp),added))
    print("total unique overrides: %d (with_text=%s)"%(len(ov),with_text))
    ctrepack.repack(src,out,ov)
    print("done ->",out)
if __name__=="__main__": main()
