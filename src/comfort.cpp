// Comfort and input: the features that decide whether a VR mod is pleasant to
// play for an hour or makes you take the headset off after ten minutes.
//
// Three things live here:
//
//  * A TUNNELLING VIGNETTE. Simulator sickness comes from vection -- seeing
//    motion your inner ear cannot feel. Narrowing the field of view while the
//    player is moving or turning removes the peripheral optical flow that
//    drives it, and widening it back when they stop keeps the restriction from
//    being noticeable. This is the single most effective comfort option in VR
//    and every serious mod has one.
//
//  * RECENTER. Seated tracking drifts, and people start a session facing the
//    wrong way. Without a key for this the only fix is restarting the game.
//
//  * SNAP TURN. Rotating in discrete steps rather than sweeping past the whole
//    world is far gentler for people prone to sickness.
//
// The vignette is drawn with fixed-function OpenGL, which is all this engine's
// context guarantees -- no shaders anywhere.

#include "codvr.h"
#include "vr.h"

#include <math.h>
#include <stdio.h>

// ------------------------------------------------------------ GL plumbing

typedef void (WINAPI *PFN_GenTextures)(int, unsigned*);
typedef void (WINAPI *PFN_BindTexture)(unsigned, unsigned);
typedef void (WINAPI *PFN_TexImage2D)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*);
typedef void (WINAPI *PFN_TexParameteri)(unsigned,unsigned,int);
typedef void (WINAPI *PFN_Enable)(unsigned);
typedef void (WINAPI *PFN_Disable)(unsigned);
typedef unsigned char (WINAPI *PFN_IsEnabled)(unsigned);
typedef void (WINAPI *PFN_BlendFunc)(unsigned,unsigned);
typedef void (WINAPI *PFN_MatrixMode)(unsigned);
typedef void (WINAPI *PFN_PushMatrix)(void);
typedef void (WINAPI *PFN_PopMatrix)(void);
typedef void (WINAPI *PFN_LoadIdentity)(void);
typedef void (WINAPI *PFN_Ortho)(double,double,double,double,double,double);
typedef void (WINAPI *PFN_Begin)(unsigned);
typedef void (WINAPI *PFN_End)(void);
typedef void (WINAPI *PFN_Color4f)(float,float,float,float);
typedef void (WINAPI *PFN_TexCoord2f)(float,float);
typedef void (WINAPI *PFN_Vertex2f)(float,float);
typedef void (WINAPI *PFN_Viewport)(int,int,int,int);
typedef void (WINAPI *PFN_GenFramebuffers)(int, unsigned*);
typedef void (WINAPI *PFN_BindFramebuffer)(unsigned, unsigned);
typedef void (WINAPI *PFN_FramebufferTexture2D)(unsigned,unsigned,unsigned,unsigned,int);
typedef void (WINAPI *PFN_GetIntegerv)(unsigned, int*);
typedef void (WINAPI *PFN_ShadeModel)(unsigned);
typedef void (WINAPI *PFN_GetFloatv)(unsigned, float*);
typedef void (WINAPI *PFN_LoadMatrixf)(const float*);
typedef void (WINAPI *PFN_ActiveTexture)(unsigned);
typedef void (WINAPI *PFN_ColorMask)(unsigned char, unsigned char, unsigned char, unsigned char);

static PFN_GenTextures          gl_GenTextures;
static PFN_BindTexture          gl_BindTexture;
static PFN_TexImage2D           gl_TexImage2D;
static PFN_TexParameteri        gl_TexParameteri;
static PFN_Enable               gl_Enable;
static PFN_Disable              gl_Disable;
static PFN_IsEnabled            gl_IsEnabled;
static PFN_BlendFunc            gl_BlendFunc;
static PFN_MatrixMode           gl_MatrixMode;
static PFN_PushMatrix           gl_PushMatrix;
static PFN_PopMatrix            gl_PopMatrix;
static PFN_LoadIdentity         gl_LoadIdentity;
static PFN_Ortho                gl_Ortho;
static PFN_Begin                gl_Begin;
static PFN_End                  gl_End;
static PFN_Color4f              gl_Color4f;
static PFN_TexCoord2f           gl_TexCoord2f;
static PFN_Vertex2f             gl_Vertex2f;
static PFN_Viewport             gl_Viewport;
static PFN_GenFramebuffers      gl_GenFramebuffers;
static PFN_BindFramebuffer      gl_BindFramebuffer;
static PFN_FramebufferTexture2D gl_FramebufferTexture2D;
static PFN_GetIntegerv          gl_GetIntegerv;
static PFN_ShadeModel           gl_ShadeModel;
static PFN_GetFloatv            gl_GetFloatv;
static PFN_LoadMatrixf          gl_LoadMatrixf;
static PFN_ActiveTexture        gl_ActiveTexture;
static PFN_ColorMask            gl_ColorMask;

enum {
    GL_TEXTURE_2D_        = 0x0DE1,
    GL_BLEND_             = 0x0BE2,
    GL_SRC_ALPHA_         = 0x0302,
    GL_ZERO_              = 0x0000,
    GL_SRC_COLOR_         = 0x0300,
    GL_ONE_MINUS_SRC_ALPHA_ = 0x0303,
    GL_DEPTH_TEST_        = 0x0B71,
    GL_SCISSOR_TEST_      = 0x0C11,
    GL_CULL_FACE_         = 0x0B44,
    GL_LIGHTING_          = 0x0B50,
    GL_PROJECTION_        = 0x1701,
    GL_MODELVIEW_         = 0x1700,
    GL_QUADS_             = 0x0007,
    GL_RGBA_              = 0x1908,
    GL_RGBA8_             = 0x8058,
    GL_UNSIGNED_BYTE_     = 0x1401,
    GL_LINEAR_            = 0x2601,
    GL_CLAMP_TO_EDGE_     = 0x812F,
    GL_TEXTURE_MIN_FILTER_= 0x2801,
    GL_TEXTURE_MAG_FILTER_= 0x2800,
    GL_TEXTURE_WRAP_S_    = 0x2802,
    GL_TEXTURE_WRAP_T_    = 0x2803,
    GL_FRAMEBUFFER_       = 0x8D40,
    GL_COLOR_ATTACHMENT0_ = 0x8CE0,
    GL_VIEWPORT_          = 0x0BA2,
    GL_TEXTURE_BINDING_2D_= 0x8069,
    GL_QUAD_STRIP_        = 0x0008,
    GL_BLEND_SRC_         = 0x0BE1,
    GL_BLEND_DST_         = 0x0BE0,
    GL_SHADE_MODEL_       = 0x0B54,
    GL_SMOOTH_            = 0x1D01,
    GL_MATRIX_MODE_       = 0x0BA0,
    GL_CURRENT_COLOR_     = 0x0B00,
    GL_MODELVIEW_MATRIX_  = 0x0BA6,
    GL_PROJECTION_MATRIX_ = 0x0BA7,
    GL_ALPHA_TEST_        = 0x0BC0,
    GL_FOG_               = 0x0B60,
    GL_COLOR_WRITEMASK_   = 0x0C23,
    GL_ACTIVE_TEXTURE_    = 0x84E0,
    GL_TEXTURE0_          = 0x84C0,
    GL_TEXTURE1_          = 0x84C1,
    GL_DRAW_FRAMEBUFFER_BINDING_ = 0x8CA6,
    GL_READ_FRAMEBUFFER_BINDING_ = 0x8CAA,
    GL_DRAW_FRAMEBUFFER_2 = 0x8CA9,
    GL_READ_FRAMEBUFFER_2 = 0x8CA8
};

static bool g_glReady;
static bool g_glTried;

static bool LoadGL()
{
    if (g_glTried)
        return g_glReady;
    g_glTried = true;

    #define GETP(v, n) v = (decltype(v))CODVR_GetGLProc(n)
    GETP(gl_GenTextures,          "glGenTextures");
    GETP(gl_BindTexture,          "glBindTexture");
    GETP(gl_TexImage2D,           "glTexImage2D");
    GETP(gl_TexParameteri,        "glTexParameteri");
    GETP(gl_Enable,               "glEnable");
    GETP(gl_Disable,              "glDisable");
    GETP(gl_IsEnabled,            "glIsEnabled");
    GETP(gl_BlendFunc,            "glBlendFunc");
    GETP(gl_MatrixMode,           "glMatrixMode");
    GETP(gl_PushMatrix,           "glPushMatrix");
    GETP(gl_PopMatrix,            "glPopMatrix");
    GETP(gl_LoadIdentity,         "glLoadIdentity");
    GETP(gl_Ortho,                "glOrtho");
    GETP(gl_Begin,                "glBegin");
    GETP(gl_End,                  "glEnd");
    GETP(gl_Color4f,              "glColor4f");
    GETP(gl_TexCoord2f,           "glTexCoord2f");
    GETP(gl_Vertex2f,             "glVertex2f");
    GETP(gl_Viewport,             "glViewport");
    GETP(gl_GenFramebuffers,      "glGenFramebuffers");
    GETP(gl_BindFramebuffer,      "glBindFramebuffer");
    GETP(gl_FramebufferTexture2D, "glFramebufferTexture2D");
    GETP(gl_GetIntegerv,          "glGetIntegerv");
    GETP(gl_ShadeModel,           "glShadeModel");
    GETP(gl_GetFloatv,            "glGetFloatv");
    GETP(gl_LoadMatrixf,          "glLoadMatrixf");
    GETP(gl_ColorMask,            "glColorMask");
    GETP(gl_ActiveTexture,        "glActiveTexture");
    if (!gl_ActiveTexture)
        GETP(gl_ActiveTexture,    "glActiveTextureARB");
    #undef GETP

    g_glReady = gl_GenTextures && gl_BindTexture && gl_TexImage2D && gl_Begin
             && gl_End && gl_Vertex2f && gl_TexCoord2f && gl_Color4f
             && gl_BindFramebuffer && gl_FramebufferTexture2D && gl_Ortho;
    if (!g_glReady)
        Log("comfort: fixed-function GL unavailable; vignette disabled");
    return g_glReady;
}

// ------------------------------------------------------------- settings

static bool  g_vignetteOn      = true;
static float g_vignetteStrength = 1.0f;   // 0..1 scale on how far it closes
static float g_speedThreshold   = 40.0f;  // units/sec where it starts closing
static float g_turnThreshold    = 45.0f;  // deg/sec where it starts closing
// HOME, not F9: the user's own game config binds F9 to "loadgame quicksave",
// so the old default LOADED A SAVE whenever recenter was pressed. Recenter is
// also on the off-hand stick click now, which is the primary path in VR.
static int   g_recenterKey      = VK_HOME;
static int   g_snapTurnDeg      = 0;      // 0 disables snap turn
static int   g_snapLeftKey      = 0;
static int   g_snapRightKey     = 0;
static float g_snapMouseScale   = 4.0f;   // mouse counts per degree

void Comfort_SetVignette(bool on)          { g_vignetteOn = on; }
void Comfort_SetVignetteStrength(float s)  { if (s >= 0.0f && s <= 1.0f) g_vignetteStrength = s; }
void Comfort_SetSpeedThreshold(float u)    { if (u > 0.0f) g_speedThreshold = u; }
void Comfort_SetTurnThreshold(float d)     { if (d > 0.0f) g_turnThreshold = d; }
void Comfort_SetRecenterKey(int vk)        { g_recenterKey = vk; }
void Comfort_SetSnapTurn(int deg)          { g_snapTurnDeg = deg; }
void Comfort_SetSnapKeys(int l, int r)     { g_snapLeftKey = l; g_snapRightKey = r; }
void Comfort_SetSnapMouseScale(float s)    { if (s > 0.0f) g_snapMouseScale = s; }

// ----------------------------------------------------------- the aperture
//
// The tunnel is drawn as geometry rather than a fixed texture, because what
// should change with speed is the APERTURE RADIUS, not the opacity. Fading a
// fixed-size mask in and out just greys the periphery; shrinking the opening is
// what actually removes the peripheral motion that causes sickness, and it is
// what the research on field-of-view restrictors describes.
//
// Published restrictors close to roughly 0.6 of the screen radius at full
// motion with a soft inner edge, so that is what the fully-closed end maps to.
enum { VIG_SEGMENTS = 96 };

static unsigned g_vigFbo;

// Aperture for the current amount: 0 = wide open (entirely off screen), 1 =
// fully closed to the published tunnel size.
static void ApertureRadii(float amount, float* inner, float* outer)
{
    // Wide open sits past the corner (1.414 in normalised units) so nothing is
    // visible at all; closed matches the 0.25 / 0.65 tunnel from the literature.
    const float in0 = 1.75f, in1 = 0.25f;
    const float out0 = 2.20f, out1 = 0.65f;
    *inner = in0 + (in1 - in0) * amount;
    *outer = out0 + (out1 - out0) * amount;
}

// ------------------------------------------------------------ the amount

static float g_vigCurrent;   // smoothed 0..1
// HMD brightness (2026-09-06): a multiplicative gain on the finished eye image,
// applied in the same pass as the vignette. The engine's r_gamma only reaches
// the headset through texture uploads (r_ignorehwgamma) and needs a restart;
// this one is immediate and linear, so 0.7 means 70 % of every pixel.
static float g_brightness = 1.0f;
void  Comfort_SetBrightness(float b) { g_brightness = b < 0.1f ? 0.1f : b > 1.0f ? 1.0f : b; }
float Comfort_Brightness() { return g_brightness; }

// How closed the tunnel should be for a given amount of virtual motion.
//
// EXPLICIT dead zone: nothing happens below the configured threshold, the
// tunnel is fully closed at four times it, linear in between. This is what
// the config file has always CLAIMED ("how much motion it takes to start
// closing"); the old formula actually started closing at any speed above
// zero and merely happened to stay off screen at low amounts because the
// aperture geometry began past the corners.
static float TargetFromMotion(float speed, float turn)
{
    if (!g_vignetteOn || g_vignetteStrength <= 0.0f)
        return 0.0f;

    const float bySpeed = (speed - g_speedThreshold) / (g_speedThreshold * 3.0f);
    const float byTurn  = (turn  - g_turnThreshold)  / (g_turnThreshold  * 3.0f);
    float t = bySpeed > byTurn ? bySpeed : byTurn;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * g_vignetteStrength;
}

static float TargetVignette()
{
    float speed = 0.0f, turn = 0.0f;
    VR_GetPlayerMotion(&speed, &turn);
    return TargetFromMotion(speed, turn);
}

float Comfort_VignetteAmount() { return g_vigCurrent; }

// One smoothing step over an explicit dt. Exponential toward the target with
// separate time constants: closing fast (~62 ms) matters for protection,
// opening slow (~225 ms) keeps the release from being jarring. These match
// the feel of the old fixed per-frame constants at 72 Hz -- which drifted
// 1.67x faster at 120 Hz, because a per-frame constant is a frame-RATE
// constant, not a time constant.
static float SmoothStep(float current, float target, float dt)
{
    if (dt < 0.0f)  dt = 0.0f;
    if (dt > 0.1f)  dt = 0.1f;          // a stall must not teleport the state
    const float tau = (target > current) ? 0.062f : 0.225f;
    float v = current + (target - current) * (1.0f - expf(-dt / tau));
    if (v < 0.001f) v = 0.0f;
    return v;
}

// Advance the smoothed amount using real elapsed time.
//
// Sub-millisecond intervals are not real frames -- the frame loop is paced by
// the headset (11-14 ms) and even a 240 fps flat run is 4 ms. They only occur
// when a test drives Comfort_Update in a tight loop, so they get the nominal
// 72 Hz step: tight loops behave exactly like the old per-frame constants,
// while genuine frame intervals stay frame-rate independent.
void Comfort_Update()
{
    static double lastTime = 0.0;
    const double now = CODVR_NowSeconds();
    float dt = (lastTime == 0.0) ? (1.0f / 72.0f) : (float)(now - lastTime);
    lastTime = now;
    if (dt < 0.001f)
        dt = 1.0f / 72.0f;
    g_vigCurrent = SmoothStep(g_vigCurrent, TargetVignette(), dt);
}

float Comfort_TestStep(float current, float target, float dtSeconds)
{
    return SmoothStep(current, target, dtSeconds);
}

float Comfort_TestTarget(float speedUnitsPerSec, float turnDegPerSec)
{
    return TargetFromMotion(speedUnitsPerSec, turnDegPerSec);
}

// ------------------------------------------------------------- rendering

// Draw the tunnel over a finished eye image. EVERY piece of GL state the draw
// touches is snapshotted and restored -- the engine owns this context and
// keeps drawing with it the moment we hand it back, and restoring most state
// is worth nothing if the one leaked bit is the one the next draw depends on.
// Two leaks survived the previous version (the matrix MODE was forced to
// MODELVIEW instead of restored, and the current colour was forced to white),
// and several states it could collide with were never touched at all (alpha
// test, fog, the second texture unit, the colour mask).
bool Comfort_DrawVignette(int eye)
{
    const bool dim = g_brightness < 0.999f;
    if (g_vigCurrent <= 0.002f && !dim)
        return true;                      // nothing to draw, not a failure
    if (!LoadGL())
        return false;

    const unsigned tex = VR_EyeTexture(eye);
    if (!tex)
        return false;
    int w = 0, h = 0;
    VR_EyeTextureSizeFor(eye, &w, &h);
    if (w < 2 || h < 2)
        return false;

    if (!g_vigFbo)
        gl_GenFramebuffers(1, &g_vigFbo);

    // --- snapshot ---
    int   oldViewport[4] = { 0, 0, 0, 0 };
    int   oldMatrixMode  = GL_MODELVIEW_;
    int   oldColorMask[4] = { 1, 1, 1, 1 };
    int   oldDrawFbo = 0, oldReadFbo = 0;
    int   oldActive  = GL_TEXTURE0_;
    float oldColor[4] = { 1, 1, 1, 1 };
    float oldProj[16], oldModel[16];
    const bool haveMatrixSnapshot = (gl_GetFloatv && gl_LoadMatrixf);

    if (gl_GetIntegerv) {
        gl_GetIntegerv(GL_VIEWPORT_, oldViewport);
        gl_GetIntegerv(GL_MATRIX_MODE_, &oldMatrixMode);
        gl_GetIntegerv(GL_COLOR_WRITEMASK_, oldColorMask);
        gl_GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING_, &oldDrawFbo);
        gl_GetIntegerv(GL_READ_FRAMEBUFFER_BINDING_, &oldReadFbo);
        if (gl_ActiveTexture)
            gl_GetIntegerv(GL_ACTIVE_TEXTURE_, &oldActive);
    }
    if (gl_GetFloatv)
        gl_GetFloatv(GL_CURRENT_COLOR_, oldColor);
    if (haveMatrixSnapshot) {
        gl_GetFloatv(GL_PROJECTION_MATRIX_, oldProj);
        gl_GetFloatv(GL_MODELVIEW_MATRIX_,  oldModel);
    }

    const bool hadBlend   = gl_IsEnabled && gl_IsEnabled(GL_BLEND_) != 0;
    const bool hadDepth   = gl_IsEnabled && gl_IsEnabled(GL_DEPTH_TEST_) != 0;
    const bool hadScissor = gl_IsEnabled && gl_IsEnabled(GL_SCISSOR_TEST_) != 0;
    const bool hadCull    = gl_IsEnabled && gl_IsEnabled(GL_CULL_FACE_) != 0;
    const bool hadLight   = gl_IsEnabled && gl_IsEnabled(GL_LIGHTING_) != 0;
    const bool hadAlpha   = gl_IsEnabled && gl_IsEnabled(GL_ALPHA_TEST_) != 0;
    const bool hadFog     = gl_IsEnabled && gl_IsEnabled(GL_FOG_) != 0;
    // The blend FUNCTION matters as much as whether blending is on, and the
    // shade model decides whether the soft edge is a gradient or a hard ring.
    int oldBlendSrc = GL_SRC_ALPHA_, oldBlendDst = GL_ONE_MINUS_SRC_ALPHA_;
    int oldShade = GL_SMOOTH_;
    if (gl_GetIntegerv) {
        gl_GetIntegerv(GL_BLEND_SRC_, &oldBlendSrc);
        gl_GetIntegerv(GL_BLEND_DST_, &oldBlendDst);
        gl_GetIntegerv(GL_SHADE_MODEL_, &oldShade);
    }

    // Texturing is disabled on BOTH units for the draw: the engine can leave
    // the lightmap unit active or enabled at swap time, and any enabled unit
    // would modulate the "untextured" rings with whatever texture -- and
    // whatever ALPHA -- happens to be bound there. Bindings themselves are
    // never touched, so they need no restore.
    unsigned char hadTexUnit[2] = { 0, 0 };
    if (gl_ActiveTexture) {
        for (int u = 0; u < 2; ++u) {
            gl_ActiveTexture(GL_TEXTURE0_ + (unsigned)u);
            hadTexUnit[u] = gl_IsEnabled ? gl_IsEnabled(GL_TEXTURE_2D_) : 0;
            gl_Disable(GL_TEXTURE_2D_);
        }
        gl_ActiveTexture(GL_TEXTURE0_);
    } else {
        hadTexUnit[0] = gl_IsEnabled ? gl_IsEnabled(GL_TEXTURE_2D_) : 0;
        gl_Disable(GL_TEXTURE_2D_);
    }

    // --- set up and draw ---
    gl_BindFramebuffer(GL_FRAMEBUFFER_, g_vigFbo);
    gl_FramebufferTexture2D(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_,
                            GL_TEXTURE_2D_, tex, 0);
    gl_Viewport(0, 0, w, h);

    if (hadScissor) gl_Disable(GL_SCISSOR_TEST_);
    if (hadDepth)   gl_Disable(GL_DEPTH_TEST_);
    if (hadCull)    gl_Disable(GL_CULL_FACE_);
    if (hadLight)   gl_Disable(GL_LIGHTING_);
    if (hadAlpha)   gl_Disable(GL_ALPHA_TEST_);   // would punch holes in the soft edge
    if (hadFog)     gl_Disable(GL_FOG_);          // would tint the "black" tunnel
    gl_Enable(GL_BLEND_);
    gl_BlendFunc(GL_SRC_ALPHA_, GL_ONE_MINUS_SRC_ALPHA_);
    if (gl_ColorMask)  gl_ColorMask(1, 1, 1, 1);
    if (gl_ShadeModel) gl_ShadeModel(GL_SMOOTH_);

    gl_MatrixMode(GL_PROJECTION_);
    if (!haveMatrixSnapshot) gl_PushMatrix();
    gl_LoadIdentity();
    gl_Ortho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);   // centred, so radii are direct
    gl_MatrixMode(GL_MODELVIEW_);
    if (!haveMatrixSnapshot) gl_PushMatrix();
    gl_LoadIdentity();

    // Brightness first: multiply the whole image by the gain. GL_ZERO /
    // GL_SRC_COLOR makes dst = src * dst, a pure gain with black staying
    // black -- nothing is lifted or tinted. The rings below then blend over
    // the dimmed image with the ordinary alpha function.
    if (dim) {
        gl_BlendFunc(GL_ZERO_, GL_SRC_COLOR_);
        gl_Color4f(g_brightness, g_brightness, g_brightness, 1.0f);
        gl_Begin(GL_QUADS_);
        gl_Vertex2f(-2.5f, -2.5f); gl_Vertex2f(2.5f, -2.5f);
        gl_Vertex2f(2.5f, 2.5f);   gl_Vertex2f(-2.5f, 2.5f);
        gl_End();
        gl_BlendFunc(GL_SRC_ALPHA_, GL_ONE_MINUS_SRC_ALPHA_);
    }
    if (g_vigCurrent > 0.002f) {
    // Draw the tunnel as two concentric rings in normalised [-1,1] space:
    // a soft edge from the clear aperture out to full black, then solid black
    // from there past the corners. No texture is involved, so nothing depends
    // on the texture environment the engine happened to leave set.
    float rIn = 0.0f, rOut = 0.0f;
    ApertureRadii(g_vigCurrent, &rIn, &rOut);
    const float rFar = 2.4f;              // safely past the corner at 1.414

    // The eye is not perfectly square, so scale the circle to stay circular.
    float ax = 1.0f, ay = 1.0f;
    if (w > h) ax = (float)w / (float)h;
    else       ay = (float)h / (float)w;

    // Soft edge: transparent at the aperture, opaque at the outer radius.
    gl_Begin(GL_QUAD_STRIP_);
    for (int i = 0; i <= VIG_SEGMENTS; ++i) {
        const float a = (float)i / VIG_SEGMENTS * 6.28318531f;
        const float ca = cosf(a) / ax, sa = sinf(a) / ay;
        gl_Color4f(0.0f, 0.0f, 0.0f, 0.0f);
        gl_Vertex2f(ca * rIn, sa * rIn);
        gl_Color4f(0.0f, 0.0f, 0.0f, 1.0f);
        gl_Vertex2f(ca * rOut, sa * rOut);
    }
    gl_End();

    // Solid surround out past the corners.
    gl_Begin(GL_QUAD_STRIP_);
    for (int i = 0; i <= VIG_SEGMENTS; ++i) {
        const float a = (float)i / VIG_SEGMENTS * 6.28318531f;
        const float ca = cosf(a) / ax, sa = sinf(a) / ay;
        gl_Color4f(0.0f, 0.0f, 0.0f, 1.0f);
        gl_Vertex2f(ca * rOut, sa * rOut);
        gl_Color4f(0.0f, 0.0f, 0.0f, 1.0f);
        gl_Vertex2f(ca * rFar, sa * rFar);
    }
    gl_End();
    }

    // --- restore, exactly ---
    if (haveMatrixSnapshot) {
        gl_MatrixMode(GL_PROJECTION_);
        gl_LoadMatrixf(oldProj);
        gl_MatrixMode(GL_MODELVIEW_);
        gl_LoadMatrixf(oldModel);
    } else {
        gl_MatrixMode(GL_MODELVIEW_);
        gl_PopMatrix();
        gl_MatrixMode(GL_PROJECTION_);
        gl_PopMatrix();
    }
    gl_MatrixMode((unsigned)oldMatrixMode);   // the engine's mode, not our guess
    gl_Color4f(oldColor[0], oldColor[1], oldColor[2], oldColor[3]);
    if (gl_ColorMask)
        gl_ColorMask(oldColorMask[0] != 0, oldColorMask[1] != 0,
                     oldColorMask[2] != 0, oldColorMask[3] != 0);

    if (gl_ActiveTexture) {
        for (int u = 0; u < 2; ++u) {
            gl_ActiveTexture(GL_TEXTURE0_ + (unsigned)u);
            if (hadTexUnit[u]) gl_Enable(GL_TEXTURE_2D_);
            else               gl_Disable(GL_TEXTURE_2D_);
        }
        gl_ActiveTexture((unsigned)oldActive);
    } else if (hadTexUnit[0]) {
        gl_Enable(GL_TEXTURE_2D_);
    }

    if (gl_BlendFunc)  gl_BlendFunc((unsigned)oldBlendSrc, (unsigned)oldBlendDst);
    if (gl_ShadeModel) gl_ShadeModel((unsigned)oldShade);
    if (!hadBlend)  gl_Disable(GL_BLEND_);
    if (hadDepth)   gl_Enable(GL_DEPTH_TEST_);
    if (hadCull)    gl_Enable(GL_CULL_FACE_);
    if (hadLight)   gl_Enable(GL_LIGHTING_);
    if (hadAlpha)   gl_Enable(GL_ALPHA_TEST_);
    if (hadFog)     gl_Enable(GL_FOG_);
    gl_BindFramebuffer(GL_READ_FRAMEBUFFER_2, (unsigned)oldReadFbo);
    gl_BindFramebuffer(GL_DRAW_FRAMEBUFFER_2, (unsigned)oldDrawFbo);
    if (hadScissor) gl_Enable(GL_SCISSOR_TEST_);
    if (gl_GetIntegerv)
        gl_Viewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);

    static bool logged = false;
    if (!logged) {
        logged = true;
        Log("comfort: vignette active (%.0f%% closed)", g_vigCurrent * 100.0f);
    }
    return true;
}

// ------------------------------------------------------ frame rate watch
//
// IMPORTANT MEASUREMENT FACT: while a VR session is live, xrWaitFrame paces
// the game loop to the headset's cadence -- under paired rendering, to TWO
// game frames per display interval (one per eye, one wait per pair).
// SwapBuffers-per-second on a healthy system is therefore about TWICE the
// headset rate, and below ONE TIMES it the engine cannot even feed a single
// eye per display frame. The pairing-aware verdict (PASS/MARGINAL/FAIL
// against 2x target) lives in vr.cpp's perf block; the warning here keeps
// the old meaning -- catastrophically below target -- where lowering
// supersample is the right advice.
static double g_fpsWindowStart;
static int    g_fpsFrames;
static float  g_fps;
static bool   g_fpsWarned;

float Comfort_FrameRate() { return g_fps; }

void Comfort_NoteFrame()
{
    const double now = CODVR_NowSeconds();
    if (g_fpsWindowStart == 0.0) {
        g_fpsWindowStart = now;
        g_fpsFrames = 0;
        return;
    }
    ++g_fpsFrames;
    const double elapsed = now - g_fpsWindowStart;
    if (elapsed < 2.0)
        return;

    g_fps = (float)(g_fpsFrames / elapsed);
    g_fpsWindowStart = now;
    g_fpsFrames = 0;

    VRPerfStats st = {};
    VR_GetPerfStats(&st);

    static int reports = 0;
    if (reports < 3) {
        ++reports;
        if (st.xrTargetHz > 1.0f)
            Log("perf: %.0f game frames/s against a %.0f Hz headset target "
                "(paired rendering wants ~%.0f/s: two game frames per XR "
                "frame)", g_fps, st.xrTargetHz, st.xrTargetHz * 2.0f);
        else
            Log("perf: presenting %.0f/s (no VR pacing active)", g_fps);
    }

    // The one warning that means something: the engine cannot even keep the
    // headset's own cadence. That IS a load problem, and supersampling is the
    // load that can be turned down.
    if (!g_fpsWarned && st.xrTargetHz > 1.0f && g_fps > 1.0f
        && g_fps < st.xrTargetHz * 0.9f) {
        g_fpsWarned = true;
        Log("");
        Log("perf: WARNING -- presenting %.0f/s against a %.0f Hz headset "
            "target: the engine is not keeping the headset's pace.",
            g_fps, st.xrTargetHz);
        Log("  Lower 'supersample' in codvr.cfg (2.25 -> 1.75 -> 1.5); it is by");
        Log("  far the most expensive setting.");
        Log("");
    }
}

// ----------------------------------------------------------------- input

// Edge-triggered so holding a key fires once. Only acts while the game has
// focus, or the mod would respond to typing in other applications.
static bool KeyPressedEdge(int vk, bool* prev)
{
    if (!vk) { *prev = false; return false; }
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool fired = down && !*prev;
    *prev = down;
    return fired;
}

static bool GameHasFocus()
{
    const HWND fg = GetForegroundWindow();
    if (!fg)
        return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// Snap turn drives the game's own aim, so the weapon turns with the view rather
// than the two drifting apart. The engine reads relative mouse motion, so a
// synthetic horizontal movement is the honest way to turn it.
static void InjectYaw(int degrees)
{
    const int counts = (int)(degrees * g_snapMouseScale);
    if (!counts)
        return;
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dx = counts;
    in.mi.dy = 0;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
}

void Comfort_PollInput()
{
    static bool prevRecenter = false, prevLeft = false, prevRight = false;

    if (!GameHasFocus()) {
        prevRecenter = prevLeft = prevRight = false;
        return;
    }

    if (KeyPressedEdge(g_recenterKey, &prevRecenter))
        VR_Recenter();

    if (g_snapTurnDeg > 0) {
        if (KeyPressedEdge(g_snapLeftKey,  &prevLeft))
            InjectYaw(-g_snapTurnDeg);
        if (KeyPressedEdge(g_snapRightKey, &prevRight))
            InjectYaw(+g_snapTurnDeg);
    }
}

// ------------------------------------------------------------- self test

// Maps a config name like "F9" or "HOME" to a virtual-key code, so the keys are
// settable without anyone having to look up hex codes.
int Comfort_ParseKey(const char* name)
{
    if (!name || !*name)
        return 0;
    if (!_stricmp(name, "none") || !_stricmp(name, "off"))
        return 0;
    if ((name[0] == 'F' || name[0] == 'f') && name[1] >= '1' && name[1] <= '9') {
        int n = atoi(name + 1);
        if (n >= 1 && n <= 24)
            return VK_F1 + (n - 1);
    }
    if (!_stricmp(name, "home"))     return VK_HOME;
    if (!_stricmp(name, "end"))      return VK_END;
    if (!_stricmp(name, "insert"))   return VK_INSERT;
    if (!_stricmp(name, "delete"))   return VK_DELETE;
    if (!_stricmp(name, "pageup"))   return VK_PRIOR;
    if (!_stricmp(name, "pagedown")) return VK_NEXT;
    if (!_stricmp(name, "left"))     return VK_LEFT;
    if (!_stricmp(name, "right"))    return VK_RIGHT;
    if (!_stricmp(name, "up"))       return VK_UP;
    if (!_stricmp(name, "down"))     return VK_DOWN;
    if (!_stricmp(name, "tab"))      return VK_TAB;
    if (name[1] == '\0') {
        char c = name[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            return (int)c;
    }
    return 0;
}

// Testing seam: drive the vignette without a game running.
void Comfort_TestSetVignette(float v) { g_vigCurrent = v; }

// Testing seam: the aperture the current amount maps to.
void Comfort_TestAperture(float amount, float* inner, float* outer)
{
    ApertureRadii(amount, inner, outer);
}
