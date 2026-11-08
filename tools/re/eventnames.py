"""Names every global event getter (Address Library IDs 64095..64237) from the executable.

Each getter returns a function-static BSTEventSource<T>. The object is in .data with its vtable
already written, so the vtable's RTTI names T. Usage: python eventnames.py
"""
import re
import struct

import sfre


def rtti_name(image, vtable):
    rva = vtable - image.base
    col = struct.unpack_from("<Q", image.mem, rva - 8)[0] - image.base
    descriptor = struct.unpack_from("<I", image.mem, col + 0xC)[0]
    return image.mem[descriptor + 0x10:descriptor + 0x90].split(b"\0")[0].decode(errors="ignore")


def main():
    image = sfre.Image()
    library = open(sfre.ADDRLIB, "rb").read()
    for event_id in range(64095, 64238):
        rva = struct.unpack_from("<I", library, 0x60 + 4 * event_id)[0]
        if rva == 0:
            print(event_id, "no address")
            continue
        code = image.mem[rva : rva + 0x70]
        lea = code.find(b"\x48\x8d\x05")  # lea rax, [rip + disp32]: the static source
        source = rva + lea + 7 + struct.unpack_from("<i", code, lea + 3)[0]
        name = rtti_name(image, struct.unpack_from("<Q", image.mem, source)[0])
        print(event_id, re.sub(r"^\.\?AV\?\$BSTEventSource@[UV]?", "", name).rstrip("@"))


if __name__ == "__main__":
    main()
