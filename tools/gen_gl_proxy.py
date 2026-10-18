"""Generate forwarding thunks + .def for an opengl32.dll proxy.

Reads the export table of the real 32-bit opengl32.dll and emits:
  src/gl_thunks.inc  - one FARPROC per export + a naked jmp thunk for each
  src/opengl32.def   - EXPORTS mapping the original name/ordinal to our thunk

Ordinals are preserved because callers may import by ordinal.
"""

import struct
import sys
from pathlib import Path

REAL_DLL = Path(r"C:\Windows\SysWOW64\opengl32.dll")
SRC = Path(__file__).resolve().parent.parent / "src"

# Exports we implement by hand in dllmain.cpp instead of blindly forwarding.
# The .def points these at i_<name>; p_<name> still holds the real address so
# the interceptor can chain through to the driver.
INTERCEPT = {
    # Scaled so the engine's own screen coordinates land correctly in the
    # supersampled offscreen target we redirect rendering into.
    "glViewport",
    "glScissor",
    # GL_BACK is not a legal buffer for a framebuffer object, and the engine
    # names it every frame; translated to the colour attachment while our
    # offscreen target is bound, or rendering would simply stop.
    "glDrawBuffer",
    "glReadBuffer",
    # Caught so the game's vsync request can be forced off: alternating eyes
    # means a monitor-synced frame rate halves what each eye actually gets.
    "wglGetProcAddress",
    "wglSwapBuffers",
    "wglCreateContext",
    "wglMakeCurrent",
    "wglDeleteContext",
    # The projection-stack watch: the engine's 3D perspective load is replaced
    # with the headset eye's true asymmetric frustum (vr.cpp decides; these
    # wrappers only observe the matrix mode and divert projection loads).
    "glMatrixMode",
    "glLoadMatrixf",
    "glLoadMatrixd",
    "glFrustum",
    "glMultMatrixf",
    "glMultMatrixd",
    # GL pass classification: what kind of projection sits on the stack is
    # what tells the scene's glViewport/glScissor apart from the 2D
    # overlay's. The 2026-08-19 headset run proved the old syscall-time flag
    # classified the WHOLE scene as HUD (the engine executes its GL after
    # the render syscall returns), which squeezed the world into the shifted
    # 16:9 band -- so the pass boundary is now read off the stream itself:
    # ortho/identity loads open the overlay, perspective loads open the
    # scene, and push/pop keep a flare-style pushed ortho from ending it.
    "glOrtho",
    "glLoadIdentity",
    "glPushMatrix",
    "glPopMatrix",
    # ---- the render-surface layer (src/vr_glsurf.cpp) ----------------------
    # The first-person weapon and hands are taken at the draw call, AFTER the
    # engine has animated and skinned them. Two kinds of entry point are
    # needed: the draws themselves, and the state that identifies which draw
    # is which. None of the state wrappers query the driver -- they only
    # remember what the engine set, so an ordinary frame pays a switch.
    "glDrawElements",
    "glDrawArrays",
    "glVertexPointer",
    "glNormalPointer",
    "glTexCoordPointer",
    "glColorPointer",
    "glEnableClientState",
    "glDisableClientState",
    "glBindTexture",
    # Driver-supported anisotropy only at the game's mip-filter assignment;
    # non-mip UI and directly resolved VR texture allocations stay unchanged.
    "glTexParameteri",
    # glDepthRange is the discriminator that matters: id Tech 3 narrows it
    # around RF_DEPTHHACK geometry, which is the engine itself saying "this is
    # the first-person overlay". That is an engine semantic, not a "near the
    # camera" guess.
    "glDepthRange",
    "glDepthMask",
    "glDepthFunc",
    "glCullFace",
    "glFrontFace",
    "glBlendFunc",
    "glAlphaFunc",
    "glEnable",
    "glDisable",
}


def rva_to_off(sections, rva):
    for va, vsize, raw_off, raw_size in sections:
        if va <= rva < va + max(vsize, raw_size):
            return raw_off + (rva - va)
    raise ValueError(f"RVA 0x{rva:X} not in any section")


def parse_exports(path):
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe:pe + 4] == b"PE\0\0", "not a PE file"

    num_sections = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    assert magic == 0x10B, f"expected PE32 (x86), got magic 0x{magic:X}"

    # Optional header: data directories start at +96 for PE32
    exp_rva, exp_size = struct.unpack_from("<II", data, opt + 96)

    sec_off = opt + opt_size
    sections = []
    for i in range(num_sections):
        s = sec_off + i * 40
        vsize, va, raw_size, raw_off = struct.unpack_from("<IIII", data, s + 8)
        sections.append((va, vsize, raw_off, raw_size))

    e = rva_to_off(sections, exp_rva)
    ordinal_base = struct.unpack_from("<I", data, e + 16)[0]
    num_funcs, num_names = struct.unpack_from("<II", data, e + 20)
    addr_funcs, addr_names, addr_ords = struct.unpack_from("<III", data, e + 28)

    off_funcs = rva_to_off(sections, addr_funcs)
    off_names = rva_to_off(sections, addr_names)
    off_ords = rva_to_off(sections, addr_ords)

    def read_cstr(rva):
        o = rva_to_off(sections, rva)
        end = data.index(b"\0", o)
        return data[o:end].decode("ascii")

    exports = []
    for i in range(num_names):
        name_rva = struct.unpack_from("<I", data, off_names + i * 4)[0]
        ordinal = struct.unpack_from("<H", data, off_ords + i * 2)[0]
        func_rva = struct.unpack_from("<I", data, off_funcs + ordinal * 4)[0]
        # A func_rva inside the export directory means a forwarder string.
        forwarded = exp_rva <= func_rva < exp_rva + exp_size
        exports.append((read_cstr(name_rva), ordinal + ordinal_base, forwarded))

    exports.sort(key=lambda x: x[1])
    return exports


def main():
    if not REAL_DLL.exists():
        sys.exit(f"missing {REAL_DLL}")

    exports = parse_exports(REAL_DLL)
    fwd = sum(1 for _, _, f in exports if f)
    print(f"{REAL_DLL.name}: {len(exports)} named exports ({fwd} forwarded)")

    SRC.mkdir(parents=True, exist_ok=True)

    inc = [
        "// GENERATED by tools/gen_gl_proxy.py -- do not edit.",
        f"// Source: {REAL_DLL}  ({len(exports)} exports)",
        "",
        "#define GL_PROXY_EXPORT_COUNT %d" % len(exports),
        "",
        "// One resolved pointer per export.",
    ]
    for name, _, _ in exports:
        inc.append(f"static FARPROC p_{name};")

    inc += [
        "",
        "// Table used by ResolveRealExports().",
        "static const char* const kExportNames[GL_PROXY_EXPORT_COUNT] = {",
    ]
    inc += [f'    "{name}",' for name, _, _ in exports]
    inc += [
        "};",
        "",
        "static FARPROC* const kExportSlots[GL_PROXY_EXPORT_COUNT] = {",
    ]
    inc += [f"    &p_{name}," for name, _, _ in exports]
    inc += [
        "};",
        "",
        "// Naked thunks: jump straight through the resolved pointer.",
        "// No prologue means the caller's stack/registers pass through untouched,",
        "// which keeps every calling convention working without knowing signatures.",
    ]
    for name, _, _ in exports:
        if name in INTERCEPT:
            inc.append(f"// {name}: intercepted by hand as i_{name} (see dllmain.cpp)")
            continue
        inc.append(
            f'extern "C" __declspec(naked) void t_{name}() {{ __asm {{ jmp dword ptr [p_{name}] }} }}'
        )

    (SRC / "gl_thunks.inc").write_text("\n".join(inc) + "\n", encoding="ascii")

    d = ["; GENERATED by tools/gen_gl_proxy.py -- do not edit.", "LIBRARY opengl32", "EXPORTS"]
    for name, ordinal, _ in exports:
        prefix = "i_" if name in INTERCEPT else "t_"
        d.append(f"    {name}={prefix}{name} @{ordinal}")
    (SRC / "opengl32.def").write_text("\n".join(d) + "\n", encoding="ascii")

    missing = INTERCEPT - {n for n, _, _ in exports}
    if missing:
        print(f"WARNING: intercept targets not exported by the real DLL: {sorted(missing)}")
    print(f"intercepted by hand: {sorted(INTERCEPT - missing)}")

    print(f"wrote {SRC / 'gl_thunks.inc'}")
    print(f"wrote {SRC / 'opengl32.def'}")


if __name__ == "__main__":
    main()
