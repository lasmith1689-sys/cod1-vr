// Buffer-object shadow. See vr_vbo.h for the design and validity contract.

#include "vr_vbo.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// How many distinct buffer ids may hold a shadow at once, across all
// contexts. The first menu trace pinned a 512-entry table: whatever renders
// the menu/overlay churns through hundreds of small stream buffers. Sized
// well above that; running out is COUNTED and refused rather than grown, so
// the module's memory ceiling is knowable.
enum { VBO_MAX_BUFFERS = 2048 };

// Contexts that render through the proxy: the game's, the Steam overlay's,
// and headroom. Beyond this, calls are tracked under a shared sentinel
// bucket -- ids may then collide across those extra contexts, which fails
// SAFE (a stale/mismatch refusal at resolve time), and the overflow is
// counted so a run that hits it says so.
enum { VBO_MAX_CTX = 8 };

// VALID intervals per buffer. The engine's observed fill pattern is an
// orphan followed by two or three abutting SubData chunks (tex | normal |
// position blocks), which merge into ONE interval; sixteen is headroom for a
// stream buffer written in many disjoint pieces. Overflow drops the smallest
// interval -- refusing bytes that were written, never vouching for bytes that
// were not.
enum { VBO_MAX_SPANS = 16 };

// Total shadow memory ceiling. The viewmodel stream buffers observed so far
// are tens of KB; world geometry could be larger. Beyond this, new shadows
// are refused (stale), because mirroring an unbounded amount of engine data
// is how a helper becomes the memory problem it was measuring.
static const size_t kVboShadowByteCap = 128u * 1024u * 1024u;

struct VboSpan { size_t lo, hi; };        // [lo, hi), hi > lo

struct VboBuf {
    unsigned       id;         // 0 = slot free
    void*          ctx;        // owning GL context (key alongside id)
    unsigned char* shadow;     // CODVR-owned copy of the buffer content
    size_t         size;       // size the engine declared via BufferData
    unsigned       gen;        // content generation: every mirrored write
    unsigned       allocGen;   // allocation generation: every BufferData
    int            stale;      // 1 = shadow cannot vouch for the content
    void*          mappedPtr;  // driver pointer between Map and Unmap
    unsigned       mapAccess;
    // WHICH BYTES ARE ACTUALLY DEFINED in the current allocation generation.
    // Sorted, disjoint, non-abutting (adjacent writes merge). Empty after an
    // orphan. See the contract in vr_vbo.h.
    VboSpan        valid[VBO_MAX_SPANS];
    int            nValid;
};

// Per-context binding state. The sentinel below stands in when calls arrive
// before any wglMakeCurrent was seen (or past the context table): everything
// stays tracked, just under one shared namespace.
static void* const kCtxSentinel = (void*)(uintptr_t)1;

struct VboCtx {
    void*    ctx;              // null = slot free
    unsigned boundArray;       // GL_ARRAY_BUFFER_ARB binding
    unsigned boundElement;     // GL_ELEMENT_ARRAY_BUFFER_ARB binding
};

static VboBuf   g_buf[VBO_MAX_BUFFERS];
static VboCtx   g_ctxTab[VBO_MAX_CTX];
static VboCtx*  g_cur;             // current context's bindings, never null
static VBOStats g_vstats;
static size_t   g_bytesShadowed;

const char* VBO_ReasonName(int reason)
{
    switch (reason) {
    case VBO_RES_OK:            return "OK";
    case VBO_RES_UNKNOWN_ID:    return "UNKNOWN_ID";
    case VBO_RES_STALE:         return "STALE";
    case VBO_RES_RANGE:         return "RANGE";
    case VBO_RES_BYTES_UNKNOWN: return "BYTES_UNKNOWN";
    default:                    return "?";
    }
}

static VboCtx* CtxSlot(void* ctx)
{
    for (int i = 0; i < VBO_MAX_CTX; ++i)
        if (g_ctxTab[i].ctx == ctx) return &g_ctxTab[i];
    for (int i = 0; i < VBO_MAX_CTX; ++i)
        if (!g_ctxTab[i].ctx) {
            g_ctxTab[i].ctx = ctx;
            g_ctxTab[i].boundArray = g_ctxTab[i].boundElement = 0;
            ++g_vstats.contextsSeen;
            return &g_ctxTab[i];
        }
    ++g_vstats.ctxTableFull;
    return nullptr;
}

static VboCtx* Cur()
{
    if (!g_cur) g_cur = CtxSlot(kCtxSentinel);
    return g_cur;
}

void VBO_NoteMakeCurrent(void* ctx)
{
    VboCtx* c = CtxSlot(ctx ? ctx : kCtxSentinel);
    if (c) g_cur = c;          // table full: keep the previous (safe) bucket
}

// One-entry MRU cache: the hot path resolves the same buffer for every sample
// of a draw, so the common case must be a compare, not a table scan.
static VboBuf* g_lastHit;

static VboBuf* FindBuf(unsigned id)
{
    if (id == 0) return nullptr;
    void* const ctx = Cur() ? Cur()->ctx : kCtxSentinel;
    if (g_lastHit && g_lastHit->id == id && g_lastHit->ctx == ctx)
        return g_lastHit;
    for (int i = 0; i < VBO_MAX_BUFFERS; ++i)
        if (g_buf[i].id == id && g_buf[i].ctx == ctx) {
            g_lastHit = &g_buf[i];
            return g_lastHit;
        }
    return nullptr;
}

static VboBuf* FindOrClaimBuf(unsigned id)
{
    VboBuf* b = FindBuf(id);
    if (b) return b;
    void* const ctx = Cur() ? Cur()->ctx : kCtxSentinel;
    for (int i = 0; i < VBO_MAX_BUFFERS; ++i) {
        if (g_buf[i].id == 0) {
            memset(&g_buf[i], 0, sizeof(g_buf[i]));
            g_buf[i].id  = id;
            g_buf[i].ctx = ctx;
            ++g_vstats.buffersLive;
            if (g_vstats.buffersLive > g_vstats.buffersPeak)
                g_vstats.buffersPeak = g_vstats.buffersLive;
            g_lastHit = &g_buf[i];
            return g_lastHit;
        }
    }
    ++g_vstats.tableFull;
    return nullptr;
}

static void FreeShadow(VboBuf* b);   // defined with the release helpers below

void VBO_NoteDeleteContext(void* ctx)
{
    if (!ctx) return;
    for (int i = 0; i < VBO_MAX_BUFFERS; ++i)
        if (g_buf[i].id && g_buf[i].ctx == ctx) {
            FreeShadow(&g_buf[i]);
            if (g_buf[i].stale && g_vstats.staleBuffers) --g_vstats.staleBuffers;
            memset(&g_buf[i], 0, sizeof(g_buf[i]));
            if (g_vstats.buffersLive) --g_vstats.buffersLive;
        }
    g_lastHit = nullptr;
    for (int i = 0; i < VBO_MAX_CTX; ++i)
        if (g_ctxTab[i].ctx == ctx) {
            if (g_cur == &g_ctxTab[i]) g_cur = nullptr;
            memset(&g_ctxTab[i], 0, sizeof(g_ctxTab[i]));
        }
}

static void FreeShadow(VboBuf* b)
{
    if (b->shadow) {
        g_bytesShadowed -= b->size;
        free(b->shadow);
        b->shadow = nullptr;
    }
    b->nValid = 0;
}

static void ReleaseBuf(VboBuf* b)
{
    FreeShadow(b);
    if (b->stale && g_vstats.staleBuffers) --g_vstats.staleBuffers;
    memset(b, 0, sizeof(*b));
    if (g_vstats.buffersLive) --g_vstats.buffersLive;
    if (g_lastHit == b) g_lastHit = nullptr;
}

static void MarkStale(VboBuf* b)
{
    if (!b->stale) { b->stale = 1; ++g_vstats.staleBuffers; }
    b->nValid = 0;                     // nothing is vouched for
}

// ------------------------------------------------------------ valid spans

// Record [lo, hi) as written in the current generation. Overlapping and
// abutting spans merge; the table stays sorted and disjoint.
static void SpanAdd(VboBuf* b, size_t lo, size_t hi)
{
    if (hi <= lo) return;
    // Merge every existing span that overlaps or abuts [lo, hi).
    int w = 0;
    for (int i = 0; i < b->nValid; ++i) {
        const VboSpan s = b->valid[i];
        if (s.hi < lo || s.lo > hi) {          // disjoint, not abutting
            b->valid[w++] = s;
        } else {
            if (s.lo < lo) lo = s.lo;
            if (s.hi > hi) hi = s.hi;
        }
    }
    b->nValid = w;
    if (b->nValid >= VBO_MAX_SPANS) {
        // Full: drop the SMALLEST existing span. Bytes that were written are
        // now refused -- the safe direction -- and it is counted.
        int smallest = 0;
        for (int i = 1; i < b->nValid; ++i)
            if (b->valid[i].hi - b->valid[i].lo
                < b->valid[smallest].hi - b->valid[smallest].lo)
                smallest = i;
        for (int i = smallest; i + 1 < b->nValid; ++i)
            b->valid[i] = b->valid[i + 1];
        --b->nValid;
        ++g_vstats.validSpanDrops;
    }
    // Insert in sorted position.
    int pos = b->nValid;
    while (pos > 0 && b->valid[pos - 1].lo > lo) {
        b->valid[pos] = b->valid[pos - 1];
        --pos;
    }
    b->valid[pos].lo = lo;
    b->valid[pos].hi = hi;
    ++b->nValid;
}

// Bytes of [lo, hi) NOT covered by any valid span.
static size_t SpanUnknownBytes(const VboBuf* b, size_t lo, size_t hi)
{
    if (hi <= lo) return 0;
    size_t covered = 0;
    for (int i = 0; i < b->nValid; ++i) {
        const size_t a = b->valid[i].lo > lo ? b->valid[i].lo : lo;
        const size_t e = b->valid[i].hi < hi ? b->valid[i].hi : hi;
        if (e > a) covered += e - a;
    }
    return (hi - lo) - covered;
}

// Fast path: spans are disjoint and merged, so full coverage means ONE span
// contains the whole range.
static int SpanCovers(const VboBuf* b, size_t lo, size_t hi)
{
    if (hi <= lo) return 1;
    for (int i = 0; i < b->nValid; ++i)
        if (b->valid[i].lo <= lo && b->valid[i].hi >= hi) return 1;
    return 0;
}

// ------------------------------------------------------------ engine mirror

void VBO_NoteGenBuffers(int n, const unsigned* ids)
{
    ++g_vstats.genCalls;
    // Nothing to shadow yet: an id becomes interesting when it is bound and
    // fed. Claiming slots here would let a gen-happy engine exhaust the table
    // with ids it never uses.
    (void)n; (void)ids;
}

void VBO_NoteDeleteBuffers(int n, const unsigned* ids)
{
    ++g_vstats.deleteCalls;
    if (!ids || n <= 0) return;
    VboCtx* c = Cur();
    for (int i = 0; i < n; ++i) {
        const unsigned id = ids[i];
        if (id == 0) continue;
        // GL semantics: deleting a bound buffer reverts that binding to 0
        // (in the deleting context).
        if (c) {
            if (c->boundArray   == id) c->boundArray   = 0;
            if (c->boundElement == id) c->boundElement = 0;
        }
        VboBuf* b = FindBuf(id);
        if (b) ReleaseBuf(b);
    }
}

void VBO_NoteBindBuffer(unsigned target, unsigned id)
{
    ++g_vstats.bindCalls;
    VboCtx* c = Cur();
    if (target == VBO_GL_ARRAY_BUFFER) {
        ++g_vstats.bindArray;
        if (c) c->boundArray = id;
    } else if (target == VBO_GL_ELEMENT_ARRAY_BUFFER) {
        ++g_vstats.bindElement;
        if (c) c->boundElement = id;
    } else {
        ++g_vstats.bindOtherTarget;
    }
}

static unsigned BoundIdFor(unsigned target)
{
    const VboCtx* c = Cur();
    if (!c) return 0;
    if (target == VBO_GL_ARRAY_BUFFER)         return c->boundArray;
    if (target == VBO_GL_ELEMENT_ARRAY_BUFFER) return c->boundElement;
    return 0;
}

void VBO_NoteBufferData(unsigned target, ptrdiff_t size, const void* data,
                        unsigned usage)
{
    (void)usage;
    ++g_vstats.dataCalls;
    if (target == VBO_GL_ARRAY_BUFFER)              ++g_vstats.dataArray;
    else if (target == VBO_GL_ELEMENT_ARRAY_BUFFER) ++g_vstats.dataElement;

    const unsigned id = BoundIdFor(target);
    if (id == 0) { ++g_vstats.dataUnbound; return; }
    VboBuf* b = FindOrClaimBuf(id);
    if (!b) return;                              // table full: counted above

    if (size < 0) { MarkStale(b); return; }

    // BufferData reallocates the store, so the shadow follows suit. A size of
    // zero is a legal way to release the store. Whatever was VALID belongs to
    // the PREVIOUS allocation generation and can never satisfy a read again.
    FreeShadow(b);
    b->size = (size_t)size;
    b->gen++;
    b->allocGen++;
    b->nValid = 0;
    if (b->stale) { --g_vstats.staleBuffers; b->stale = 0; }

    if (b->size == 0) return;
    if (g_bytesShadowed + b->size > kVboShadowByteCap) {
        ++g_vstats.allocFailed;
        MarkStale(b);
        return;
    }
    b->shadow = (unsigned char*)malloc(b->size);
    if (!b->shadow) {
        ++g_vstats.allocFailed;
        MarkStale(b);
        return;
    }
    g_bytesShadowed += b->size;
    if (g_bytesShadowed > g_vstats.bytesPeak) g_vstats.bytesPeak = g_bytesShadowed;

    if (data) {
        memcpy(b->shadow, data, b->size);
        SpanAdd(b, 0, b->size);                  // fully defined by this call
    } else {
        // The orphaning idiom: the store exists but holds UNDEFINED content
        // until SubData/Map fills it. Zero-fill so a mistaken read is at
        // least deterministic -- but vouch for NOTHING.
        memset(b->shadow, 0, b->size);
        ++g_vstats.orphans;
    }
}

void VBO_NoteBufferSubData(unsigned target, ptrdiff_t offset, ptrdiff_t size,
                           const void* data)
{
    ++g_vstats.subDataCalls;
    if (target == VBO_GL_ARRAY_BUFFER)              ++g_vstats.subDataArray;
    else if (target == VBO_GL_ELEMENT_ARRAY_BUFFER) ++g_vstats.subDataElement;

    const unsigned id = BoundIdFor(target);
    if (id == 0) { ++g_vstats.dataUnbound; return; }
    VboBuf* b = FindBuf(id);
    if (!b) {
        // SubData into a buffer whose BufferData we never saw (created before
        // the proxy loaded, or an upload path we do not mirror). The id is
        // claimed so the refusal is per-buffer and visible, not repeated noise.
        b = FindOrClaimBuf(id);
        if (b) { ++g_vstats.subOutOfRange; MarkStale(b); }
        return;
    }
    if (!b->shadow || b->stale) { ++g_vstats.subOutOfRange; MarkStale(b); return; }
    if (offset < 0 || size < 0) { ++g_vstats.subOutOfRange; MarkStale(b); return; }
    const size_t off = (size_t)offset, len = (size_t)size;
    if (off + len < off || off + len > b->size) {
        // The engine knows a size we do not: our model of this buffer is
        // wrong, so stop vouching for any of it.
        ++g_vstats.subOutOfRange;
        MarkStale(b);
        return;
    }
    if (data && len) {
        memcpy(b->shadow + off, data, len);
        b->gen++;
        SpanAdd(b, off, off + len);              // exactly the written bytes
    }
}

void VBO_NoteMapBuffer(unsigned target, unsigned access, void* mappedPtr)
{
    ++g_vstats.mapCalls;
    const unsigned id = BoundIdFor(target);
    if (id == 0 || !mappedPtr) return;
    VboBuf* b = FindOrClaimBuf(id);
    if (!b) return;
    b->mappedPtr  = mappedPtr;
    b->mapAccess  = access;
    if (!b->shadow) {
        // Mapped a buffer we never saw sized. glGetBufferParameteriv could
        // answer, but it needs a live driver and this module must not own GL
        // calls -- so the buffer is stale until a BufferData shows its size.
        ++g_vstats.mapUnknownSize;
        MarkStale(b);
    }
}

void VBO_NoteUnmapBuffer(unsigned target)
{
    ++g_vstats.unmapCalls;
    const unsigned id = BoundIdFor(target);
    if (id == 0) return;
    VboBuf* b = FindBuf(id);
    if (!b) return;
    // The engine's writes are complete and the driver pointer is still valid
    // -- this note runs BEFORE the Unmap is forwarded. Sync the whole store:
    // GL offers no record of which bytes a mapping changed, and a wrong guess
    // here would be silent geometry corruption in every later resolve.
    if (b->mappedPtr && b->shadow && !b->stale) {
        memcpy(b->shadow, b->mappedPtr, b->size);
        b->gen++;
        b->nValid = 0;
        SpanAdd(b, 0, b->size);                  // a whole-store sync defines it all
        ++g_vstats.unmapCopies;
    }
    b->mappedPtr = nullptr;      // nothing driver-owned survives the Unmap
    b->mapAccess = 0;
}

// ------------------------------------------------------------------ queries

unsigned VBO_BoundArrayBuffer()   { const VboCtx* c = Cur(); return c ? c->boundArray   : 0; }
unsigned VBO_BoundElementBuffer() { const VboCtx* c = Cur(); return c ? c->boundElement : 0; }

static void FillInfo(VBOResolveInfo* info, const VboBuf* b, int reason,
                     size_t unknownBytes)
{
    if (!info) return;
    memset(info, 0, sizeof(*info));
    info->reason = reason;
    info->unknownBytes = unknownBytes;
    if (b) {
        info->contentGen = b->gen;
        info->allocGen   = b->allocGen;
        info->size       = b->size;
        info->validSpans = b->nValid;
    }
}

// The common gate: allocated, in range, not stale. `requireDefined` adds the
// validity contract. Counters are ticked here so both entry points agree.
static const void* ResolveCommon(unsigned id, size_t offset, size_t bytes,
                                 VBOResolveInfo* info, int requireDefined)
{
    VboBuf* b = FindBuf(id);
    if (!b) {
        ++g_vstats.resolveMissUnknown;
        FillInfo(info, nullptr, VBO_RES_UNKNOWN_ID, bytes);
        return nullptr;
    }
    if (!b->shadow || b->stale) {
        ++g_vstats.resolveMissStale;
        FillInfo(info, b, VBO_RES_STALE, bytes);
        return nullptr;
    }
    const size_t end = offset + bytes;
    if (end < offset || end > b->size) {
        ++g_vstats.resolveMissRange;
        FillInfo(info, b, VBO_RES_RANGE, bytes);
        return nullptr;
    }
    if (requireDefined && bytes != 0 && !SpanCovers(b, offset, end)) {
        // The bytes must have been WRITTEN in this allocation generation.
        // Without this the module vouches for the zero-fill an orphan left
        // behind, and the caller reads a geometrically plausible but entirely
        // invented point cloud.
        ++g_vstats.resolveMissUndefined;
        FillInfo(info, b, VBO_RES_BYTES_UNKNOWN, SpanUnknownBytes(b, offset, end));
        return nullptr;
    }
    ++g_vstats.resolveHits;
    FillInfo(info, b, VBO_RES_OK, requireDefined ? 0 : SpanUnknownBytes(b, offset, end));
    return b->shadow + offset;
}

const void* VBO_ResolveRange(unsigned id, size_t offset, size_t bytes)
{
    return ResolveCommon(id, offset, bytes, nullptr, 1);
}

const void* VBO_ResolveRangeEx(unsigned id, size_t offset, size_t bytes,
                               VBOResolveInfo* info)
{
    return ResolveCommon(id, offset, bytes, info, 1);
}

const void* VBO_ResolveAllocated(unsigned id, size_t offset, size_t bytes,
                                 VBOResolveInfo* info)
{
    return ResolveCommon(id, offset, bytes, info, 0);
}

int VBO_RangeDefined(unsigned id, size_t offset, size_t bytes)
{
    const VboBuf* b = FindBuf(id);
    if (!b || !b->shadow || b->stale) return 0;
    const size_t end = offset + bytes;
    if (end < offset || end > b->size) return 0;
    return SpanCovers(b, offset, end);
}

unsigned VBO_Generation(unsigned id)
{
    const VboBuf* b = FindBuf(id);
    return b ? b->gen : 0;
}

unsigned VBO_AllocGeneration(unsigned id)
{
    const VboBuf* b = FindBuf(id);
    return b ? b->allocGen : 0;
}

void VBO_GetStats(VBOStats* out)
{
    if (!out) return;
    *out = g_vstats;
    out->bytesShadowed = g_bytesShadowed;
}

void VBO_Reset()
{
    for (int i = 0; i < VBO_MAX_BUFFERS; ++i)
        if (g_buf[i].id) { FreeShadow(&g_buf[i]); memset(&g_buf[i], 0, sizeof(g_buf[i])); }
    memset(&g_vstats, 0, sizeof(g_vstats));
    memset(g_ctxTab, 0, sizeof(g_ctxTab));
    g_cur = nullptr;
    g_bytesShadowed = 0;
    g_lastHit = nullptr;
}
