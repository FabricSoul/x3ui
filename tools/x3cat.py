#!/usr/bin/env python3
"""x3cat.py — read X3 Terran Conflict / Albion Prelude .cat/.dat archives.

X3 archive format (empirically verified against this install, matches the
community spec from Doubleshadow / the X-universe wiki):

  .cat  = the CATALOG (index), a text file XOR-encrypted byte-by-byte with a
          position-dependent key:  plain[i] = cipher[i] ^ ((0xDB + i) & 0xFF).
          Decoded text: line 0 = a (often placeholder, e.g. "foo.dat") dat name;
          every following line is "<relative/path> <bytesize>". Files live in the
          sibling .dat (same basename: 01.cat -> 01.dat) concatenated in this
          exact order, so each file's offset = sum of all previous sizes.

  .dat  = the DATA blob: the concatenated file bytes, each byte XOR 0x33.
          A decoded slice whose path ends .pck (or starts with the gzip magic
          1f 8b) is gzip-compressed -> gunzip to get the real content.

So full extraction of one file = read its [offset, offset+size) slice from .dat,
XOR 0x33, then gunzip if it's a .pck. X3AP content lives under addon/ (addon cats
win over base). Base-game pages (17/35, job-name page) and types/*.txt live ONLY
in the archives, never loose — which is why grepping the loose folders misses them.

usage:
  x3cat.py list   <cat>                      # path / size / offset listing
  x3cat.py find   <cat...> <path_regex>      # entries whose path matches
  x3cat.py cat    <cat> <path_regex>         # decode+gunzip matching files to stdout
  x3cat.py grep   <content_regex> <pathglob> <cat...>   # search file CONTENT
        pathglob filters which files to crack first (cheap); only matching files
        are XOR'd+gunzipped+searched. e.g.  grep 'Food Transporter' 't/.*\\.pck' addon/04.cat
"""
import sys, os, re, gzip

_X33 = bytes(b ^ 0x33 for b in range(256))            # fast dat XOR table

def decode_cat(catpath):
    raw = open(catpath, 'rb').read()
    dec = bytes((raw[i] ^ ((0xDB + i) & 0xFF)) for i in range(len(raw)))
    text = dec.decode('latin1')
    out, off = [], 0
    for ln in text.replace('\r\n', '\n').split('\n'):
        ln = ln.strip()
        if not ln:
            continue
        path, sep, size = ln.rpartition(' ')
        if not sep or not size.isdigit():
            continue                                   # line 0 (dat name) / junk
        out.append((path, int(size), off))
        off += int(size)
    return out

def dat_path(catpath):
    return catpath[:-4] + '.dat'

def read_entry(catpath, entry):
    path, size, off = entry
    with open(dat_path(catpath), 'rb') as f:
        f.seek(off)
        blob = f.read(size)
    data = blob.translate(_X33)
    if path.lower().endswith('.pck') or data[:2] == b'\x1f\x8b':
        try:
            data = gzip.decompress(data)
        except Exception:
            pass
    return data

def main():
    a = sys.argv
    if len(a) < 3:
        print(__doc__); sys.exit(1)
    mode = a[1]
    if mode == 'list':
        for p, s, o in decode_cat(a[2]):
            print(f"{o:>12} {s:>10}  {p}")
    elif mode == 'find':
        *cats, rx = a[2:]
        pat = re.compile(rx, re.I)
        for cat in cats:
            for p, s, o in decode_cat(cat):
                if pat.search(p):
                    print(f"{cat}: {p}  ({s} bytes)")
    elif mode == 'cat':
        cat, rx = a[2], a[3]
        pat = re.compile(rx, re.I)
        for e in decode_cat(cat):
            if pat.search(e[0]):
                sys.stdout.buffer.write(read_entry(cat, e))
    elif mode == 'grep':
        crx, pathglob, *cats = a[2], a[3], a[4:]
        cpat, ppat = re.compile(crx, re.I), re.compile(pathglob, re.I)
        for cat in cats:
            for e in decode_cat(cat):
                if not ppat.search(e[0]):
                    continue
                try:
                    txt = read_entry(cat, e).decode('latin1')
                except Exception:
                    continue
                for ln in txt.split('\n'):
                    if cpat.search(ln):
                        print(f"{cat}:{e[0]}: {ln.strip()[:160]}")
    else:
        print(__doc__); sys.exit(1)

if __name__ == '__main__':
    main()
