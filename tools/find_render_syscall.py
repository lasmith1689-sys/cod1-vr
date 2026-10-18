"""Decide which cgame syscall is R_RenderScene by disassembling cgamex86.dll.

Both syscall 71 and 243 appeared to receive the refdef pointer at runtime, but
that reading is unreliable: for a zero-argument syscall the "first argument" we
sampled is just whatever was left on the stack, which can easily be a refdef
pointer pushed moments earlier.

Arity settles it. Quake 3's trap wrappers compile to:

    push <arg>            ; only when the call takes one
    push <syscall number>
    call [g_syscall]
    add  esp, N           ; N = 4 * (1 + argument count)

so N == 8 means one argument (a refdef -> R_RenderScene) and N == 4 means none
(-> R_ClearScene). The module is not packed, so this is readable statically.

Anchors on the indirect call through the syscall pointer rather than on a
`push imm`, because the compiler is free to materialise the number in other
ways. Disassembly restarts after invalid bytes so the whole section is covered
-- a single linear pass stops at the first bad opcode and silently misses most
of the code.
"""

import struct
from collections import defaultdict
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_32

GAME = Path(r"H:\SteamLibrary\steamapps\common\Call of Duty")
MODULE = GAME / "cgamex86.dll"
SYSCALL_RVA = 0x657E8
CANDIDATES = (71, 243)


def load_text(path):
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    base = struct.unpack_from("<I", data, opt + 28)[0]
    for i in range(nsec):
        s = opt + optsz + i * 40
        name = data[s:s + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, va, rawsz, rawoff = struct.unpack_from("<IIII", data, s + 8)
        if name == ".text":
            return data[rawoff:rawoff + rawsz], base + va, base
    raise SystemExit("no .text")


def disasm_all(code, addr):
    """Linear sweep that resumes one byte past each invalid instruction."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    out = []
    off = 0
    while off < len(code):
        produced = 0
        for ins in md.disasm(code[off:], addr + off):
            out.append(ins)
            produced += ins.size
        off += produced + 1 if produced == 0 else produced
    return out


def main():
    code, text_va, base = load_text(MODULE)
    target = base + SYSCALL_RVA
    insns = disasm_all(code, text_va)
    print(f"{MODULE.name}: {len(insns)} instructions recovered from .text "
          f"({len(code)} bytes)")
    print(f"syscall pointer global: 0x{target:08X}\n")

    needle = f"0x{target:x}"
    results = defaultdict(list)
    call_sites = 0

    for n, ins in enumerate(insns):
        if ins.mnemonic != "call" or needle not in ins.op_str.lower():
            continue
        call_sites += 1

        # Walk back for the pushed syscall number.
        number = None
        for k in range(n - 1, max(-1, n - 8), -1):
            p = insns[k]
            if p.mnemonic == "push":
                try:
                    number = int(p.op_str.strip(), 0)
                except ValueError:
                    number = None
                break
            if p.mnemonic == "call":
                break

        # Walk forward for the stack cleanup.
        cleanup = None
        for k in range(n + 1, min(len(insns), n + 4)):
            a = insns[k]
            if a.mnemonic == "add" and a.op_str.replace(" ", "").startswith("esp,"):
                try:
                    cleanup = int(a.op_str.split(",")[1].strip(), 0)
                except ValueError:
                    pass
                break
            if a.mnemonic in ("ret", "call", "jmp"):
                break

        if number is not None:
            results[number].append(cleanup)

    print(f"indirect calls through the syscall pointer: {call_sites}")
    print(f"distinct syscall numbers resolved: {len(results)}\n")

    for cand in CANDIDATES:
        print(f"=== syscall {cand} ===")
        hits = results.get(cand)
        if not hits:
            print("  not found at any resolvable call site\n")
            continue
        for c in hits:
            args = (c // 4 - 1) if c else None
            print(f"  cleanup add esp,{c}"
                  f"{f'  -> {args} argument(s)' if args is not None else '  -> unknown'}")
        print()

    print("=== VERDICT ===")
    verdict = None
    for cand in CANDIDATES:
        vals = {c for c in results.get(cand, []) if c}
        if vals == {8}:
            print(f"  syscall {cand}: ONE argument -> receives the refdef -> R_RenderScene")
            verdict = cand
        elif vals == {4}:
            print(f"  syscall {cand}: NO arguments -> not the render call")
            print(f"                  (the refdef we sampled at runtime was stale stack)")
        elif vals:
            print(f"  syscall {cand}: inconsistent cleanups {sorted(vals)}")
        else:
            print(f"  syscall {cand}: undetermined")

    print()
    if verdict:
        print(f"  => renderSyscall {verdict}")
    else:
        print("  => inconclusive; runtime auto-detection decides")

    # Context: what else is called once per frame, for sanity.
    common = sorted(results.items(), key=lambda kv: -len(kv[1]))[:8]
    print("\nmost-referenced syscalls in cgame (number: call sites):")
    for num, lst in common:
        print(f"  {num:4d}: {len(lst)}")


if __name__ == "__main__":
    main()
