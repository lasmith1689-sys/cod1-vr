// vr_snapshot.cpp -- see vr_snapshot.h.
#include "vr_snapshot.h"
#include "codvr.h"
#include "vr.h"
#include "vr_input.h"
#include "vr_native_hands.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

// ------------------------------------------------------------ config/state
static VRSnapConfig g_cfg = { 0, 10.0f, 1, 0, 80, 2, 1.0f };
static VRSnapState  g_st  = { -1.0, 0, 0, 0 };
static char         g_dir[MAX_PATH];
static int          g_dirReady;
static int          g_pendingMask;      // eyes still to write for the current pair
static int          g_pendingWhy;
static int          g_pendingSeq;
static double       g_pendingNowS;
static int          g_writeFails;

int VRSnap_ConfigKey(const char* key, float val)
{
    if (!_stricmp(key, "vr_snapshot"))                 { g_cfg.enabled = (val != 0.0f); }
    else if (!_stricmp(key, "vr_snapshot_interval_s")) { g_cfg.intervalS = val < 0 ? 0 : val; }
    else if (!_stricmp(key, "vr_snapshot_on_trigger")) { g_cfg.onTrigger = (val != 0.0f); }
    else if (!_stricmp(key, "vr_snapshot_on_button"))  { g_cfg.onButton = (val != 0.0f); }
    else if (!_stricmp(key, "vr_snapshot_max"))        { g_cfg.maxCount = (int)val; }
    else if (!_stricmp(key, "vr_snapshot_scale"))      { int d = (int)val; g_cfg.scaleDiv = d < 1 ? 1 : d > 8 ? 8 : d; }
    else if (!_stricmp(key, "vr_snapshot_debounce_s")) { g_cfg.debounceS = val < 0 ? 0 : val; }
    else return 0;
    return 1;
}

void VRSnap_GetConfig(VRSnapConfig* out) { if (out) *out = g_cfg; }

int VRSnap_Decide(const VRSnapConfig* cfg, VRSnapState* st, double nowS,
                  int triggerDown, int buttonDown)
{
    const int trigEdge = triggerDown && !st->prevTrigger;
    const int btnEdge  = buttonDown && !st->prevButton;
    st->prevTrigger = triggerDown ? 1 : 0;
    st->prevButton  = buttonDown ? 1 : 0;
    if (!cfg->enabled) return 0;
    if (cfg->maxCount > 0 && st->taken >= cfg->maxCount) return 0;
    int why = 0;
    if (st->lastSnapS < 0.0) why |= VRSNAP_WHY_FIRST;
    else if (cfg->intervalS > 0.0f && nowS - st->lastSnapS >= cfg->intervalS) why |= VRSNAP_WHY_TIMER;
    if (cfg->onTrigger && trigEdge) why |= VRSNAP_WHY_TRIGGER;
    if (cfg->onButton && btnEdge) why |= VRSNAP_WHY_BUTTON;
    if (!why) return 0;
    // Debounce every source, the first shot included (it fires at once).
    if (st->lastSnapS >= 0.0 && nowS - st->lastSnapS < cfg->debounceS) return 0;
    st->lastSnapS = nowS;
    ++st->taken;
    return why;
}

// ------------------------------------------------------------- PNG writer
static unsigned g_crcTable[256];
static void CrcInit()
{
    if (g_crcTable[1]) return;
    for (unsigned n = 0; n < 256; ++n) {
        unsigned c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crcTable[n] = c;
    }
}
static unsigned Crc(unsigned crc, const unsigned char* p, size_t n)
{
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) crc = g_crcTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}
static void Put32(unsigned char* p, unsigned v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}
static int WriteChunk(FILE* f, const char* type, const unsigned char* data, size_t n)
{
    unsigned char hdr[8];
    Put32(hdr, (unsigned)n);
    memcpy(hdr + 4, type, 4);
    unsigned crc = Crc(0, hdr + 4, 4);
    if (n) crc = Crc(crc, data, n);
    unsigned char tail[4];
    Put32(tail, crc);
    if (fwrite(hdr, 1, 8, f) != 8) return 0;
    if (n && fwrite(data, 1, n, f) != n) return 0;
    if (fwrite(tail, 1, 4, f) != 4) return 0;
    return 1;
}

int VRSnap_WritePng(const char* path, const unsigned char* rgb, int w, int h)
{
    if (!path || !rgb || w < 1 || h < 1) return 0;
    CrcInit();
    // Raw scanlines: filter byte 0 + w*3 bytes per row.
    const size_t rowBytes = (size_t)w * 3 + 1;
    const size_t rawLen = rowBytes * (size_t)h;
    unsigned char* raw = (unsigned char*)malloc(rawLen);
    if (!raw) return 0;
    for (int y = 0; y < h; ++y) {
        raw[y * rowBytes] = 0;
        memcpy(raw + y * rowBytes + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    }
    // zlib stream of STORED deflate blocks (<= 65535 bytes each).
    const size_t blocks = (rawLen + 65534) / 65535;
    const size_t zLen = 2 + rawLen + blocks * 5 + 4;
    unsigned char* z = (unsigned char*)malloc(zLen);
    if (!z) { free(raw); return 0; }
    size_t zp = 0;
    z[zp++] = 0x78; z[zp++] = 0x01;
    unsigned a = 1, b = 0;
    for (size_t i = 0; i < rawLen; ++i) { a = (a + raw[i]) % 65521u; b = (b + a) % 65521u; }
    size_t off = 0;
    while (off < rawLen || rawLen == 0) {
        size_t n = rawLen - off; if (n > 65535) n = 65535;
        const int last = (off + n >= rawLen);
        z[zp++] = (unsigned char)(last ? 1 : 0);
        z[zp++] = (unsigned char)(n & 0xFF); z[zp++] = (unsigned char)(n >> 8);
        z[zp++] = (unsigned char)(~n & 0xFF); z[zp++] = (unsigned char)((~n >> 8) & 0xFF);
        memcpy(z + zp, raw + off, n); zp += n;
        off += n;
        if (rawLen == 0) break;
    }
    Put32(z + zp, (b << 16) | a); zp += 4;
    free(raw);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) { free(z); return 0; }
    static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    int ok = fwrite(sig, 1, 8, f) == 8;
    unsigned char ihdr[13];
    Put32(ihdr, (unsigned)w); Put32(ihdr + 4, (unsigned)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    ok = ok && WriteChunk(f, "IHDR", ihdr, 13);
    ok = ok && WriteChunk(f, "IDAT", z, zp);
    ok = ok && WriteChunk(f, "IEND", nullptr, 0);
    const long len = ftell(f);
    fclose(f);
    free(z);
    return ok ? (int)len : 0;
}

void VRSnap_Downsample(const unsigned char* rgb, int w, int h, int div,
                       unsigned char* out, int* outW, int* outH)
{
    if (div < 1) div = 1;
    const int ow = w / div, oh = h / div;
    if (outW) *outW = ow;
    if (outH) *outH = oh;
    if (!out || ow < 1 || oh < 1) return;
    const int area = div * div;
    for (int y = 0; y < oh; ++y) {
        for (int x = 0; x < ow; ++x) {
            unsigned s[3] = { 0, 0, 0 };
            for (int dy = 0; dy < div; ++dy) {
                const unsigned char* row = rgb + ((size_t)(y * div + dy) * w + (size_t)x * div) * 3;
                for (int dx = 0; dx < div; ++dx) {
                    s[0] += row[dx * 3]; s[1] += row[dx * 3 + 1]; s[2] += row[dx * 3 + 2];
                }
            }
            unsigned char* o = out + ((size_t)y * ow + x) * 3;
            o[0] = (unsigned char)((s[0] + area / 2) / area);
            o[1] = (unsigned char)((s[1] + area / 2) / area);
            o[2] = (unsigned char)((s[2] + area / 2) / area);
        }
    }
}

// ---------------------------------------------------------------- runtime
void VRSnap_Init(const char* dllPath)
{
    g_dir[0] = 0; g_dirReady = 0;
    if (!dllPath) return;
    strncpy_s(g_dir, dllPath, _TRUNCATE);
    char* slash = strrchr(g_dir, '\\');
    if (!slash) return;
    strcpy_s(slash + 1, MAX_PATH - (slash + 1 - g_dir), "snapshots");
    if (!g_cfg.enabled) return;
    if (CreateDirectoryA(g_dir, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) {
        g_dirReady = 1;
        Log("snapshot: ENABLED -> %s (interval %.1fs, trigger %d, button %d, max %d, scale 1/%d)",
            g_dir, g_cfg.intervalS, g_cfg.onTrigger, g_cfg.onButton, g_cfg.maxCount, g_cfg.scaleDiv);
    } else {
        Log("snapshot: cannot create %s (err %lu) -- disabled", g_dir, GetLastError());
    }
}

void VRSnap_Update(const VRInputState* in, double nowS)
{
    if (!g_cfg.enabled || !g_dirReady) return;
    if (g_pendingMask) {
        // An aborted pair never delivers its second eye; do not wedge.
        if (nowS - g_pendingNowS < 1.0) return;
        Log("snapshot: #%d pair incomplete (mask %d) -- dropped", g_pendingSeq, g_pendingMask);
        g_pendingMask = 0;
    }
    int trig = 0, btn = 0;
    if (in && in->valid) {
        const VRHandState& r = in->hand[VRI_RIGHT];
        trig = r.trigger >= 0.6f;
        btn = r.btnPrimary != 0;
    }
    const int why = VRSnap_Decide(&g_cfg, &g_st, nowS, trig, btn);
    if (!why) return;
    g_pendingMask = 3;
    g_pendingWhy = why;
    g_pendingSeq = g_st.taken;
    g_pendingNowS = nowS;
}

typedef void (__stdcall* PFN_GetIntegerv)(unsigned, int*);
typedef void (__stdcall* PFN_BindTexture)(unsigned, unsigned);
typedef void (__stdcall* PFN_GetTexImage)(unsigned, int, unsigned, unsigned, void*);
typedef void (__stdcall* PFN_PixelStorei)(unsigned, int);
typedef void (__stdcall* PFN_ActiveTexture)(unsigned);
typedef unsigned (__stdcall* PFN_GetError)();
static PFN_GetIntegerv gl_GetIntegerv; static PFN_BindTexture gl_BindTexture;
static PFN_GetTexImage gl_GetTexImage; static PFN_PixelStorei gl_PixelStorei;
static PFN_ActiveTexture gl_ActiveTexture; static PFN_GetError gl_GetError;
static bool g_glTried, g_glReady;
enum { S_GL_TEXTURE_2D = 0x0DE1, S_GL_TEXTURE_BINDING_2D = 0x8069, S_GL_RGB = 0x1907,
       S_GL_UNSIGNED_BYTE = 0x1401, S_GL_PACK_ALIGNMENT = 0x0D05, S_GL_PACK_ROW_LENGTH = 0x0D02,
       S_GL_PACK_SKIP_ROWS = 0x0D03, S_GL_PACK_SKIP_PIXELS = 0x0D04,
       S_GL_ACTIVE_TEXTURE = 0x84E0, S_GL_TEXTURE0 = 0x84C0, S_GL_PIXEL_PACK_BUFFER_BINDING = 0x88ED };

static bool LoadGL()
{
    if (g_glTried) return g_glReady;
    g_glTried = true;
    gl_GetIntegerv = (PFN_GetIntegerv)CODVR_GetGLProc("glGetIntegerv");
    gl_BindTexture = (PFN_BindTexture)CODVR_GetGLProc("glBindTexture");
    gl_GetTexImage = (PFN_GetTexImage)CODVR_GetGLProc("glGetTexImage");
    gl_PixelStorei = (PFN_PixelStorei)CODVR_GetGLProc("glPixelStorei");
    gl_ActiveTexture = (PFN_ActiveTexture)CODVR_GetGLProc("glActiveTexture");
    gl_GetError = (PFN_GetError)CODVR_GetGLProc("glGetError");
    g_glReady = gl_GetIntegerv && gl_BindTexture && gl_GetTexImage && gl_PixelStorei;
    if (!g_glReady) Log("snapshot: GL entry points missing -- disabled");
    return g_glReady;
}

static void WriteSidecar(const char* path, int eye, int w, int h, int ow, int oh)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || !f) return;
    fprintf(f, "snapshot %d eye %d (%s) why %d t %.3f serial %u\n", g_pendingSeq, eye,
            eye ? "RIGHT" : "LEFT", g_pendingWhy, g_pendingNowS, VR_TestFrameSerial());
    fprintf(f, "eye texture %dx%d, file %dx%d (1/%d)\n", w, h, ow, oh, g_cfg.scaleDiv);
    const VRInputState* in = VRInput_State();
    if (in) {
        fprintf(f, "input valid %d focused %d\n", in->valid, in->focused);
        for (int hd = 0; hd < VRI_HANDS; ++hd) {
            const VRHandState& s = in->hand[hd];
            fprintf(f, "hand %s track %d grip pos (%.3f %.3f %.3f) quat (%.4f %.4f %.4f %.4f) "
                       "aim quat (%.4f %.4f %.4f %.4f) trigger %.2f squeeze %.2f A/X %d B/Y %d stick %d\n",
                    hd == VRI_RIGHT ? "RIGHT" : "LEFT", (int)s.trackState,
                    s.gripPos[0], s.gripPos[1], s.gripPos[2],
                    s.gripQuat[0], s.gripQuat[1], s.gripQuat[2], s.gripQuat[3],
                    s.aimQuat[0], s.aimQuat[1], s.aimQuat[2], s.aimQuat[3],
                    s.trigger, s.squeeze, s.btnPrimary, s.btnSecondary, s.stickClick);
        }
    }
    VRNHDebug d; memset(&d, 0, sizeof(d));
    VRNH_GetDebug(&d);
    fprintf(f, "nativehands enabled %d pairs %u\n", d.enabled, d.pairs);
    for (int hd = 0; hd < VRI_HANDS; ++hd) {
        const VRNHHandDebug& s = d.hand[hd];
        fprintf(f, "nh %s engaged %d reason %d track %d target (%.3f %.3f %.3f) axis rows "
                   "(%.3f %.3f %.3f)(%.3f %.3f %.3f)(%.3f %.3f %.3f) grip (%.3f %.3f %.3f) "
                   "pairErr %.4fu %.3fdeg\n",
                hd == VRI_RIGHT ? "RIGHT" : "LEFT", s.engaged, s.reason, s.trackState,
                s.targetEyeish[0], s.targetEyeish[1], s.targetEyeish[2],
                s.targetAxisEyeish[0], s.targetAxisEyeish[1], s.targetAxisEyeish[2],
                s.targetAxisEyeish[3], s.targetAxisEyeish[4], s.targetAxisEyeish[5],
                s.targetAxisEyeish[6], s.targetAxisEyeish[7], s.targetAxisEyeish[8],
                s.gripEyeish[0], s.gripEyeish[1], s.gripEyeish[2], s.pairPosErrU, s.pairRotErrDeg);
    }
    const VRNHGunDebug& g = d.gun;
    float pitch = 0, yaw = 0;
    if (g.barrelValid) {
        pitch = (float)(asin(g.barrelRoom[2] < -1 ? -1 : g.barrelRoom[2] > 1 ? 1 : g.barrelRoom[2]) * 57.2957795);
        yaw = (float)(atan2(g.barrelRoom[1], g.barrelRoom[0]) * 57.2957795);
    }
    fprintf(f, "gun installed %d reason %d key %d grip (%.3f %.3f %.3f) twohand %d t %.2f "
               "barrel %d (%.3f %.3f %.3f) pitch %.1f yaw %.1f bore-from-sight %d muzzle %d (%.3f %.3f %.3f)\n",
            g.targetInstalled, g.reason, g.vertexCount,
            g.desiredGripEyeish[0], g.desiredGripEyeish[1], g.desiredGripEyeish[2],
            g.twoHandState, g.supportT, g.barrelValid,
            g.barrelRoom[0], g.barrelRoom[1], g.barrelRoom[2], pitch, yaw,
            g.boreFromSightLine, g.muzzleValid,
            g.muzzleEyeish[0], g.muzzleEyeish[1], g.muzzleEyeish[2]);
    float tr[3] = { 0, 0, 0 };
    VRNH_GetHandVisualTrim(VRI_RIGHT, tr);
    fprintf(f, "hand visual trim R pitch %.1f yaw %.1f roll %.1f\n", tr[0], tr[1], tr[2]);
    fclose(f);
}

void VRSnap_AfterEye(int eye)
{
    if (!g_pendingMask || eye < 0 || eye > 1) return;
    if (!(g_pendingMask & (1 << eye))) return;
    g_pendingMask &= ~(1 << eye);
    if (g_writeFails >= 5) return;
    if (!LoadGL()) { g_writeFails = 5; return; }
    const unsigned tex = VR_EyeTexture(eye);
    int w = 0, h = 0;
    VR_EyeTextureSizeFor(eye, &w, &h);
    if (!tex || w < 2 || h < 2) return;

    // Snapshot the GL state we touch.
    int oldActive = S_GL_TEXTURE0, oldTex = 0, oldAlign = 4, oldRowLen = 0, oldSkipR = 0, oldSkipP = 0, oldPbo = 0;
    if (gl_ActiveTexture) { gl_GetIntegerv(S_GL_ACTIVE_TEXTURE, &oldActive); gl_ActiveTexture(S_GL_TEXTURE0); }
    gl_GetIntegerv(S_GL_TEXTURE_BINDING_2D, &oldTex);
    gl_GetIntegerv(S_GL_PACK_ALIGNMENT, &oldAlign);
    gl_GetIntegerv(S_GL_PACK_ROW_LENGTH, &oldRowLen);
    gl_GetIntegerv(S_GL_PACK_SKIP_ROWS, &oldSkipR);
    gl_GetIntegerv(S_GL_PACK_SKIP_PIXELS, &oldSkipP);
    gl_GetIntegerv(S_GL_PIXEL_PACK_BUFFER_BINDING, &oldPbo);
    if (gl_GetError) gl_GetError();

    unsigned char* rgb = (unsigned char*)malloc((size_t)w * h * 3);
    int ok = rgb != nullptr;
    if (ok) {
        gl_PixelStorei(S_GL_PACK_ALIGNMENT, 1);
        gl_PixelStorei(S_GL_PACK_ROW_LENGTH, 0);
        gl_PixelStorei(S_GL_PACK_SKIP_ROWS, 0);
        gl_PixelStorei(S_GL_PACK_SKIP_PIXELS, 0);
        gl_BindTexture(S_GL_TEXTURE_2D, tex);
        gl_GetTexImage(S_GL_TEXTURE_2D, 0, S_GL_RGB, S_GL_UNSIGNED_BYTE, rgb);
        if (gl_GetError) { const unsigned e = gl_GetError(); if (e) { ok = 0; Log("snapshot: glGetTexImage error 0x%X", e); } }
    }
    // Restore.
    gl_BindTexture(S_GL_TEXTURE_2D, (unsigned)oldTex);
    gl_PixelStorei(S_GL_PACK_ALIGNMENT, oldAlign);
    gl_PixelStorei(S_GL_PACK_ROW_LENGTH, oldRowLen);
    gl_PixelStorei(S_GL_PACK_SKIP_ROWS, oldSkipR);
    gl_PixelStorei(S_GL_PACK_SKIP_PIXELS, oldSkipP);
    if (gl_ActiveTexture) gl_ActiveTexture((unsigned)oldActive);

    if (ok && oldPbo != 0) { ok = 0; Log("snapshot: a pixel pack buffer was bound -- skipped"); }
    if (ok) {
        // GL rows run bottom-up; flip while downsampling into a top-first image.
        const int div = g_cfg.scaleDiv;
        int ow = 0, oh = 0;
        VRSnap_Downsample(rgb, w, h, div, nullptr, &ow, &oh);
        unsigned char* shrunk = (unsigned char*)malloc((size_t)ow * oh * 3);
        if (shrunk) {
            VRSnap_Downsample(rgb, w, h, div, shrunk, &ow, &oh);
            for (int y = 0; y < oh / 2; ++y) {
                unsigned char* a = shrunk + (size_t)y * ow * 3;
                unsigned char* b = shrunk + (size_t)(oh - 1 - y) * ow * 3;
                for (int i = 0; i < ow * 3; ++i) { const unsigned char t = a[i]; a[i] = b[i]; b[i] = t; }
            }
            char path[MAX_PATH];
            sprintf_s(path, "%s\\snap_%03d_%c.png", g_dir, g_pendingSeq, eye ? 'R' : 'L');
            const int bytes = VRSnap_WritePng(path, shrunk, ow, oh);
            if (bytes > 0) {
                Log("snapshot: #%d eye %d why %d -> %s (%dx%d, %d bytes)", g_pendingSeq, eye,
                    g_pendingWhy, path, ow, oh, bytes);
                if (eye == 1 || !(g_pendingMask & 2)) {
                    sprintf_s(path, "%s\\snap_%03d.txt", g_dir, g_pendingSeq);
                    WriteSidecar(path, eye, w, h, ow, oh);
                }
            } else {
                ++g_writeFails;
                Log("snapshot: write FAILED %s", path);
            }
            free(shrunk);
        }
    }
    free(rgb);
}
