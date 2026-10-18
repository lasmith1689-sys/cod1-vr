// Buffer-object shadow: CPU copies of what the engine uploads to GL buffers.
//
// WHY THIS EXISTS
//
// The 2026-08-26 crash proved the engine draws the viewmodel out of vertex
// buffer objects: every recorded viewmodel draw carries a glVertexPointer
// "pointer" under 64 KB (0x14, 0x1E0, 0x82C8, 0x12C0) -- byte OFFSETS into a
// bound GL_ARRAY_BUFFER_ARB, not addresses. ClientRangeReadable() now refuses
// those instead of faulting, which keeps the process alive but also makes
// every vertex-reading feature (pivots, the palm solve, hand deltas)
// silently inert in-game: the geometry the features need is on the other
// side of a bus.
//
// GL will hand that data back only at the price of a pipeline stall
// (glGetBufferSubDataARB / glMapBufferARB on the draw path), which a 2003
// engine already missing its frame pace cannot pay. But the data does not
// have to be asked for: every byte in a buffer object got there through an
// API call WE forward -- glBufferDataARB, glBufferSubDataARB, or a
// glMapBufferARB/glUnmapBufferARB write window. So this module keeps a CPU
// SHADOW of each buffer, mirrored as the engine's own uploads pass through
// the interception, and the hot path reads CODVR-owned memory:
//
//     BufferData     -> (re)allocate the shadow, copy the payload
//     BufferSubData  -> copy the changed range
//     Map ... Unmap  -> at Unmap time (the pointer is still valid until the
//                       forward happens) copy the whole buffer; no driver
//                       pointer survives past the Unmap
//     DeleteBuffers  -> free the shadow; later offsets into that id refuse
//
// No GPU readback, no glFinish, no stall, ever. A buffer whose content this
// module cannot vouch for (an upload path it has not seen, a SubData beyond
// the declared size, the shadow allocation failing) is marked STALE and every
// resolve against it refuses -- the same fail-closed posture as the client
// array probe: a refusal costs one draw's feature, a wrong guess costs
// correctness silently.
//
// THE VALIDITY CONTRACT (project-lead review 2026-08-29, section 2)
//
// Every shadow tracks, per buffer:
//   * an ALLOCATION GENERATION, bumped by every BufferData;
//   * a CONTENT generation, bumped by every mirrored write;
//   * the allocated size;
//   * the set of byte intervals WRITTEN in the current allocation generation
//     (VALID); everything else in the allocation is UNKNOWN.
// Rules:
//   BufferData(size, non-NULL): new allocation generation; [0,size) VALID.
//   BufferData(size, NULL)    : new allocation generation; the ENTIRE store is
//                               UNKNOWN. GL says the content is undefined, and
//                               the 2026-08-28 headset run showed what
//                               vouching for the zero-fill does: the palm
//                               solve fits its bind cloud onto 48 copies of
//                               (0,0,0) and refuses at a CONSTANT residual
//                               equal to the bind cloud's own radius of
//                               gyration, with vboMiss reading 0. Not a crash,
//                               not a miss: a silent wrong answer.
//   BufferSubData(off, len)   : exactly [off, off+len) becomes VALID.
//   Map ... Unmap             : the whole store becomes VALID (the mapping
//                               contract syncs the whole store; GL keeps no
//                               record of which bytes a mapping changed).
//   A read touching ANY UNKNOWN byte is REFUSED with VBO_RES_BYTES_UNKNOWN,
//   distinct from unknown-id / stale / out-of-range. No zero-filled substitute
//   is ever returned. Previous-generation data can never satisfy a read in a
//   new generation, because BufferData clears the valid set.
//
// THREADING. Everything here is called from the engine's GL thread, through
// the same interception that already treats GL as single-threaded. No locks.
//
// This header must stay free of GL and Windows headers: the safe suite
// compiles the module with no driver present.

#pragma once

#include <stddef.h>

// ------------------------------------------------------------------ GL enums
// Spelled out locally, same policy as vr_glsurf.h.
enum {
    VBO_GL_ARRAY_BUFFER         = 0x8892,   // GL_ARRAY_BUFFER_ARB
    VBO_GL_ELEMENT_ARRAY_BUFFER = 0x8893,   // GL_ELEMENT_ARRAY_BUFFER_ARB
};

// ---------------------------------------------------------------- contexts
//
// Buffer ids are per share-group, and MORE THAN ONE context renders through
// this proxy: the game's, and whatever the Steam overlay creates. The first
// menu trace showed their call streams interleaved -- id 1 in one context is
// NOT id 1 in the other, and a global table would happily serve one context's
// bytes for the other's id. So every binding and every buffer is keyed by the
// context that was current when the call arrived. Two contexts that share
// lists (wglShareLists) would be over-split by this; nothing in this game
// does that, and over-splitting fails SAFE (a refusal), not wrong.
void VBO_NoteMakeCurrent(void* ctx);      // ctx may be null (release)
void VBO_NoteDeleteContext(void* ctx);    // frees that context's shadows

// -------------------------------------------------------- engine-call mirror
//
// Called from dllmain's extension wrappers as the engine's own calls pass
// through. Order contract: every note may run BEFORE the call is forwarded to
// the driver EXCEPT NoteGenBuffers (the driver fills the ids, so it must be
// noted after) -- and NoteUnmapBuffer MUST run before the forward, because
// the mapped pointer dies when the driver's Unmap returns.
void VBO_NoteGenBuffers(int n, const unsigned* ids);
void VBO_NoteDeleteBuffers(int n, const unsigned* ids);
void VBO_NoteBindBuffer(unsigned target, unsigned id);
void VBO_NoteBufferData(unsigned target, ptrdiff_t size, const void* data,
                        unsigned usage);
void VBO_NoteBufferSubData(unsigned target, ptrdiff_t offset, ptrdiff_t size,
                           const void* data);
void VBO_NoteMapBuffer(unsigned target, unsigned access, void* mappedPtr);
void VBO_NoteUnmapBuffer(unsigned target);

// What is bound right now. glVertexPointer latches the ARRAY binding at call
// time; glDrawElements latches the ELEMENT binding at draw time -- the two
// callers in vr_glsurf sample these at exactly those moments.
unsigned VBO_BoundArrayBuffer();
unsigned VBO_BoundElementBuffer();

// ------------------------------------------------------------------ resolve
//
// Why a resolve declined, so a refusal is attributable at the site that
// asked instead of being one anonymous miss.
enum VBOResolveReason {
    VBO_RES_OK = 0,
    VBO_RES_UNKNOWN_ID,      // id never seen (or deleted)
    VBO_RES_STALE,           // shadow cannot vouch for the store at all
    VBO_RES_RANGE,           // offset+bytes outside the declared size
    VBO_RES_BYTES_UNKNOWN,   // allocated, in range, but bytes never written
                             // in the current allocation generation
};
const char* VBO_ReasonName(int reason);

struct VBOResolveInfo {
    int      reason;         // VBOResolveReason
    unsigned contentGen;     // bumped on every mirrored write
    unsigned allocGen;       // bumped on every BufferData
    size_t   size;           // declared store size (0 if unknown)
    size_t   unknownBytes;   // bytes of the request not VALID (BYTES_UNKNOWN)
    int      validSpans;     // number of VALID intervals the store holds
};

// The hot-path read. Resolve [offset, offset+bytes) of buffer `id` to a
// pointer into the CODVR shadow, or null when the module cannot vouch for the
// content (unknown id, stale shadow, out of range, arithmetic wrap, or ANY
// byte of the range unknown). The returned pointer is CODVR-owned malloc
// memory: always CPU-readable, no VirtualQuery needed, valid until the next
// engine upload to that buffer.
const void* VBO_ResolveRange(unsigned id, size_t offset, size_t bytes);
const void* VBO_ResolveRangeEx(unsigned id, size_t offset, size_t bytes,
                               VBOResolveInfo* info);

// Is every byte of [offset, offset+bytes) VALID in the current generation?
// No pointer, no counters touched: this is the per-landmark gate a reader
// asks BEFORE dereferencing a store it obtained with VBO_ResolveAllocated.
int VBO_RangeDefined(unsigned id, size_t offset, size_t bytes);

// A pointer into the store for a range that is allocated, in range and not
// stale -- WITHOUT requiring the bytes to be defined. Exists for ONE caller
// pattern: a reader that touches only a few known bytes of a large span (the
// palm solve reads 48 landmarks of 1674 vertices) resolves the span here and
// then asks VBO_RangeDefined for each landmark it is about to read. Reading
// any byte through this pointer without that check is the silent-wrong-data
// bug this module exists to prevent; do not add a second caller casually.
// Undefined bytes in the store are zero-filled so a mistaken read is at least
// deterministic, but the CONTRACT is that they are never read.
const void* VBO_ResolveAllocated(unsigned id, size_t offset, size_t bytes,
                                 VBOResolveInfo* info);

// Content generation of a buffer: bumped on every mirrored upload. Lets a
// consumer notice "the geometry changed under me" without comparing bytes.
// Returns 0 for an unknown id.
unsigned VBO_Generation(unsigned id);
// Allocation generation: bumped on every BufferData (orphaning or not).
unsigned VBO_AllocGeneration(unsigned id);

// --------------------------------------------------------------- statistics
//
// Split by reason, same policy as GLSurfStats: "the shadow declined" and
// "the engine used an upload path we do not mirror" are different facts, and
// only the census of these counters can say which one a flat run saw.
struct VBOStats {
    // engine call counts, per entry point
    unsigned genCalls, deleteCalls, bindCalls;
    unsigned dataCalls, subDataCalls, mapCalls, unmapCalls;
    // per-target split of the two that matter
    unsigned bindArray, bindElement, bindOtherTarget;
    unsigned dataArray, dataElement;
    unsigned subDataArray, subDataElement;
    // shadow bookkeeping
    unsigned buffersLive;          // ids currently holding a shadow
    unsigned buffersPeak;
    size_t   bytesShadowed;        // total shadow memory right now
    size_t   bytesPeak;
    unsigned unmapCopies;          // whole-buffer syncs done at Unmap time
    // refusals, by reason
    unsigned tableFull;            // more live ids than the table holds
    unsigned allocFailed;          // malloc refused; buffer marked stale
    unsigned subOutOfRange;        // SubData beyond the declared size
    unsigned mapUnknownSize;       // mapped a buffer never sized by BufferData
    unsigned dataUnbound;          // upload with nothing bound (engine bug)
    unsigned staleBuffers;         // ids currently marked stale
    // resolves
    unsigned resolveHits;
    unsigned resolveMissUnknown;   // id never seen
    unsigned resolveMissStale;     // shadow could not vouch for content
    unsigned resolveMissRange;     // offset+bytes outside the buffer
    // The store exists but the requested bytes were never written in the
    // current allocation generation, so their content is UNDEFINED by GL.
    // Counted separately so a run can say "the shadow declined because the
    // engine had not filled it yet" (VBO_RES_BYTES_UNKNOWN).
    unsigned resolveMissUndefined;
    unsigned orphans;              // BufferData(NULL) seen: store reset to
                                   // undefined
    unsigned validSpanDrops;       // the per-buffer VALID interval table was
                                   // full and the smallest interval was
                                   // dropped: bytes that WERE written are now
                                   // treated as unknown (extra refusals, never
                                   // a false vouch -- the safe direction)
    unsigned contextsSeen;         // distinct GL contexts that made calls
    unsigned ctxTableFull;         // contexts beyond the table (calls kept
                                   // under a sentinel bucket, still safe)
};
void VBO_GetStats(VBOStats* out);

// Full reset -- the suite calls it per case. Frees every shadow.
void VBO_Reset();
