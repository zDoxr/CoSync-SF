"""sfre: small static-analysis helper for Starfield.exe (no IDA/Ghidra needed).

Evidence it produces is read directly from the shipped executable:
  * function bounds from the PE .pdata exception table (exact, compiler-emitted)
  * disassembly via capstone
  * rip-relative data references and direct call/jmp xrefs by a full .text scan
  * Address Library (versionlib v5) ID -> RVA, the same mapping CommonLibSF uses
  * MSVC RTTI: class name -> type descriptor -> complete object locators -> vtables

Usage (python -I recommended):
  sfre.py id <ID>...                 RVA for Address Library IDs
  sfre.py rid <RVA>...               Address Library IDs for RVAs
  sfre.py func <RVA> [maxinsn]       disassemble the function containing RVA
  sfre.py str <text>                 find ASCII string(s) and their code references
  sfre.py refs <RVA>                 code locations referencing RVA (lea/mov rip-rel, call, jmp)
  sfre.py vtbl <MangledName>         vtables for an RTTI class, e.g. .?AVActor@@
  sfre.py q <RVA> [count]            read qwords at RVA (pointers shown as RVAs)

Defaults: EXE and versionlib paths below; override with SFRE_EXE / SFRE_ADDRLIB.
"""
import bisect
import os
import struct
import sys

import capstone
import numpy as np
import pefile

EXE = os.environ.get("SFRE_EXE", r"D:\SteamLibrary\steamapps\common\Starfield\Starfield.exe")
ADDRLIB = os.environ.get(
    "SFRE_ADDRLIB", r"D:\SteamLibrary\steamapps\common\Starfield\Data\SFSE\Plugins\versionlib-1-16-244-0.bin")


class Image:
    def __init__(self, path=EXE):
        self.pe = pefile.PE(path, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        size = self.pe.OPTIONAL_HEADER.SizeOfImage
        self.mem = bytearray(size)
        raw = open(path, "rb").read()
        self.sections = {}
        for s in self.pe.sections:
            name = s.Name.rstrip(b"\0").decode()
            n = min(s.SizeOfRawData, s.Misc_VirtualSize)
            self.mem[s.VirtualAddress:s.VirtualAddress + n] = raw[s.PointerToRawData:s.PointerToRawData + n]
            self.sections[name] = (s.VirtualAddress, s.Misc_VirtualSize)
        self.mem = bytes(self.mem)
        self._funcs = None
        self.cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.cs.detail = True

    # ---- functions (.pdata) ----
    def funcs(self):
        if self._funcs is None:
            va, size = self.sections[".pdata"]
            arr = np.frombuffer(self.mem[va:va + size - size % 12], dtype="<u4").reshape(-1, 3)
            arr = arr[arr[:, 0] != 0]
            order = np.argsort(arr[:, 0])
            self._funcs = (arr[order, 0].copy(), arr[order, 1].copy(), arr[order, 2].copy())
        return self._funcs

    def func_of(self, rva):
        """(begin, end) of the .pdata range containing rva, following chained unwind info to the primary entry."""
        begins, ends, unwind = self.funcs()
        i = bisect.bisect_right(begins, rva) - 1
        if i < 0 or rva >= ends[i]:
            return None
        b, e, u = int(begins[i]), int(ends[i]), int(unwind[i])
        # Chained unwind info (UNW_FLAG_CHAININFO = 4): the primary RUNTIME_FUNCTION follows the codes.
        for _ in range(16):
            flags = self.mem[u] >> 3
            if not flags & 4:
                break
            count = self.mem[u + 2]
            off = u + 4 + ((count + 1) & ~1) * 2
            pb, pe_, pu = struct.unpack_from("<III", self.mem, off)
            b, u = pb, pu
        return b, e

    # ---- address library ----
    def addrlib(self):
        data = open(ADDRLIB, "rb").read()
        fmt, = struct.unpack_from("<I", data, 0)
        assert fmt == 5, f"unexpected address library format {fmt}"
        ver = struct.unpack_from("<4I", data, 4)
        ptr, dfmt, count = struct.unpack_from("<iiI", data, 0x54)
        table = np.frombuffer(data, dtype="<u4", count=count, offset=0x60)
        return ver, table

    # ---- disassembly ----
    def disasm(self, start, end, maxn=100000):
        code = self.mem[start:end]
        return list(self.cs.disasm(code, self.base + start, maxn))

    def rva(self, va):
        return va - self.base

    # ---- references ----
    def text(self):
        va, size = self.sections[".text"]
        return va, np.frombuffer(self.mem[va:va + size], dtype=np.uint8)

    def refs_to(self, target):
        """Candidate code references to target RVA: rip-relative disp32 at any offset whose
        end-of-instruction lands 4..6 bytes after the disp (covers lea/mov/cmp; imm8/imm32 forms),
        plus E8/E9 rel32. Every candidate is confirmed by decoding the instruction."""
        tva, t = self.text()
        n = len(t) - 4
        d = (t[:n].astype(np.int64) | (t[1:n + 1].astype(np.int64) << 8) | (t[2:n + 2].astype(np.int64) << 16)
             | (t[3:n + 3].astype(np.int64) << 24))
        d = np.where(d >= 2**31, d - 2**32, d)
        pos = np.arange(n, dtype=np.int64) + tva
        hits = set()
        for tail in (4, 5, 8):  # disp is followed by 0, 1 or 4 more bytes
            m = np.nonzero(pos + tail + d == target)[0]
            for k in m:
                hits.add(int(pos[k]))
        out = []
        for disp_at in sorted(hits):
            for back in range(1, 8):
                start = disp_at - back
                ins = next(self.cs.disasm(self.mem[start:start + 16], self.base + start, 1), None)
                if ins is None:
                    continue
                tgt = self.ins_target(ins)
                if tgt == target and ins.address - self.base + ins.size > disp_at:
                    out.append((start, ins))
                    break
        return out

    def ins_target(self, ins):
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                return ins.address + ins.size + op.mem.disp - self.base
            if op.type == capstone.x86.X86_OP_IMM and ins.mnemonic in ("call", "jmp") or (
                    op.type == capstone.x86.X86_OP_IMM and ins.mnemonic.startswith("j")):
                return op.imm - self.base
        return None

    # ---- strings ----
    def find_bytes(self, needle, section=".rdata"):
        va, size = self.sections[section]
        blob = self.mem[va:va + size]
        out, i = [], blob.find(needle)
        while i >= 0:
            out.append(va + i)
            i = blob.find(needle, i + 1)
        return out

    def cstr(self, rva, maxlen=200):
        b = self.mem[rva:rva + maxlen]
        return b.split(b"\0")[0].decode("latin1")

    def q(self, rva):
        return struct.unpack_from("<Q", self.mem, rva)[0]

    # ---- RTTI ----
    def vtables(self, mangled):
        """Vtables of a class: TypeDescriptor name '.?AV<cls>@@' -> COLs (signature 1, x64) -> vtable = COL ref + 8."""
        name = mangled.encode() + b"\0"
        tds = [r - 0x10 for r in self.find_bytes(name, ".data")]
        res = []
        rva_va, rsize = self.sections[".rdata"]
        rd = np.frombuffer(self.mem[rva_va:rva_va + rsize - rsize % 4], dtype="<u4")
        for td in tds:
            for idx in np.nonzero(rd == td)[0]:
                col = rva_va + int(idx) * 4 - 12  # COL: sig, offset, cdOffset, pTypeDescriptor(rva)
                sig, off, cd = struct.unpack_from("<III", self.mem, col)
                if sig != 1:
                    continue
                colva = self.base + col
                qs = np.frombuffer(self.mem[rva_va:rva_va + rsize - rsize % 8], dtype="<u8")
                for j in np.nonzero(qs == colva)[0]:
                    res.append((td, col, off, rva_va + int(j) * 8 + 8))
                # also unaligned search
        return res


def fmt_ins(img, ins):
    rva = ins.address - img.base
    extra = ""
    t = img.ins_target(ins)
    if t is not None and 0 <= t < len(img.mem):
        s = img.mem[t:t + 64]
        if all(32 <= c < 127 for c in s[:4]) and b"\0" in s:
            extra = f'   ; "{img.cstr(t)}"'
        else:
            extra = f"   ; -> {t:#x}"
    return f"  {rva:#09x}: {ins.mnemonic:6} {ins.op_str}{extra}"


def main(argv):
    img = Image()
    cmd = argv[1]
    if cmd == "id":
        ver, table = img.addrlib()
        print("addrlib game version", ver)
        for a in argv[2:]:
            print(f"ID {a} -> RVA {int(table[int(a)]):#x}")
    elif cmd == "rid":
        ver, table = img.addrlib()
        for a in argv[2:]:
            r = int(a, 16)
            print(a, "-> IDs", np.nonzero(table == r)[0].tolist())
    elif cmd == "func":
        r = int(argv[2], 16)
        maxn = int(argv[3]) if len(argv) > 3 else 400
        fr = img.func_of(r)
        if not fr:
            print("not inside a .pdata function")
            return
        b, e = fr
        print(f"function {b:#x}..{e:#x} (pdata)")
        for ins in img.disasm(b, e, maxn):
            print(fmt_ins(img, ins))
    elif cmd == "str":
        text = " ".join(argv[2:]).encode()
        for sec in (".rdata", ".data"):
            for r in img.find_bytes(text + b"\0", sec):
                print(f'{sec} {r:#x} "{img.cstr(r)}"')
                for at, ins in img.refs_to(r):
                    f = img.func_of(at)
                    print(f"    ref {fmt_ins(img, ins).strip()}  in func {f[0]:#x}" if f else f"    ref {at:#x}")
    elif cmd == "refs":
        r = int(argv[2], 16)
        for at, ins in img.refs_to(r):
            f = img.func_of(at)
            print(f"{fmt_ins(img, ins)}  in func {f[0]:#x}" if f else f"{at:#x}")
    elif cmd == "vtbl":
        for td, col, off, vt in img.vtables(argv[2]):
            print(f"TD {td:#x} COL {col:#x} offset {off:#x} vtable {vt:#x}")
    elif cmd == "q":
        r = int(argv[2], 16)
        n = int(argv[3]) if len(argv) > 3 else 8
        for i in range(n):
            v = img.q(r + 8 * i)
            print(f"  {r + 8 * i:#x}: {v:#018x}  (rva {v - img.base:#x})")


if __name__ == "__main__":
    main(sys.argv)
