"""Static inspection of CoD1's cgame module.

Confirms the Quake 3 module ABI (vmMain / dllEntry) and locates the global
variable that holds the engine syscall pointer -- the hook point the whole
VR mod hangs off.

In Quake 3, dllEntry is compiled to:
    mov eax, [esp+4]      ; the engine's syscall function pointer
    mov [g_syscall], eax  ; stash it in a module global
    ret
Recovering the address baked into that second instruction gives us a pointer
we can overwrite at runtime to interpose on every cgame -> engine call.
"""

import struct
import sys
from pathlib import Path

GAME = Path(r"H:\SteamLibrary\steamapps\common\Call of Duty")


def load_pe(path):
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe:pe + 4] == b"PE\0\0"
    num_sections = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    image_base = struct.unpack_from("<I", data, opt + 28)[0]
    exp_rva, exp_size = struct.unpack_from("<II", data, opt + 96)
    sec_off = opt + opt_size
    sections = []
    for i in range(num_sections):
        s = sec_off + i * 40
        name = data[s:s + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, va, raw_size, raw_off = struct.unpack_from("<IIII", data, s + 8)
        sections.append((name, va, vsize, raw_off, raw_size))
    return data, image_base, exp_rva, exp_size, sections


def rva_to_off(sections, rva):
    for _, va, vsize, raw_off, raw_size in sections:
        if va <= rva < va + max(vsize, raw_size):
            return raw_off + (rva - va)
    raise ValueError(f"RVA 0x{rva:X} unmapped")


def exports_of(path):
    data, base, exp_rva, exp_size, sections = load_pe(path)
    if exp_rva == 0:
        return base, sections, []
    e = rva_to_off(sections, exp_rva)
    ord_base = struct.unpack_from("<I", data, e + 16)[0]
    num_names = struct.unpack_from("<I", data, e + 24)[0]
    a_funcs, a_names, a_ords = struct.unpack_from("<III", data, e + 28)
    of, on, oo = (rva_to_off(sections, x) for x in (a_funcs, a_names, a_ords))
    out = []
    for i in range(num_names):
        nrva = struct.unpack_from("<I", data, on + i * 4)[0]
        o = rva_to_off(sections, nrva)
        name = data[o:data.index(b"\0", o)].decode("ascii")
        ordinal = struct.unpack_from("<H", data, oo + i * 2)[0]
        frva = struct.unpack_from("<I", data, of + ordinal * 4)[0]
        out.append((name, frva))
    return base, sections, out


def main():
    for mod in ("cgamex86.dll", "gamex86.dll", "uix86.dll"):
        path = GAME / mod
        if not path.exists():
            print(f"!! missing {path}")
            continue
        data, base, exp_rva, exp_size, sections = load_pe(path)
        _, _, exports = exports_of(path)

        print(f"\n=== {mod} ===")
        print(f"  ImageBase 0x{base:08X}   sections: "
              + ", ".join(f"{n}@0x{va:X}" for n, va, _, _, _ in sections))
        print(f"  exports ({len(exports)}): "
              + ", ".join(n for n, _ in exports) if exports else "  (no exports)")

        for name, frva in exports:
            off = rva_to_off(sections, frva)
            code = data[off:off + 24]
            print(f"  {name} @ RVA 0x{frva:X}")
            print(f"    bytes: {' '.join(f'{b:02X}' for b in code)}")

            # mov eax,[esp+4] ; mov [imm32],eax ; ret   ->  8B 44 24 04 A3 xx xx xx xx C3
            if code[:4] == b"\x8b\x44\x24\x04" and code[4] == 0xA3:
                gv = struct.unpack_from("<I", code, 5)[0]
                print(f"    ** dllEntry pattern matched **")
                print(f"       syscall global VA  = 0x{gv:08X}")
                print(f"       syscall global RVA = 0x{gv - base:08X}")
                for n, va, vsize, _, _ in sections:
                    if va <= gv - base < va + vsize:
                        print(f"       lives in section   = {n}")


if __name__ == "__main__":
    main()
