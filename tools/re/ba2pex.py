"""Extract one file from a Bethesda BA2 (BTDX, GNRL) archive and print native-function
signatures from a compiled Papyrus .pex. Usage: ba2pex.py <archive.ba2> <path/in/archive.pex> [FunctionName...]"""
import struct, sys, zlib

def ba2_extract(path, want):
    f = open(path, 'rb'); d = f.read(24)
    magic, ver, kind, count, nameoff = struct.unpack('<4sI4sIQ', d)
    assert magic == b'BTDX' and kind == b'GNRL', (magic, kind)
    hdr = 24 + (8 if ver >= 2 else 0) + (4 if ver == 3 else 0)
    f.seek(hdr); recs = [struct.unpack('<IIIIQIII', f.read(36)) for _ in range(count)]
    f.seek(nameoff); names = []
    for _ in range(count):
        n, = struct.unpack('<H', f.read(2)); names.append(f.read(n).decode('latin1').replace(chr(92), '/').lower())
    i = names.index(want.lower())
    _, _, _, flags, off, packed, size, _ = recs[i]
    f.seek(off); raw = f.read(packed or size)
    if packed:
        try: raw = zlib.decompress(raw)
        except zlib.error:
            import lz4.block; raw = lz4.block.decompress(raw, uncompressed_size=size)
    return raw

E = '>'
def pex_strings(b):
    global E
    E = '<' if b[:4] == bytes.fromhex('dec057fa') else '>'
    # Header: magic u32 BE, major u8, minor u8, gameid u16, time u64, source, user, machine (wstring = u16 len + bytes)
    p = 4 + 2 + 2 + 8
    def ws(p):
        n, = struct.unpack(E + 'H', b[p:p + 2]); return b[p + 2:p + 2 + n].decode('latin1'), p + 2 + n
    for _ in range(3): _, p = ws(p)
    n, = struct.unpack(E + 'H', b[p:p + 2]); p += 2
    table = []
    for _ in range(n):
        s, p = ws(p); table.append(s)
    return table

if __name__ == '__main__':
    raw = ba2_extract(sys.argv[1], sys.argv[2])
    open(sys.argv[2].split('/')[-1], 'wb').write(raw) if len(sys.argv) > 3 and sys.argv[3] == '--save' else None
    st = pex_strings(raw)
    print('pex', len(raw), 'bytes,', len(st), 'strings; big-endian magic', raw[:4].hex())
    for fn in sys.argv[3:]:
        if fn not in st: print(fn, 'not in string table'); continue
        idx = st.index(fn)
        # Find the function entry: name index (u16 BE) followed by its FunctionData:
        # returnType u16, docstring u16, userFlags u32, flags u8, numParams u16, params (name u16, type u16)*
        key = struct.pack(E + 'H', idx)
        pos = 0
        while True:
            pos = raw.find(key, pos)
            if pos < 0: break
            try:
                ret, doc, uflags, flags, npar = struct.unpack(E + 'HHIBH', raw[pos + 2:pos + 13])
                if npar < 16 and ret < len(st) and doc < len(st):
                    pars = [struct.unpack(E + 'HH', raw[pos + 13 + 4 * k:pos + 17 + 4 * k]) for k in range(npar)]
                    if all(a < len(st) and t < len(st) for a, t in pars) and (flags & 2):
                        print(f'{st[ret]} {fn}(' + ', '.join(f'{st[t]} {st[a]}' for a, t in pars) + ')' + ('  native' if flags & 2 else ''))
            except struct.error:
                pass
            pos += 1
