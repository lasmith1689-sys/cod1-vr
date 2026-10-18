// ===========================================================================
// CLIENT-ARRAY SAFETY -- the 2026-08-26 crash-to-desktop
// ===========================================================================
//
// WHAT HAPPENED
//
// The 2026-08-26 headset run crashed to desktop on the FIRST stereo scene,
// the first frame on which viewmodel draws exist at all. Windows Error
// Reporting:
//
//     Faulting module   opengl32.DLL   (our proxy)
//     Exception code    0xc0000005     (access violation)
//     Fault offset      0x00021c3a
//
// Disassembling the deployed binary at that offset lands inside
// SampledVertex(), on the instruction that dereferences the client vertex
// array:
//
//     add  edx, ds:[g_st.vertex.p]
//     mov  eax, dword ptr [edx]        <-- 0x00021c3a
//
// WHY IT FAULTED
//
// The engine does not hand us a CPU pointer. Every one of the 125 draws in
// the archived flat capture (runs/sweep-20260825) records a vertex pointer
// under 64 KB:
//
//     cls=gun       vertPtr=0x00000014
//     cls=gun       vertPtr=0x000001E0
//     cls=hand_L    vertPtr=0x000082C8
//     cls=sleeve_R  vertPtr=0x000012C0
//
// Those are BYTE OFFSETS into a bound vertex buffer object, not addresses.
// With a VBO bound, glVertexPointer's last argument stops being a pointer --
// and the first 64 KB of a Win32 address space is never mapped, so
// dereferencing one is a guaranteed access violation.
//
// We never saw the binds. The proxy intercepts glVertexPointer but not
// glBindBufferARB, and the engine resolves buffer-object entry points through
// wglGetProcAddress, so the layer had no way to know the "pointer" it was
// storing had changed meaning.
//
// WHY THE SUITE MISSED IT
//
// Every existing case calls EnterViewmodelPass(), which sets a real, valid
// array. The harness modelled a client-array engine because that is what we
// assumed the engine was. A test that only ever supplies a good pointer
// cannot discover that the code has no defence against a bad one.
//
// THE RULE THIS ENCODES
//
// The layer may not dereference a client array it cannot prove is
// dereferenceable. There is no GL query that reports the length of a client
// array, and none that reports whether a "pointer" is really an offset -- so
// the only safe posture is to READ NOTHING unless the address is plausible
// and the index is bounded, and to fail closed to "draw exactly what the
// engine asked for" when it is not.

// The real recorded values from the archived capture. Used verbatim so this
// case cannot drift away from what the engine actually does.
#define VBO_HAND_OFFSET   ((const void*)(uintptr_t)0x000082C8u)
#define VBO_GUN_OFFSET    ((const void*)(uintptr_t)0x00000014u)
#define VBO_SLEEVE_OFFSET ((const void*)(uintptr_t)0x000012C0u)

// Run a draw and report whether it access-violated. The whole point of this
// file is that a fault here is a RESULT, not a crash -- so it is caught and
// scored rather than allowed to take the suite down with it.
static int DrawFaulted(unsigned mode, int count, unsigned type, const void* idx)
{
    __try {
        GLSurf_OnDrawElements(mode, count, type, idx);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

static int DrawArraysFaulted(unsigned mode, int first, int count)
{
    __try {
        GLSurf_OnDrawArrays(mode, first, count);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

// Put the layer in the viewmodel pass, but with the array the engine really
// supplies: a buffer-object offset.
static void EnterViewmodelPassWithVBO(const void* offset, int stride)
{
    GLSurf_ScenePhase(1);
    GLSurf_NoteDepthRange(0.0, 0.2);
    GLSurf_NoteEnable(GLS_GL_BLEND, 0);
    GLSurf_NoteViewport(0, 0, 1440, 1440);
    GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, stride, offset);
    GLSurf_NoteNormalPointer(GLS_GL_FLOAT, stride, offset);
    GLSurf_NoteClientState(GLS_GL_NORMAL_ARRAY, 1);
    GLSurf_NoteClientState(GLS_GL_VERTEX_ARRAY, 1);
}

static void S_ClientArraySafety()
{
    printf("--- client-array safety: the 2026-08-26 crash ---\n");
    BuildSyntheticGeometry();

    // ---- 1: the exact crash, reproduced ---------------------------------
    //
    // THE PATH THAT KILLED THE GAME is the UNMERGED one. GLSurf_OnDrawElements
    // calls NoteClassPivot(cls, type, idx, count) unconditionally for every
    // non-merged draw, before any class or transform check -- so the gun batch
    // reaches SampledVertex on the first viewmodel frame whether or not
    // anything is being transformed. The merged hand path, by contrast, only
    // gets there when a split is actually wanted, which is why the first
    // version of this case passed against the broken code and proved nothing.
    {
        ResetLayerForCase();
        EnterViewmodelPassWithVBO(VBO_GUN_OFFSET, 32);
        const int faulted = DrawFaulted(GLS_GL_TRIANGLES, 324,
                                        GLS_GL_UNSIGNED_SHORT, g_handIdx);
        CHECK(!faulted,
              "an UNMERGED viewmodel draw from a VBO offset (0x14) does NOT "
              "access-violate -- the 2026-08-26 crash");
    }
    {
        // And the merged path, with a split actually requested.
        ResetLayerForCase();
        EnterViewmodelPassWithVBO(VBO_HAND_OFFSET, 0);
        float d[16]; MockTranslate(d, 4.0f, 0, 0);
        GLSurf_SetClassDelta(GLSC_HAND_R, d);
        const int faulted = DrawFaulted(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                                        GLS_GL_UNSIGNED_INT, g_handIdx);
        CHECK(!faulted,
              "the merged hand batch, split and transformed from a VBO offset "
              "(0x82C8), does NOT access-violate");
    }

    // ---- 2: every recorded offset, every index width --------------------
    {
        const void* offsets[] = { VBO_HAND_OFFSET, VBO_GUN_OFFSET, VBO_SLEEVE_OFFSET };
        const char* names[]   = { "0x82C8 (hand)", "0x14 (gun)", "0x12C0 (sleeve)" };
        const unsigned types[] = { GLS_GL_UNSIGNED_INT, GLS_GL_UNSIGNED_SHORT,
                                   GLS_GL_UNSIGNED_BYTE };
        const char* tnames[]   = { "uint", "ushort", "ubyte" };
        int faults = 0, cases = 0;
        for (int o = 0; o < 3; ++o)
            for (int t = 0; t < 3; ++t) {
                ResetLayerForCase();
                EnterViewmodelPassWithVBO(offsets[o], 0);
                ++cases;
                if (DrawFaulted(GLS_GL_TRIANGLES, 324, types[t], g_handIdx)) {
                    ++faults;
                    printf("        | FAULT: offset %s, %s indices\n",
                           names[o], tnames[t]);
                }
            }
        CHECK(faults == 0,
              "%d offset/index-width combinations, %d access violations",
              cases, faults);
    }

    // ---- 3: glDrawArrays takes the same path ----------------------------
    {
        ResetLayerForCase();
        EnterViewmodelPassWithVBO(VBO_GUN_OFFSET, 32);
        CHECK(!DrawArraysFaulted(GLS_GL_TRIANGLES, 0, 1428),
              "glDrawArrays from a VBO offset does NOT access-violate");
    }

    // ---- 4: a null array is refused, not read ---------------------------
    {
        ResetLayerForCase();
        EnterViewmodelPassWithVBO((const void*)0, 0);
        CHECK(!DrawFaulted(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                           GLS_GL_UNSIGNED_INT, g_handIdx),
              "a null vertex array does NOT access-violate");
    }

    // ---- 5: a valid array still works -----------------------------------
    //
    // The guard must not be a blanket refusal: with a real pointer the layer
    // has to behave exactly as before, or the fix has cost us the feature.
    {
        ResetLayerForCase();
        EnterViewmodelPass();                       // the real g_verts array
        float d[16]; MockTranslate(d, 4.0f, 0, 0);
        GLSurf_SetClassDelta(GLSC_HAND_R, d);
        const int owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES,
                                                GLS_HAND_INDICES * 2,
                                                GLS_GL_UNSIGNED_INT, g_handIdx);
        CHECK(owned == 1, "a real client array is still claimed and split");
        float piv[3];
        CHECK(GLSurf_ClassPivot(GLSC_HAND_R, piv) == 1,
              "and its pivot is still computed from real vertices");
    }

    // ---- 6: an index beyond the array is not chased ----------------------
    //
    // Even with a readable base pointer, an index the array does not contain
    // walks off the end. GL offers no way to ask how long a client array is,
    // so the only defence is a cap on how far a sample may reach.
    {
        ResetLayerForCase();
        EnterViewmodelPass();
        static unsigned wild[64];
        for (int i = 0; i < 64; ++i) wild[i] = 0x3FFFFFFFu - (unsigned)i;
        CHECK(!DrawFaulted(GLS_GL_TRIANGLES, 64, GLS_GL_UNSIGNED_INT, wild),
              "indices far beyond the array do NOT access-violate");
        GLSurfStats st; GLSurf_GetStats(&st);
        CHECK(st.rejectedNullPage + st.rejectedUnmapped + st.rejectedWildIndex > 0,
              "the refusals are counted by reason (nullPage %u, unmapped %u, "
              "wild %u)", st.rejectedNullPage, st.rejectedUnmapped,
              st.rejectedWildIndex);
    }
}

// ---------------------------------------------------------------------------

// A driver whose drawElements calls straight back into the interception --
// the wiring mistake the re-entrancy guard exists for. Installed only inside
// the mutant case below.
static int g_recurseDepthSeen;
static void WINAPI Mock_RecursiveDrawElements(unsigned mode, int count,
                                              unsigned type, const void* idx)
{
    if (++g_recurseDepthSeen > 64) return;      // stop the test, not the layer
    GLSurf_OnDrawElements(mode, count, type, idx);
}

static void S_DrawReentrancy()
{
    printf("--- GL re-entrancy: a re-issued draw cannot come back in ---\n");
    BuildSyntheticGeometry();

    // ---- production wiring never re-enters ------------------------------
    {
        ResetLayerForCase();
        EnterViewmodelPass();
        float d[16]; MockTranslate(d, 4.0f, 0, 0);
        GLSurf_SetClassDelta(GLSC_HAND_R, d);
        GLSurf_OnDrawElements(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                              GLS_GL_UNSIGNED_INT, g_handIdx);
        CHECK(GLSurf_ReentryCount() == 0,
              "a split, transformed batch re-enters the layer 0 times");
        CHECK(CountKind(REC_DRAW) == 2,
              "the split produced exactly TWO sub-draws, not a recursive "
              "expansion (%d)", CountKind(REC_DRAW));
    }

    // ---- MUTANT: a driver wired back to the interception ----------------
    //
    // If dllmain ever filled GLSurfDriver with the i_gl* wrappers instead of
    // the real entry points, every re-issued sub-draw would come back through
    // here. Without the depth guard that is an unbounded recursion and the
    // stack runs out; with it the re-entry is declined and counted.
    {
        ResetLayerForCase();
        GLSurfDriver bad;
        memset(&bad, 0, sizeof(bad));
        bad.drawElements = Mock_RecursiveDrawElements;   // the wiring mistake
        bad.drawArrays   = Mock_DrawArrays;
        bad.loadMatrixf  = Mock_LoadMatrixf;
        bad.matrixMode   = Mock_MatrixMode;
        bad.getFloatv    = Mock_GetFloatv;
        bad.getIntegerv  = Mock_GetIntegerv;
        bad.getDoublev   = Mock_GetDoublev;
        bad.isEnabled    = Mock_IsEnabled;
        bad.getTexLevelParameteriv = Mock_GetTexLevel;
        bad.vertexPointer = Mock_VertexPointer;
        bad.normalPointer = Mock_NormalPointer;
        GLSurf_SetDriver(&bad);
        g_recurseDepthSeen = 0;

        EnterViewmodelPass();
        float d[16]; MockTranslate(d, 4.0f, 0, 0);
        GLSurf_SetClassDelta(GLSC_HAND_R, d);
        GLSurf_OnDrawElements(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                              GLS_GL_UNSIGNED_INT, g_handIdx);

        CHECK(g_recurseDepthSeen <= 4,
              "MUTANT: a self-calling driver bottoms out after %d calls, not "
              "64 -- the guard held", g_recurseDepthSeen);
        CHECK(GLSurf_ReentryCount() > 0,
              "MUTANT: the re-entry was COUNTED (%u), not silently absorbed",
              GLSurf_ReentryCount());
        ResetLayerForCase();                    // put the real mock back
    }
}

// ---------------------------------------------------------------------------

// The engine-shaped upload: a buffer whose bytes at offset 0x82C8 are the
// merged hand vertices, exactly as the flat captures record the layout.
// Shared by the soak below and the S_VboShadow suite further down.
enum { VBOT_HAND_OFF = 0x82C8, VBOT_STRIDE = 12, VBOT_VERTS = GLS_HAND_VERTS * 2 };
enum { VBOT_SIZE = VBOT_HAND_OFF + VBOT_VERTS * VBOT_STRIDE };
enum { VBOT_ARRAY_ID = 7, VBOT_ELEM_ID = 9 };

static unsigned char g_vboStage[VBOT_SIZE];

// Upload the synthetic hand geometry into shadow buffer VBOT_ARRAY_ID at the
// recorded offset, through the same note calls dllmain's wrappers make.
static void UploadHandVbo()
{
    memset(g_vboStage, 0, sizeof(g_vboStage));
    memcpy(g_vboStage + VBOT_HAND_OFF, g_verts, VBOT_VERTS * VBOT_STRIDE);
    VBO_NoteBindBuffer(VBO_GL_ARRAY_BUFFER, VBOT_ARRAY_ID);
    VBO_NoteBufferData(VBO_GL_ARRAY_BUFFER, VBOT_SIZE, g_vboStage, 0x88E4);
}

// The viewmodel pass as the REAL engine enters it: buffer bound, pointer is
// the byte offset. The bind must precede the pointer call -- GL latches the
// binding then, and so does the layer.
static void EnterViewmodelPassVboBacked()
{
    GLSurf_ScenePhase(1);
    GLSurf_NoteDepthRange(0.0, 0.2);
    GLSurf_NoteEnable(GLS_GL_BLEND, 0);
    GLSurf_NoteViewport(0, 0, 1440, 1440);
    GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, VBOT_STRIDE,
                             (const void*)(uintptr_t)VBOT_HAND_OFF);
    GLSurf_NoteClientState(GLS_GL_VERTEX_ARRAY, 1);
}

static void S_ClientArraySoak()
{
    printf("--- soak: 10k scene frames of hostile client arrays ---\n");
    BuildSyntheticGeometry();
    ResetLayerForCase();

    // Every array shape the engine has ever been observed to present, plus
    // the ones that would hurt: buffer offsets, a null, a wild pointer, and
    // the real thing.
    const void* arrays[] = {
        g_verts,                                    // a genuine client array
        VBO_HAND_OFFSET, VBO_GUN_OFFSET, VBO_SLEEVE_OFFSET,
        (const void*)0,
        (const void*)(uintptr_t)0xDEADBEEFu,        // mapped nowhere
        (const void*)(uintptr_t)0xFFFFFFF0u,        // wraps on any offset
    };
    const unsigned types[] = { GLS_GL_UNSIGNED_INT, GLS_GL_UNSIGNED_SHORT,
                               GLS_GL_UNSIGNED_BYTE };
    const int counts[] = { 324, 36, 750, 1428, GLS_HAND_INDICES * 2 };
    const int strides[] = { 0, 12, 32 };

    int faults = 0;
    const int kFrames = 10000;
    for (int f = 0; f < kFrames; ++f) {
        // A new scene frame: this is what the game does between eyes.
        if ((f % 7) == 0) { GLSurf_BeginFrame(); GLSurf_SceneQueued(f & 1); }

        const void* arr = arrays[f % 7];
        const int stride = strides[f % 3];
        GLSurf_ScenePhase(1);
        GLSurf_NoteDepthRange(0.0, 0.2);
        GLSurf_NoteViewport(0, 0, 1440, 1440);

        // Buffer-object churn: the engine's bind/upload/delete lifecycle
        // interleaved with the hostile arrays. When a bind is active, the
        // rotating "arrays" above become OFFSETS -- some resolving into the
        // mirror, some out of range, some against deleted or unknown ids.
        // Every combination must refuse or resolve; none may fault.
        switch (f % 13) {
            case 0:  UploadHandVbo(); break;                          // fresh mirror
            case 4:  VBO_NoteBindBuffer(VBO_GL_ARRAY_BUFFER, 999u); break; // unknown id
            case 6:  { float j[2] = { 1.0f, 2.0f };
                       VBO_NoteBufferSubData(VBO_GL_ARRAY_BUFFER, 0, sizeof(j), j); }
                     break;
            case 8:  { const unsigned dead = VBOT_ARRAY_ID;
                       VBO_NoteDeleteBuffers(1, &dead); } break;
            case 11: VBO_NoteBindBuffer(VBO_GL_ARRAY_BUFFER, 0); break;    // client again
            default: break;
        }

        GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, stride, arr);
        GLSurf_NoteNormalPointer(GLS_GL_FLOAT, stride, arr);
        GLSurf_NoteClientState(GLS_GL_VERTEX_ARRAY, 1);
        GLSurf_NoteClientState(GLS_GL_NORMAL_ARRAY, 1);

        // Weapon switch: the batch size changes under the layer.
        const int count = counts[f % 5];

        // Support attach / reload enter+cancel / tracking loss all reduce, at
        // this layer, to a hand target appearing and disappearing between
        // draws. Churn it every frame so no state can be assumed stable.
        if ((f % 3) == 0) {
            const float off[3] = { 0.01f * (f % 11), -0.02f, 0.03f };
            const float rot[9] = { 1,0,0, 0,1,0, 0,0,1 };
            GLSurf_SetHandTarget(GLSC_HAND_R, 1, off, rot);
            GLSurf_SetHandPairEligible(GLSC_HAND_R, 1);   // wiring's job
            GLSurf_SetHandTarget(GLSC_HAND_L, (f % 6) == 0, off, rot);
        } else {
            GLSurf_ClearHandTargets();
        }
        if ((f % 5) == 0) {
            float d[16]; MockTranslate(d, 0.1f * (f % 9), 0, 0);
            GLSurf_SetClassDelta(GLSC_GUN, d);
        } else if ((f % 5) == 1) {
            GLSurf_ClearAllDeltas();
        }

        if (DrawFaulted(GLS_GL_TRIANGLES, count, types[f % 3], g_handIdx))
            ++faults;
        if ((f % 11) == 0 && DrawArraysFaulted(GLS_GL_TRIANGLES, 0, count))
            ++faults;
        if (faults > 3) break;                  // no point running 10k of these
    }

    GLSurfStats st;
    GLSurf_GetStats(&st);
    CHECK(faults == 0, "%d frames, %d access violations", kFrames, faults);
    CHECK(GLSurf_ReentryCount() == 0,
          "%d frames, 0 re-entries (%u)", kFrames, GLSurf_ReentryCount());
    CHECK(st.rejectedNullPage + st.rejectedUnmapped + st.rejectedWildIndex > 0,
          "the hostile arrays were REFUSED, not read (nullPage %u, unmapped %u, "
          "wild %u)", st.rejectedNullPage, st.rejectedUnmapped, st.rejectedWildIndex);
    CHECK(st.probeCacheHits > st.probeQueries,
          "the probe cache carries the load: %u hits vs %u VirtualQuery calls",
          st.probeCacheHits, st.probeQueries);
    ResetLayerForCase();
}

// ===========================================================================
// BUFFER-OBJECT SHADOW (vr_vbo) -- the way BACK to the geometry
// ===========================================================================
//
// Refusing VBO offsets keeps the process alive but leaves every vertex-reading
// feature inert on the real engine, which draws the viewmodel from buffer
// objects. The shadow mirrors the engine's own uploads as they pass through
// the interception, and the resolvers read the mirror. These cases feed the
// engine's REAL recorded offsets through the production draw path with a
// shadow in place, and require the features to WORK -- then take the shadow
// away piece by piece and require the refusals back.

static void S_VboShadow()
{
    printf("--- vbo shadow: the engine's offsets resolve to mirrored data ---\n");
    BuildSyntheticGeometry();

    // ---- 1: the crash case, WORKING -------------------------------------
    //
    // Same offset that killed the process, same draw -- but with the engine's
    // upload mirrored, the batch must now be claimed, split and given a
    // pivot, with zero null-page refusals. This is the difference between
    // "does not crash" and "the feature is back".
    {
        ResetLayerForCase();
        UploadHandVbo();
        EnterViewmodelPassVboBacked();
        float d[16]; MockTranslate(d, 4.0f, 0, 0);
        GLSurf_SetClassDelta(GLSC_HAND_R, d);
        const int faulted = DrawFaulted(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                                        GLS_GL_UNSIGNED_INT, g_handIdx);
        CHECK(!faulted, "the 0x82C8 merged hand draw, VBO-backed, does not fault");
        CHECK(CountKind(REC_DRAW) == 2,
              "and is claimed and SPLIT into two sub-draws (%d) -- the shadow "
              "made the midpoint proof possible again", CountKind(REC_DRAW));
        float piv[3];
        CHECK(GLSurf_ClassPivot(GLSC_HAND_R, piv) == 1,
              "and the pivot is recovered from SHADOW vertices");
        GLSurfStats st; GLSurf_GetStats(&st);
        CHECK(st.vboVertResolves > 0 && st.rejectedNullPage == 0,
              "reads came from the shadow (%u resolves), not refusals "
              "(nullPage %u)", st.vboVertResolves, st.rejectedNullPage);
    }

    // ---- 1b: AN ORPHANED BUFFER IS NOT GEOMETRY -------------------------
    //
    // The 2026-08-28 hardware run refused ~50% of real hand solves at the
    // SOLVER site with a constant 0.0455 m residual while vboMiss read 0.
    // "vboMiss 0" was taken to mean sourcing was healthy. It did not: an
    // orphaned store resolved SUCCESSFULLY to a zero fill, so the solver got
    // 48 identical points, produced an arbitrary rotation, and refused at a
    // residual that is a property of the LANDMARK TABLE rather than of any
    // geometry -- a number that cannot be told apart from a real mismatch.
    //
    // The contract now: an orphan is a REFUSAL until the engine writes, and
    // the refusal is attributed. This is the case that would have named the
    // fault in one line instead of a run.
    {
        ResetLayerForCase();
        // The engine's per-frame streaming idiom: orphan, then refill.
        VBO_NoteBindBuffer(VBO_GL_ARRAY_BUFFER, VBOT_ARRAY_ID);
        VBO_NoteBufferData(VBO_GL_ARRAY_BUFFER, VBOT_SIZE, nullptr, 0x88E8);
        EnterViewmodelPassVboBacked();
        float d[16]; MockTranslate(d, 4.0f, 0, 0);
        GLSurf_SetClassDelta(GLSC_HAND_R, d);
        CHECK(!DrawFaulted(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                           GLS_GL_UNSIGNED_INT, g_handIdx),
              "a draw against a freshly ORPHANED buffer does not fault");
        float piv[3];
        CHECK(GLSurf_ClassPivot(GLSC_HAND_R, piv) == 0,
              "and yields NO pivot -- undefined bytes are not a point cloud");
        {
            GLSurfStats st; GLSurf_GetStats(&st);
            VBOStats v; VBO_GetStats(&v);
            CHECK(st.vboVertMisses > 0 && v.resolveMissUndefined > 0
                  && v.resolveHits == 0,
                  "the refusal is COUNTED and attributed to undefined content "
                  "(vertMiss %u, missUndefined %u, hits %u)",
                  st.vboVertMisses, v.resolveMissUndefined, v.resolveHits);
        }

        // ...and the moment the engine actually fills it, the same draw works.
        memset(g_vboStage, 0, sizeof(g_vboStage));
        memcpy(g_vboStage + VBOT_HAND_OFF, g_verts, VBOT_VERTS * VBOT_STRIDE);
        VBO_NoteBufferSubData(VBO_GL_ARRAY_BUFFER, VBOT_HAND_OFF,
                              VBOT_VERTS * VBOT_STRIDE,
                              g_vboStage + VBOT_HAND_OFF);
        RecReset();
        CHECK(!DrawFaulted(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                           GLS_GL_UNSIGNED_INT, g_handIdx)
              && CountKind(REC_DRAW) == 2,
              "after the refill the same batch is claimed and split again");
        CHECK(GLSurf_ClassPivot(GLSC_HAND_R, piv) == 1,
              "and the pivot comes back from REAL written vertices");
    }

    // ---- 1c: a partial refill vouches only for what was written ---------
    {
        ResetLayerForCase();
        VBO_NoteBindBuffer(VBO_GL_ARRAY_BUFFER, VBOT_ARRAY_ID);
        VBO_NoteBufferData(VBO_GL_ARRAY_BUFFER, VBOT_SIZE, nullptr, 0x88E8);
        // Write only the FIRST half of the hand span.
        const size_t halfBytes = (size_t)VBOT_VERTS * VBOT_STRIDE / 2;
        memset(g_vboStage, 0, sizeof(g_vboStage));
        memcpy(g_vboStage + VBOT_HAND_OFF, g_verts, VBOT_VERTS * VBOT_STRIDE);
        VBO_NoteBufferSubData(VBO_GL_ARRAY_BUFFER, VBOT_HAND_OFF,
                              (int)halfBytes, g_vboStage + VBOT_HAND_OFF);
        CHECK(VBO_ResolveRange(VBOT_ARRAY_ID, VBOT_HAND_OFF, halfBytes) != nullptr,
              "the written half resolves");
        CHECK(VBO_ResolveRange(VBOT_ARRAY_ID, VBOT_HAND_OFF,
                               halfBytes * 2) == nullptr,
              "a span reaching past the written run REFUSES -- half real "
              "vertices and half undefined is not a hand");
    }

    // ---- 2: no shadow, refusal is back ----------------------------------
    //
    // The delta matters: a merged batch nobody asked to transform reads no
    // vertex at all (that is the cost-nothing path), so the refusal counters
    // only tick when a read was actually attempted.
    {
        ResetLayerForCase();
        VBO_NoteBindBuffer(VBO_GL_ARRAY_BUFFER, 31);   // an id never uploaded
        EnterViewmodelPassVboBacked();
        float d[16]; MockTranslate(d, 4.0f, 0, 0);
        GLSurf_SetClassDelta(GLSC_HAND_R, d);
        CHECK(!DrawFaulted(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                           GLS_GL_UNSIGNED_INT, g_handIdx),
              "an unmirrored buffer id refuses instead of faulting");
        GLSurfStats st; GLSurf_GetStats(&st);
        VBOStats v; VBO_GetStats(&v);
        CHECK(st.vboVertMisses > 0 && v.resolveMissUnknown > 0,
              "and the miss is counted by reason (vert %u, unknown-id %u)",
              st.vboVertMisses, v.resolveMissUnknown);
    }

    // ---- 3: BufferSubData is mirrored -----------------------------------
    {
        ResetLayerForCase();
        UploadHandVbo();
        const unsigned genBefore = VBO_Generation(VBOT_ARRAY_ID);
        float moved[3] = { 123.0f, -45.0f, 6.0f };
        VBO_NoteBufferSubData(VBO_GL_ARRAY_BUFFER, VBOT_HAND_OFF,
                              sizeof(moved), moved);
        const float* r = (const float*)VBO_ResolveRange(VBOT_ARRAY_ID,
                                                        VBOT_HAND_OFF, 12);
        CHECK(r && r[0] == 123.0f && r[1] == -45.0f,
              "a SubData range lands in the shadow verbatim");
        CHECK(VBO_Generation(VBOT_ARRAY_ID) == genBefore + 1,
              "and bumps the buffer's generation");
    }

    // ---- 4: the Map/Unmap window is mirrored at Unmap time --------------
    {
        ResetLayerForCase();
        VBO_NoteBindBuffer(VBO_GL_ARRAY_BUFFER, VBOT_ARRAY_ID);
        VBO_NoteBufferData(VBO_GL_ARRAY_BUFFER, VBOT_SIZE, nullptr, 0x88E8);
        // THE SILENT-WRONG-DATA GUARD.
        //
        // This used to assert the opposite -- that an orphan left a "defined,
        // zeroed shadow" that resolved successfully. GL says the content is
        // UNDEFINED after BufferData(NULL), so vouching for those zeros hands
        // the caller an invented point cloud. Downstream that is not a crash
        // and not a miss: the palm solve fits its bind cloud onto 48 copies of
        // (0,0,0), refuses at a CONSTANT residual equal to the bind cloud's own
        // radius of gyration (0.0448 m for the shipped trigger table), and
        // vboMiss reads 0 the whole time -- indistinguishable in a log from a
        // real geometry mismatch. The resolve must REFUSE until something
        // writes.
        const float* r0 = (const float*)VBO_ResolveRange(VBOT_ARRAY_ID,
                                                         VBOT_HAND_OFF, 12);
        CHECK(r0 == nullptr,
              "an orphaning BufferData(NULL) leaves the store UNDEFINED -- the "
              "shadow refuses to vouch for it rather than serving zeros");
        {
            VBOStats vo; VBO_GetStats(&vo);
            CHECK(vo.orphans == 1 && vo.resolveMissUndefined == 1,
                  "and the refusal is COUNTED as undefined-content, not as a "
                  "hit (orphans %u, missUndefined %u)",
                  vo.orphans, vo.resolveMissUndefined);
        }

        // The engine maps, writes into the driver's window, unmaps. The
        // staging array below IS that window as far as the mirror is told.
        memset(g_vboStage, 0, sizeof(g_vboStage));
        memcpy(g_vboStage + VBOT_HAND_OFF, g_verts, VBOT_VERTS * VBOT_STRIDE);
        VBO_NoteMapBuffer(VBO_GL_ARRAY_BUFFER, 0x88B9 /*WRITE_ONLY*/, g_vboStage);
        VBO_NoteUnmapBuffer(VBO_GL_ARRAY_BUFFER);

        const float* r1 = (const float*)VBO_ResolveRange(VBOT_ARRAY_ID,
                                                         VBOT_HAND_OFF, 12);
        CHECK(r1 && r1[0] == g_verts[0] && r1[1] == g_verts[1],
              "Unmap syncs the mapped content into the shadow");
        VBOStats v; VBO_GetStats(&v);
        CHECK(v.unmapCopies == 1, "exactly one whole-buffer sync (%u)",
              v.unmapCopies);
        CHECK(r1 != (const float*)(g_vboStage + VBOT_HAND_OFF),
              "and the resolved pointer is CODVR shadow memory, not the "
              "driver's mapping");

        // The feature works off the map-filled shadow too.
        EnterViewmodelPassVboBacked();
        float d[16]; MockTranslate(d, 4.0f, 0, 0);
        GLSurf_SetClassDelta(GLSC_HAND_R, d);
        CHECK(!DrawFaulted(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                           GLS_GL_UNSIGNED_INT, g_handIdx)
              && CountKind(REC_DRAW) == 2,
              "a map-uploaded viewmodel batch is claimed and split");
    }

    // ---- 5: delete invalidates ------------------------------------------
    {
        ResetLayerForCase();
        UploadHandVbo();
        EnterViewmodelPassVboBacked();
        const unsigned dead = VBOT_ARRAY_ID;
        VBO_NoteDeleteBuffers(1, &dead);
        CHECK(!DrawFaulted(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                           GLS_GL_UNSIGNED_INT, g_handIdx),
              "a draw against a DELETED buffer refuses, never reads freed "
              "shadow");
        CHECK(VBO_ResolveRange(VBOT_ARRAY_ID, 0, 4) == nullptr,
              "and the id no longer resolves");
    }

    // ---- 6: offsets beyond the buffer are misses, not reads -------------
    {
        ResetLayerForCase();
        UploadHandVbo();
        GLSurf_ScenePhase(1);
        GLSurf_NoteDepthRange(0.0, 0.2);
        GLSurf_NoteViewport(0, 0, 1440, 1440);
        GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, VBOT_STRIDE,
                                 (const void*)(uintptr_t)(VBOT_SIZE + 64));
        GLSurf_NoteClientState(GLS_GL_VERTEX_ARRAY, 1);
        float d[16]; MockTranslate(d, 4.0f, 0, 0);
        GLSurf_SetClassDelta(GLSC_HAND_R, d);        // force a read attempt
        CHECK(!DrawFaulted(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                           GLS_GL_UNSIGNED_INT, g_handIdx),
              "an offset past the buffer end refuses cleanly");
        VBOStats v; VBO_GetStats(&v);
        CHECK(v.resolveMissRange > 0, "counted as a RANGE miss (%u)",
              v.resolveMissRange);
    }

    // ---- 7: a SubData the model cannot hold poisons the buffer ----------
    //
    // If the engine writes past what BufferData declared, our model of that
    // buffer is wrong -- so the whole shadow stops vouching, rather than
    // serving bytes that may be half-right.
    {
        ResetLayerForCase();
        UploadHandVbo();
        float junk[4] = { 1, 2, 3, 4 };
        VBO_NoteBufferSubData(VBO_GL_ARRAY_BUFFER, VBOT_SIZE - 4,
                              sizeof(junk), junk);
        CHECK(VBO_ResolveRange(VBOT_ARRAY_ID, 0, 4) == nullptr,
              "a SubData past the declared size marks the buffer STALE");
        EnterViewmodelPassVboBacked();
        CHECK(!DrawFaulted(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                           GLS_GL_UNSIGNED_INT, g_handIdx),
              "and later draws refuse instead of reading a wrong mirror");
    }

    // ---- 8: element-buffer indices resolve through the shadow -----------
    {
        ResetLayerForCase();
        UploadHandVbo();
        VBO_NoteBindBuffer(VBO_GL_ELEMENT_ARRAY_BUFFER, VBOT_ELEM_ID);
        VBO_NoteBufferData(VBO_GL_ELEMENT_ARRAY_BUFFER, sizeof(g_handIdx),
                           g_handIdx, 0x88E4);
        EnterViewmodelPassVboBacked();
        float d[16]; MockTranslate(d, 4.0f, 0, 0);
        GLSurf_SetClassDelta(GLSC_HAND_R, d);
        // The index "pointer" is now offset 0 into the element buffer.
        const int faulted = DrawFaulted(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                                        GLS_GL_UNSIGNED_INT, (const void*)0);
        CHECK(!faulted, "element-buffer indices at offset 0 do not fault");
        CHECK(CountKind(REC_DRAW) == 2,
              "and the batch still splits -- indices came from the shadow (%d)",
              CountKind(REC_DRAW));
        GLSurfStats st; GLSurf_GetStats(&st);
        CHECK(st.vboIdxResolves > 0, "index resolves are counted (%u)",
              st.vboIdxResolves);

        // Element buffer bound but never uploaded: refuse, don't read.
        ResetLayerForCase();
        UploadHandVbo();
        VBO_NoteBindBuffer(VBO_GL_ELEMENT_ARRAY_BUFFER, 77);
        EnterViewmodelPassVboBacked();
        GLSurf_SetClassDelta(GLSC_HAND_R, d);
        CHECK(!DrawFaulted(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                           GLS_GL_UNSIGNED_INT, (const void*)0),
              "an unmirrored element buffer refuses instead of faulting");
        GLSurf_GetStats(&st);
        CHECK(st.vboIdxMisses > 0, "and the index miss is counted (%u)",
              st.vboIdxMisses);
    }

    // ---- 9: a hostile CLIENT index pointer is probed, not trusted --------
    //
    // The 2026-08-26 fix probed the vertex array; the index array was still
    // read on faith. Same fault class, same rule now: prove it or refuse it.
    {
        ResetLayerForCase();
        EnterViewmodelPass();                     // valid client VERTEX array
        float d[16]; MockTranslate(d, 4.0f, 0, 0);
        GLSurf_SetClassDelta(GLSC_HAND_R, d);
        CHECK(!DrawFaulted(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                           GLS_GL_UNSIGNED_INT,
                           (const void*)(uintptr_t)0xDEADBEEFu),
              "a wild client INDEX pointer is refused, not dereferenced");
    }

    // ---- 10: the scratch route fails closed on VBO-backed arrays --------
    //
    // Scratch redirects glVertexPointer at CODVR memory. With a buffer bound
    // the driver would read our ADDRESS as an OFFSET -- so the route must
    // refuse and let the engine draw untouched, and the driver must never
    // see a scratch pointer while a buffer is bound.
    {
        ResetLayerForCase();
        UploadHandVbo();
        EnterViewmodelPassVboBacked();
        GLSurf_SetRoute(GLS_ROUTE_SCRATCH_ID);
        float d[16]; MockTranslate(d, 4.0f, 0, 0);
        GLSurf_SetClassDelta(GLSC_HAND_R, d);
        CHECK(!DrawFaulted(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                           GLS_GL_UNSIGNED_INT, g_handIdx),
              "scratch route over a VBO does not fault");
        CHECK(CountKind(REC_VPTR) == 0,
              "and the driver never saw a scratch vertexPointer while a "
              "buffer was bound (%d)", CountKind(REC_VPTR));
        GLSurfStats st; GLSurf_GetStats(&st);
        CHECK(st.rejectedVboScratch > 0,
              "the refusal is counted by reason (%u)", st.rejectedVboScratch);
        GLSurf_SetRoute(GLS_ROUTE_MODELVIEW_ID);
    }
    ResetLayerForCase();
}

// ---------------------------------------------------------------------------

static void S_ProductionInertAndCheap()
{
    printf("--- production: heavy instrumentation off, interception cheap ---\n");

    // ---- 18/19/20: nothing expensive can be armed by a production config --
    //
    // The screenshot grab, the CSV census and the WM_CLOSE are all gated on
    // probe-contract values. Rather than assert on the gates one at a time,
    // assert on the thing that decides them: an unarmed config cannot set any
    // of them, so none of those paths has an input that could run it.
    {
        CodvrProbeCfg c;
        ProbeCfg_Production(&c);
        ProbeCfg_Enforce(&c);
        CHECK(c.sweep == 0,
              "production: the sweep is off, so GrabScreenshot has no caller");
        CHECK(c.autoQuit == 0,
              "production: auto-quit is off, so nothing posts WM_CLOSE");
        CHECK(c.capFrames == 0,
              "production: the draw census is off, so no per-draw CSV recording");
        CHECK(c.quitAfter == 0,
              "production: no no-capture quit timer");
        CHECK(c.flatFocus == 0, "production: no flat-run focus assist");
        CHECK(c.probeClass == CODVR_PROBE_CLASS_NONE,
              "production: no surface is being transformed by a probe");
    }
    {
        // And the whole stale sweep block still cannot arm any of them.
        CodvrProbeCfg c = RunCfg(kStaleSweepBlock);
        CHECK(c.sweep == 0 && c.autoQuit == 0 && c.capFrames == 0,
              "the 2026-08-25 override block arms no heavyweight path");
    }

    // ---- the layer does no capture work unless armed ---------------------
    {
        ResetLayerForCase();
        EnterViewmodelPass();
        CHECK(GLSurf_CaptureArmed() == 0,
              "a freshly reset layer has no capture armed");
        for (int i = 0; i < 200; ++i)
            GLSurf_OnDrawElements(GLS_GL_TRIANGLES, 324, GLS_GL_UNSIGNED_SHORT,
                                  g_handIdx);
        CHECK(GLSurf_CaptureRecordCount() == 0,
              "200 draws with no capture armed record NOTHING (%d)",
              GLSurf_CaptureRecordCount());
    }

    // ---- 17/Phase 10: what the interception actually costs ---------------
    //
    // Measured, not asserted from reasoning. The budget that matters: at the
    // ~240 game frames/s the paired scheduler wants, a frame is 4.2 ms, and
    // the viewmodel is a handful of draws. Anything in the tens of
    // microseconds per draw would be a problem; anything under a microsecond
    // is not the reason the engine is missing its pace.
    {
        ResetLayerForCase();
        EnterViewmodelPass();
        LARGE_INTEGER freq, a, b;
        QueryPerformanceFrequency(&freq);

        const int kDraws = 20000;
        QueryPerformanceCounter(&a);
        for (int i = 0; i < kDraws; ++i)
            GLSurf_OnDrawElements(GLS_GL_TRIANGLES, 324, GLS_GL_UNSIGNED_SHORT,
                                  g_handIdx);
        QueryPerformanceCounter(&b);
        const double us = (double)(b.QuadPart - a.QuadPart) * 1e6
                        / (double)freq.QuadPart / (double)kDraws;
        printf("        | classified viewmodel draw: %.3f us each "
               "(%d draws)\n", us, kDraws);
        CHECK(us < 20.0,
              "a classified viewmodel draw costs %.3f us in the interception "
              "(budget 20 us; a 240 Hz frame is 4167 us)", us);

        // The world/menu case: the classifier must early-out, because this one
        // runs thousands of times per frame rather than a handful.
        ResetLayerForCase();
        GLSurf_ScenePhase(0);                 // not in a 3D scene: the menu
        QueryPerformanceCounter(&a);
        for (int i = 0; i < kDraws; ++i)
            GLSurf_OnDrawElements(GLS_GL_TRIANGLES, 324, GLS_GL_UNSIGNED_SHORT,
                                  g_handIdx);
        QueryPerformanceCounter(&b);
        const double usMenu = (double)(b.QuadPart - a.QuadPart) * 1e6
                            / (double)freq.QuadPart / (double)kDraws;
        printf("        | non-scene draw:            %.3f us each\n", usMenu);
        CHECK(usMenu < 2.0,
              "a non-scene draw costs %.3f us -- the classifier early-outs "
              "(budget 2 us; the menu issues ~14000 of these)", usMenu);
    }
    ResetLayerForCase();
}
