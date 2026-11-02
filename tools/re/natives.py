"""List Papyrus native registrations for given names: class, arg count, native function pointer."""
import sys
from sfre import Image, fmt_ins
img = Image()
for name in sys.argv[1:]:
    for r in img.find_bytes(name.encode() + b"\0", ".rdata"):
        for at, ins in img.refs_to(r):
            # look forward for the call to the per-native constructor, and class string in r8
            seq = img.disasm(at, at + 40, 6)
            cls, ctor = None, None
            back = img.disasm(at - 20, at, 10)
            for i in back + seq:
                t = img.ins_target(i)
                if i.mnemonic == "lea" and i.op_str.startswith("r8") and t:
                    cls = img.cstr(t)
                if i.mnemonic == "call" and t:
                    ctor = t
                    break
            if not ctor:
                continue
            argc, fn = None, None
            for i in img.disasm(ctor, ctor + 0x80, 30):
                if "dword ptr [rsp + 0x20]" in i.op_str and i.mnemonic == "mov" and argc is None:
                    argc = i.operands[1].imm if i.operands[1].type == 2 else None
                if i.mnemonic == "lea" and i.op_str.startswith("rax") and fn is None and argc is not None:
                    pass
                if "qword ptr [rdi + 0x50], rax" in i.op_str or "qword ptr [rbx + 0x50], rax" in i.op_str:
                    break
            # native fn pointer: lea rax,[...] immediately before 'mov [reg+0x50], rax'
            ins = img.disasm(ctor, ctor + 0x100, 60)
            for a, b in zip(ins, ins[1:]):
                if b.mnemonic == "mov" and "+ 0x50], rax" in b.op_str and a.mnemonic == "lea":
                    fn = img.ins_target(a)
                    break
            print(f"{cls}.{name}: params={argc} native={fn:#x}" if fn else f"{cls}.{name}: params={argc} ctor={ctor:#x}")
