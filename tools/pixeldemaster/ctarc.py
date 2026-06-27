#!/usr/bin/env python3
"""Chrono Trigger (Android v2.1.5) resources.bin reader.
Verified format: 'ARC1' / DetchmanResource. LCG-XOR(rand) + per-entry deflate.
  seed = BASE + blob_file_offset; per byte seed = seed*MUL+INC; key = seed>>24
  header@0 (16B): magic 'ARC1', u32 totalLen, u32 tocOffset, u32 tocSize
  TOC = decrypt(seed=BASE+tocOff) -> [BE u32 usize][raw deflate]
        inflated: u32 fileCount, then count*12B {i32 nameOff, i32 pos, i32 size}, name pool follows
  entry = decrypt(seed=BASE+pos) -> [BE u32 usize][raw deflate]
"""
import struct, zlib, sys, os

BASE=0x19000000; MUL=0x41C64E6D; INC=0x3039

def ks(seed,n):
    out=bytearray(n); s=seed&0xffffffff
    for i in range(n):
        s=(s*MUL+INC)&0xffffffff; out[i]=(s>>24)&0xff
    return bytes(out)

def dec(buf,seed):
    return bytes(b^x for b,x in zip(buf, ks(seed,len(buf))))

def inflate_blob(blob):
    """[BE u32 usize][raw deflate] -> bytes"""
    usize=struct.unpack(">I",blob[:4])[0]
    for wb in (-15,15,31):
        try:
            raw=zlib.decompress(blob[4:],wb)
            if len(raw)==usize: return raw
        except Exception: pass
    # last resort: decompressobj (trailing garbage tolerant)
    d=zlib.decompressobj(-15); raw=d.decompress(blob[4:])
    return raw

def open_arc(path):
    f=open(path,"rb")
    hdr=dec(f.read(16),BASE+0)
    magic,total,tocOff,tocSz=struct.unpack("<IIII",hdr)
    assert magic==0x31435241, "bad magic %08x"%magic
    flen=os.path.getsize(path)
    assert total==flen, "totalLen %d != file %d"%(total,flen)
    f.seek(tocOff); toc=inflate_blob(dec(f.read(tocSz),(BASE+tocOff)&0xffffffff))
    return f, toc, (magic,total,tocOff,tocSz)

def parse_toc(toc):
    count=struct.unpack("<I",toc[:4])[0]
    ents=[]
    p=4
    for i in range(count):
        nameOff,pos,size=struct.unpack_from("<iii",toc,p); p+=12
        ents.append((nameOff,pos,size))
    # resolve names: nameOff is relative to TOC blob start (try a couple bases)
    def readcstr(base,off):
        j=base+off
        if j<0 or j>=len(toc): return None
        e=toc.find(b"\0",j)
        if e<0: return None
        try: return toc[j:e].decode("utf-8")
        except: return toc[j:e].decode("latin1")
    out=[]
    base_blob=0
    for (nameOff,pos,size) in ents:
        nm=readcstr(base_blob,nameOff)
        out.append((nm,pos,size,nameOff))
    return count, out

if __name__=="__main__":
    path=sys.argv[1] if len(sys.argv)>1 else "resources.bin"
    f,toc,info=open_arc(path)
    print("header: magic ARC1  totalLen=0x%x  tocOff=0x%x  tocSz=0x%x"%(info[1],info[2],info[3]))
    print("inflated TOC = %d bytes"%len(toc))
    count,ents=parse_toc(toc)
    print("fileCount=%d"%count)
    print("--- first 25 entries (name, pos, size, nameOff) ---")
    for e in ents[:25]:
        print("  %-44s pos=0x%-9x size=%-9d nameOff=%d"%(str(e[0]),e[1],e[2],e[3]))
    # sanity: how many names resolved?
    named=sum(1 for e in ents if e[0])
    print("resolved names: %d / %d"%(named,count))
    # write full list
    with open("entries.txt","w") as o:
        for nm,pos,size,no in ents:
            o.write("%s\t%d\t%d\n"%(nm,pos,size))
    print("wrote entries.txt")
