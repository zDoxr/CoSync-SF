"""animvars: map Starfield's static BSFixedString globals to their names and list the code that
uses the animation-graph variable names (evidence for docs/ANIMATION_REVERSE_ENGINEERING.md).

  python tools/re/animvars.py names  out.json        # every global built by BSFixedString ctor 0x28CBA80
  python tools/re/animvars.py refs   names.json out.json [lo hi]   # code refs into a global range
                                                     # (default: the graph-variable cluster 0x5E89B20..0x5E8A5E0)
  python tools/re/animvars.py annotate names.json in.c out.c       # tag IDA pseudocode globals with names

Static initializers look like: lea rdx,"<name>"; lea rcx,<global>; call 0x28CBA80 (1.16.244).
"""
import collections, json, re, sys
import numpy as np
import sfre

CTOR = 0x28CBA80


def names(out):
    img = sfre.Image()
    ts, tn = img.sections[".text"]
    arr = np.frombuffer(img.mem[ts:ts + tn], dtype=np.uint8)
    res = {}
    for i in np.nonzero(arr == 0xE8)[0]:
        at = ts + int(i)
        if at + 5 + int.from_bytes(img.mem[at + 1:at + 5], "little", signed=True) != CTOR:
            continue
        s = g = None
        for ins in img.cs.disasm(img.mem[at - 24:at + 5], img.base + at - 24):
            if ins.mnemonic == "lea" and "rip" in ins.op_str:
                t = img.ins_target(ins)
                if ins.op_str.startswith("rdx"): s = t
                if ins.op_str.startswith("rcx"): g = t
        if s and g:
            n = img.cstr(s)
            if n and all(32 <= ord(c) < 127 for c in n):
                res[g] = n
    json.dump({hex(k): v for k, v in sorted(res.items())}, open(out, "w"), indent=0)
    print(len(res), "named globals")


def refs(names_file, out, lo=0x5E89B20, hi=0x5E8A5E0):
    img = sfre.Image()
    nm = {int(k, 16): v for k, v in json.load(open(names_file)).items()}
    tva, t = img.text()
    n = len(t) - 4
    d = (t[:n].astype(np.int64) | (t[1:n + 1].astype(np.int64) << 8) | (t[2:n + 2].astype(np.int64) << 16)
         | (t[3:n + 3].astype(np.int64) << 24))
    d = np.where(d >= 2**31, d - 2**32, d)
    pos = np.arange(n, dtype=np.int64) + tva
    cands = set()
    for tail in (4, 5, 8):
        tg = pos + tail + d
        cands.update(int(pos[k]) for k in np.nonzero((tg >= lo) & (tg < hi))[0])
    found = collections.defaultdict(list)
    for disp_at in sorted(cands):
        for back in range(1, 8):
            s = disp_at - back
            ins = next(img.cs.disasm(img.mem[s:s + 16], img.base + s, 1), None)
            if ins is None: continue
            tg = img.ins_target(ins)
            if tg is not None and lo <= tg < hi and ins.address - img.base + ins.size > disp_at:
                f = img.func_of(s)
                found[tg].append([hex(s), hex(f[0]) if f else None, ins.mnemonic + " " + ins.op_str])
                break
    json.dump({hex(g): {"name": nm.get(g, "?"), "refs": found[g]} for g in sorted(found)}, open(out, "w"), indent=1)
    print(len(found), "globals referenced")


def annotate(names_file, src, dst):
    nm = {int(k, 16): v for k, v in json.load(open(names_file)).items()}
    def sub(m):
        n = nm.get(int(m.group(2), 16) - 0x140000000)
        return f'{m.group(0)}/*"{n}"*/' if n else m.group(0)
    text = open(src, encoding="utf-8").read()
    open(dst, "w", encoding="utf-8").write(
        re.sub(r"\b(qword|unk|off|stru|byte|dword|xmmword)_(14[0-9A-F]{7})\b", sub, text))


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "names": names(sys.argv[2])
    elif cmd == "refs": refs(sys.argv[2], sys.argv[3], *(int(x, 16) for x in sys.argv[4:6]))
    elif cmd == "annotate": annotate(sys.argv[2], sys.argv[3], sys.argv[4])
