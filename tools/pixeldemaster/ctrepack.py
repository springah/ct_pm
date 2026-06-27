#!/usr/bin/env python3
"""Repack Chrono Trigger Android resources.bin ('ARC1'/DetchmanResource),
optionally overriding entries with replacement raw bytes (e.g. Pixel Demaster PNGs).
Format per ct-resources-bin-format: LCG-XOR(seed=BASE+pos) + per-entry raw-deflate
([BE u32 usize][deflate]). See ctarc.py for the verified reader.
"""
import struct, zlib, sys, os, ctarc

BASE=ctarc.BASE  # 0x19000000

def enc_blob(raw, pos):
    """raw bytes -> [BE usize][GZIP stream] then XOR(seed=BASE+pos).
    The engine inflates via ZipUtils::inflateMemory -> inflateInit2_(windowBits=47),
    i.e. auto zlib/gzip header detection, which REJECTS raw/headerless deflate.
    Originals are gzip (1f 8b 08 ..). Must emit a gzip-wrapped stream, NOT raw deflate."""
    co=zlib.compressobj(9, zlib.DEFLATED, 16+15)   # 16+15 = gzip wrapper (1f 8b), mtime 0
    comp=co.compress(raw)+co.flush()
    blob=struct.pack(">I",len(raw))+comp
    return ctarc.dec(blob,(BASE+pos)&0xffffffff)   # dec==enc (symmetric XOR)

def repack(in_path, out_path, overrides=None, verbose=True):
    overrides=overrides or {}
    f,toc,info=ctarc.open_arc(in_path)
    count,ents=ctarc.parse_toc(toc)       # (name,pos,size,nameOff)
    # 1) gather each entry's RAW (inflated) content: override or original
    raws=[]; used=set()
    for nm,pos,size,no in ents:
        if nm in overrides:
            raws.append(overrides[nm]); used.add(nm)
        else:
            f.seek(pos); raws.append(ctarc.inflate_blob(ctarc.dec(f.read(size),(BASE+pos)&0xffffffff)))
    if verbose: print("applied %d/%d overrides (%d unused)"%(len(used),count,len(set(overrides)-used)))
    # 2) lay out entries right after 16-byte header, record new pos/size
    HDR=16
    cur=HDR
    blobs=[]; newmeta=[]   # (nameOff,pos,size)
    for i,(nm,pos,size,no) in enumerate(ents):
        b=enc_blob(raws[i],cur)
        blobs.append(b); newmeta.append([no,cur,len(b)]); cur+=len(b)
    entries_end=cur
    # 3) rebuild TOC blob: count + entries{nameOff,pos,size} + name pool (reuse nameOffs/pool)
    #    name pool layout is unchanged (names identical) -> reuse original pool bytes & nameOffs.
    pool_start=4+count*12
    pool=toc[pool_start:]                  # original pooled name strings (unchanged)
    newtoc=bytearray()
    newtoc+=struct.pack("<I",count)
    for nameOff,pos,size in newmeta:
        newtoc+=struct.pack("<iii",nameOff,pos,size)
    newtoc+=pool
    # 4) encrypt+deflate the TOC at offset entries_end
    toc_off=entries_end
    toc_blob=enc_blob(bytes(newtoc),toc_off)
    total=toc_off+len(toc_blob)
    # 5) header
    hdr=struct.pack("<IIII",0x31435241,total,toc_off,len(toc_blob))
    hdr=ctarc.dec(hdr,BASE+0)
    # 6) write
    with open(out_path,"wb") as o:
        o.write(hdr)
        for b in blobs: o.write(b)
        o.write(toc_blob)
    if verbose: print("wrote %s  (%d bytes, tocOff=0x%x)"%(out_path,total,toc_off))
    return out_path

if __name__=="__main__":
    # round-trip self-test: repack with NO overrides, reopen, verify N entries decode identically
    src=sys.argv[1] if len(sys.argv)>1 else "/Users/shant/Desktop/ct_emu/assets/resources.bin"
    out="/tmp/rt_test.bin"
    print("=== round-trip (no overrides) ===")
    repack(src,out,{})
    # verify
    fa,ta,ia=ctarc.open_arc(src); ca,ea=ctarc.parse_toc(ta)
    fb,tb,ib=ctarc.open_arc(out); cb,eb=ctarc.parse_toc(tb)
    assert ca==cb, "count mismatch %d/%d"%(ca,cb)
    import random
    idxs=list(range(0,ca,max(1,ca//40)))[:40]
    bad=0
    for i in idxs:
        nm,pa,sa,_=ea[i]; _,pb,sb,_=eb[i]
        fa.seek(pa); ra=ctarc.inflate_blob(ctarc.dec(fa.read(sa),(BASE+pa)&0xffffffff))
        fb.seek(pb); rb=ctarc.inflate_blob(ctarc.dec(fb.read(sb),(BASE+pb)&0xffffffff))
        if ra!=rb: bad+=1; print("  MISMATCH %s"%nm)
    print("verified %d sample entries, mismatches=%d"%(len(idxs),bad))
    print("ORIG size=%d  REPACK size=%d"%(os.path.getsize(src),os.path.getsize(out)))
    print("OK" if bad==0 else "FAIL")
