"""IDA batch: decompile a list of functions without waiting for full auto-analysis.

Usage (fresh scratch database, auto-analysis disabled with -a):
  set COSYNC_DECOMP_IN=<file with one RVA per line>   (hex, e.g. 0x18ef1a0; '#' starts a comment)
  set COSYNC_DECOMP_OUT=<output .c file>
  idat -A -a -c -L<log> -S"decompile_list.py" -o<scratch.i64> Starfield.exe

Each function is analysed locally with plan_and_wait before Hex-Rays runs (callee
prototypes are therefore guessed; treat argument types as INFERRED). Output: one block per
function, headed by its RVA, followed by the pseudocode or the failure reason.
"""
import os
import ida_auto, ida_funcs, ida_hexrays, ida_nalt, ida_pro

BASE = ida_nalt.get_imagebase()


def analyse(ea):
    f = ida_funcs.get_func(ea)
    if not f:
        ida_funcs.add_func(ea)
        f = ida_funcs.get_func(ea)
    if not f:
        return None
    ida_auto.plan_and_wait(f.start_ea, f.end_ea)
    return ida_funcs.get_func(ea)


def main():
    src = os.environ["COSYNC_DECOMP_IN"]
    dst = os.environ["COSYNC_DECOMP_OUT"]
    if not ida_hexrays.init_hexrays_plugin():
        open(dst, "w").write("Hex-Rays not available\n")
        return
    rvas = []
    for line in open(src):
        line = line.split("#")[0].strip()
        if line:
            rvas.append(int(line, 16))
    with open(dst, "w", encoding="utf-8") as out:
        for rva in rvas:
            ea = BASE + rva
            out.write(f"\n// ===== RVA {rva:#x} (VA {ea:#x}) =====\n")
            f = analyse(ea)
            if not f:
                out.write("// no function at this address\n")
                continue
            try:
                cf = ida_hexrays.decompile(f.start_ea)
                out.write(str(cf) if cf else "// decompile returned None\n")
            except ida_hexrays.DecompilationFailure as e:
                out.write(f"// decompilation failed: {e}\n")
            out.flush()


main()
ida_pro.qexit(0)
