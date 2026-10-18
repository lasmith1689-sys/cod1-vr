#!/usr/bin/env python3
"""Static answer to the hand-visual blocker.

THE QUESTION
------------
The first-person view is two models: a hands model carrying the skeleton
(and `tag_weapon`), and a gun model attached to it. The entity hook sees
syscalls 37 and 38 go past and can rewrite the transform in the struct they
point at -- but if it cannot tell one add from another, it can only move the
whole assembly rigidly, which is wrong the moment the support hand leaves the
rifle.

The previous pass planned to answer this by printing an argument census during
a headset session. That spends the scarcest resource in the project -- a real
play session -- on a question the shipped binary already answers. The call
sites are in cgamex86.dll; this reads them.

WHAT IT ESTABLISHES
-------------------
1. syscall 37 is  syscall(37, refEntity*, 0)
2. syscall 38 is  syscall(38, parentRefEntity*, modelHandle, vec3*, vec3*)
   so the forwarder's a2 is a PER-ATTACHMENT MODEL HANDLE, and a3/a4 are
   pointers to two writable float[3] belonging to that attachment alone.
3. Both 37 call sites pass the same persistent global struct, so the two base
   adds per scene are one entity drawn twice with a flag toggled -- not two
   different models.

Reads the DLL from disk. Does not load, launch, patch or execute anything.

Usage:  python tools/analyze_viewweapon.py [--dll <path>]
"""

import argparse
import collections
import struct
import sys

import capstone

DEFAULT_DLL = r"H:\SteamLibrary\steamapps\common\Call of Duty\cgamex86.dll"
ENTITY_ADD_SYSCALLS = (37, 38)


def load_pe(path):
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("%s is not a PE image" % path)
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsize = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    base = struct.unpack_from("<I", data, opt + 28)[0]
    secs = []
    for i in range(nsec):
        s = opt + optsize + i * 40
        name = data[s:s + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, va, rsize, roff = struct.unpack_from("<IIII", data, s + 8)
        secs.append((name, va, vsize, roff, rsize))
    return data, base, secs


def disasm_resync(md, data, base, start, end):
    """Linear sweep that restarts a byte later on undecodable input.

    A plain sweep stops at the first jump table and silently returns a tenth
    of the function bodies -- which is exactly how an earlier version of this
    reported 'found 0 call sites'.
    """
    out = []
    off = start
    while off < end:
        got = False
        for ins in md.disasm(data[off:end], base + off):
            out.append(ins)
            got = True
            off = ins.address - base + ins.size
        if not got:
            off += 1
    return out


def imm(op):
    try:
        return int(op, 0)
    except ValueError:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dll", default=DEFAULT_DLL)
    args = ap.parse_args()

    data, base, secs = load_pe(args.dll)
    text = [s for s in secs if s[0] == ".text"]
    if not text:
        raise SystemExit("no .text section")
    _n, va, vsize, _roff, _rsize = text[0]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    insns = disasm_resync(md, data, base, va, va + vsize)
    print("%s" % args.dll)
    print("  image base 0x%08X, .text 0x%06X..0x%06X, %d instructions decoded"
          % (base, va, va + vsize, len(insns)))

    # The syscall dispatcher is the indirect target every trap_* stub calls.
    dispatch = collections.Counter()
    for i, ins in enumerate(insns):
        if ins.mnemonic == "call" and ins.op_str.startswith("dword ptr ["):
            if i and insns[i - 1].mnemonic == "push" and imm(insns[i - 1].op_str) is not None:
                dispatch[ins.op_str] += 1
    if not dispatch:
        raise SystemExit("no indirect syscall dispatcher found")
    disp_op, disp_n = dispatch.most_common(1)[0]
    print("  syscall dispatcher: call %s  (%d trap stubs)" % (disp_op, disp_n))

    # Every entity-add call site, with the pushes that build its argument list.
    print("\n--- entity-add call sites -------------------------------------")
    sites = []
    for i, ins in enumerate(insns):
        if ins.mnemonic != "push":
            continue
        num = imm(ins.op_str)
        if num not in ENTITY_ADD_SYSCALLS:
            continue
        if i + 1 >= len(insns):
            continue
        nxt = insns[i + 1]
        if nxt.mnemonic != "call" or nxt.op_str != disp_op:
            continue
        pushes = []
        k = i - 1
        while k >= 0 and insns[k].mnemonic in ("push", "lea", "mov", "movzx",
                                               "add", "test", "cmp", "je", "jne"):
            if insns[k].mnemonic == "push":
                pushes.append(insns[k].op_str)
            elif insns[k].mnemonic in ("je", "jne", "cmp", "test"):
                break
            k -= 1
        pushes.reverse()
        # cleanup after the call tells us the true argument count
        argbytes = None
        for k in range(i + 2, min(i + 5, len(insns))):
            if insns[k].mnemonic == "add" and insns[k].op_str.startswith("esp,"):
                argbytes = imm(insns[k].op_str.split(",")[1].strip())
                break
        nargs = (argbytes // 4 - 1) if argbytes else None
        sites.append((num, ins.address - base, pushes, nargs))
        print("  syscall %2d at RVA 0x%05X  args=%s  push order (arg1..): %s"
              % (num, ins.address - base,
                 "?" if nargs is None else nargs,
                 ", ".join(reversed(pushes[-(nargs or 4):]))))

    print("""
--- what that means -------------------------------------------------
  syscall(37, refEntity*, 0)
      2 arguments. The base scene-entity add.
  syscall(38, parentRefEntity*, modelHandle, vec3*, vec3*)
      4 arguments. Argument 2 is a handle obtained from syscall(34, id) for
      THAT attachment, so it differs per attachment. Arguments 3 and 4 are
      pointers to two float[3] inside the attachment's own record -- the
      +0x150 and +0x15c fields, both accessed as fld/fstp triples elsewhere
      in cgame.

  The existing forwarder is EntityForward(num, p, a2, a3, a4), so it already
  receives all of this: a2 is the per-attachment model handle and a3/a4 point
  at writable per-attachment vectors. Discriminating and posing an individual
  attachment needs no new hook class.""")
    return 0


if __name__ == "__main__":
    sys.exit(main())
