// Pure projection / frustum math for the VR renderer. Header-only, no
// OpenGL, no OpenXR, no Windows: vr.cpp uses it in production and
// tests/vr_safetest.cpp includes it directly, so every formula here is
// verified offline against an independent implementation of the Khronos
// reference construction (hello_xr's XrMatrix4x4f_CreateProjection).
//
// Matrices are OpenGL column-major float[16]: element (row r, col c) is
// m[c*4 + r]. Eye space is the GL convention: +X right, +Y up, the camera
// looks along -Z. Tangents are signed view-plane extents at unit distance --
// tanLeft/tanDown negative, tanRight/tanUp positive for any normal view.
#pragma once

#include <math.h>

// The standard off-axis perspective projection -- the GRAPHICS_OPENGL branch
// of Khronos XrMatrix4x4f_CreateProjection -- from signed edge tangents.
// farZ <= nearZ selects an infinite far plane (the Khronos convention).
static inline void VRM_ProjectionFromTangents(
    float tanLeft, float tanRight, float tanUp, float tanDown,
    float nearZ, float farZ, float out[16])
{
    const float w = tanRight - tanLeft;
    const float h = tanUp - tanDown;
    for (int i = 0; i < 16; ++i)
        out[i] = 0.0f;
    out[0]  = 2.0f / w;
    out[5]  = 2.0f / h;
    out[8]  = (tanRight + tanLeft) / w;
    out[9]  = (tanUp + tanDown) / h;
    out[11] = -1.0f;
    if (farZ <= nearZ) {
        out[10] = -1.0f;                 // infinite far plane
        out[14] = -2.0f * nearZ;
    } else {
        out[10] = -(farZ + nearZ) / (farZ - nearZ);
        out[14] = -2.0f * farZ * nearZ / (farZ - nearZ);
    }
}

// Same, from the raw OpenXR fov angles (radians, angleLeft/angleDown
// negative), exactly like XrMatrix4x4f_CreateProjectionFov.
static inline void VRM_ProjectionFromFovRad(
    float angleLeft, float angleRight, float angleUp, float angleDown,
    float nearZ, float farZ, float out[16])
{
    VRM_ProjectionFromTangents(tanf(angleLeft), tanf(angleRight),
                               tanf(angleUp), tanf(angleDown),
                               nearZ, farZ, out);
}

// Is this matrix structurally a perspective projection? The gate is
// deliberately shape-based so it can be decided per call with no history:
// the w row must pick -z (m[11] == -1, m[15] == 0), the focal terms must be
// positive with no shear, the translation column empty, and the depth row
// must look like -(f+n)/(f-n) / -2fn/(f-n) (both negative for 0 < n <= f,
// including the infinite-far limit m[10] == -1). Orthographic matrices
// (m[15] == 1, m[11] == 0) and every rigid modelview transform fail this.
static inline int VRM_IsPerspective(const float m[16])
{
    const float eps = 1e-4f;
    if (fabsf(m[11] + 1.0f) > eps) return 0;
    if (fabsf(m[15]) > eps) return 0;
    if (!(m[0] > eps) || !(m[5] > eps)) return 0;
    if (fabsf(m[1]) > eps || fabsf(m[4]) > eps) return 0;   // no shear
    if (fabsf(m[2]) > eps || fabsf(m[6]) > eps) return 0;   // clean depth row
    if (fabsf(m[3]) > eps || fabsf(m[7]) > eps) return 0;
    if (fabsf(m[12]) > eps || fabsf(m[13]) > eps) return 0; // no translation
    if (!(m[14] < -eps)) return 0;
    if (!(m[10] < eps)) return 0;
    return 1;
}

// Recognises the 2D pass's orthographic projection, for tests and logging.
static inline int VRM_IsOrtho(const float m[16])
{
    const float eps = 1e-4f;
    return fabsf(m[15] - 1.0f) <= eps && fabsf(m[11]) <= eps
        && fabsf(m[3]) <= eps && fabsf(m[7]) <= eps
        && m[0] != 0.0f && m[5] != 0.0f;
}

// Install new lateral optics into an existing perspective matrix, leaving the
// depth mapping bit-identical. The key identity: with r = n*tanRight and
// l = n*tanLeft, the lateral terms 2n/(r-l) and (r+l)/(r-l) collapse to
// 2/(tanRight-tanLeft) and (tanRight+tanLeft)/(tanRight-tanLeft) -- the near
// plane cancels out. So the engine keeps whatever near/far (finite or
// infinite) it chose, and only the frustum's lateral shape changes.
static inline void VRM_ReplaceLateralOptics(
    const float engine[16], float tanLeft, float tanRight,
    float tanUp, float tanDown, float out[16])
{
    const float w = tanRight - tanLeft;
    const float h = tanUp - tanDown;
    for (int i = 0; i < 16; ++i)
        out[i] = engine[i];
    out[0] = 2.0f / w;
    out[1] = 0.0f;
    out[4] = 0.0f;
    out[5] = 2.0f / h;
    out[8] = (tanRight + tanLeft) / w;
    out[9] = (tanUp + tanDown) / h;
}

// Recover the signed edge tangents from a perspective matrix:
// m[0] = 2/(r-l), m[8] = (r+l)/(r-l)  =>  r = (m8+1)/m0, l = (m8-1)/m0.
static inline void VRM_ExtractTangents(const float m[16],
    float* tanLeft, float* tanRight, float* tanUp, float* tanDown)
{
    *tanRight = (m[8] + 1.0f) / m[0];
    *tanLeft  = (m[8] - 1.0f) / m[0];
    *tanUp    = (m[9] + 1.0f) / m[5];
    *tanDown  = (m[9] - 1.0f) / m[5];
}

// Recover near/far from the depth row. An infinite far plane reports
// *farZ = 0 (the m[10] == -1 case has no finite far to name).
static inline void VRM_ExtractNearFar(const float m[16], float* nearZ, float* farZ)
{
    const float dn = m[10] - 1.0f;
    const float df = m[10] + 1.0f;
    *nearZ = (fabsf(dn) > 1e-9f) ? m[14] / dn : 0.0f;
    *farZ  = (fabsf(df) > 1e-6f) ? m[14] / df : 0.0f;
}

// Full projective transform of an eye-space point to NDC, for tests.
static inline void VRM_ProjectPoint(const float m[16],
    float x, float y, float z, float ndc[3])
{
    const float cx = m[0]*x + m[4]*y + m[8]*z  + m[12];
    const float cy = m[1]*x + m[5]*y + m[9]*z  + m[13];
    const float cz = m[2]*x + m[6]*y + m[10]*z + m[14];
    const float cw = m[3]*x + m[7]*y + m[11]*z + m[15];
    ndc[0] = cx / cw;
    ndc[1] = cy / cw;
    ndc[2] = cz / cw;
}

// Does a symmetric half-angle pair contain an asymmetric frustum? Used to
// prove the culling superset really is a superset of both eye frusta.
static inline int VRM_SymmetricContains(float halfXRad, float halfYRad,
    float angleLeft, float angleRight, float angleUp, float angleDown)
{
    const float tx = tanf(halfXRad), ty = tanf(halfYRad);
    return tanf(fabsf(angleLeft))  <= tx + 1e-6f
        && tanf(fabsf(angleRight)) <= tx + 1e-6f
        && tanf(fabsf(angleUp))    <= ty + 1e-6f
        && tanf(fabsf(angleDown))  <= ty + 1e-6f;
}

// Where the optical axis (view direction, tangent 0) sits inside the rendered
// image, as a pixel offset from the image centre. The flat HUD is drawn at
// identical pixel positions in both eyes; under an asymmetric projection the
// image centre is NOT the view axis, so an unshifted HUD element would sit at
// different view angles per eye (~15 deg each way on Quest 3 -- a ~30 deg
// binocular divergence, unfusable). Positive dx moves content right,
// positive dy up, both in GL viewport space.
static inline void VRM_PrincipalPointShiftPx(
    float tanLeft, float tanRight, float tanUp, float tanDown,
    int viewW, int viewH, int* dxPx, int* dyPx)
{
    const float w = tanRight - tanLeft;
    const float h = tanUp - tanDown;
    const float fx = (w != 0.0f) ? (-tanLeft / w - 0.5f) : 0.0f;
    const float fy = (h != 0.0f) ? (-tanDown / h - 0.5f) : 0.0f;
    *dxPx = (int)(fx * (float)viewW + (fx >= 0.0f ? 0.5f : -0.5f));
    *dyPx = (int)(fy * (float)viewH + (fy >= 0.0f ? 0.5f : -0.5f));
}

// ------------------------------------------------------------ pose algebra
//
// Shared by the head path (vr.cpp keeps its own private copies, pinned equal
// by tests) and the controller path (vr_input/vr_weapon), so hand poses go
// through EXACTLY the same OpenXR -> Quake convention as the head:
//   OpenXR : right-handed, +X right, +Y up, forward -Z, metres
//   Quake  : +X forward, +Y left, +Z up; axes are three ROWS forward/left/up

static inline void VRM_XrToQuakeVec(const float v[3], float out[3])
{
    out[0] = -v[2];
    out[1] = -v[0];
    out[2] =  v[1];
}

// Quaternion (x,y,z,w) -> Quake row basis (forward/left/up).
static inline void VRM_QuatToQuakeAxis(const float q[4], float axis[9])
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float right[3] = { 1 - 2*(y*y + z*z),     2*(x*y + z*w),     2*(x*z - y*w) };
    const float up[3]    = {     2*(x*y - z*w), 1 - 2*(x*x + z*z),     2*(y*z + x*w) };
    const float back[3]  = {     2*(x*z + y*w),     2*(y*z - x*w), 1 - 2*(x*x + y*y) };
    const float fwd[3]   = { -back[0], -back[1], -back[2] };
    const float left[3]  = { -right[0], -right[1], -right[2] };
    VRM_XrToQuakeVec(fwd,  &axis[0]);
    VRM_XrToQuakeVec(left, &axis[3]);
    VRM_XrToQuakeVec(up,   &axis[6]);
}

// ----------------------------------------------------- room/eye conversion
//
// Native hands (vr_native_hands.*) need one more leg: a room-space
// controller pose -> the space a viewmodel draw's OWN vertices are recovered
// in (see docs/COORDINATE-SPACES.md). Two steps, each a named primitive so
// neither can be silently skipped or reordered:
//
//   1. WORLD -> LOCAL of some reference basis (here, the head/camera this
//      pair was located with). Axis rows are basis vectors expressed in
//      world coordinates (the convention this whole file and vr_handpose.cpp
//      share: world = sum_k local[k] * row_k), so going the other way is a
//      dot with each row -- the standard inverse of an orthonormal matrix.
//   2. The MEASURED CoD-world -> GL-eye axis swap (docs/COORDINATE-SPACES.md
//      "CoD world -> GL eye"): eye_x=-world_y, eye_y=+world_z, eye_z=-world_x.
//      This is a fixed proper rotation (det +1), not a per-frame quantity.
//
// Both are applied to a VECTOR by direct substitution and to a full basis by
// applying the same vector map to each of its three rows independently --
// correct because a basis row is itself just a physical direction being
// re-expressed in a new coordinate labelling, not an operator being
// conjugated.

static inline void VRM_WorldToLocalVec(const float axis[9], const float worldVec[3],
                                       float outLocal[3])
{
    for (int k = 0; k < 3; ++k)
        outLocal[k] = axis[k*3 + 0] * worldVec[0]
                    + axis[k*3 + 1] * worldVec[1]
                    + axis[k*3 + 2] * worldVec[2];
}

static inline void VRM_LocalToWorldVec(const float axis[9], const float localVec[3],
                                       float outWorld[3])
{
    for (int c = 0; c < 3; ++c)
        outWorld[c] = axis[0*3 + c] * localVec[0]
                    + axis[1*3 + c] * localVec[1]
                    + axis[2*3 + c] * localVec[2];
}

// Re-express `axis` (rows = world-expressed basis vectors) in the local
// coordinates of `refAxis` (also rows = world-expressed basis vectors).
static inline void VRM_WorldToLocalAxis(const float refAxis[9], const float axis[9],
                                        float outLocal[9])
{
    for (int k = 0; k < 3; ++k)
        VRM_WorldToLocalVec(refAxis, &axis[k*3], &outLocal[k*3]);
}

// The measured, fixed CoD-world -> GL-eye axis swap. Verified proper
// (det = +1): it is a rotation, not a reflection, so it never flips a hand.
static inline void VRM_QuakeToGLEyeVec(const float q[3], float out[3])
{
    out[0] = -q[1];
    out[1] =  q[2];
    out[2] = -q[0];
}

static inline void VRM_QuakeToGLEyeAxis(const float q[9], float out[9])
{
    for (int k = 0; k < 3; ++k)
        VRM_QuakeToGLEyeVec(&q[k*3], &out[k*3]);
}

// The exact inverse of the swap above (it is a rotation, so its inverse is
// its transpose): GL eye -> CoD world axes. Used to reconstruct a rendered
// object back into the pair's common frame for the stereo verdict.
static inline void VRM_GLEyeToQuakeVec(const float e[3], float out[3])
{
    out[0] = -e[2];
    out[1] = -e[0];
    out[2] =  e[1];
}

static inline void VRM_GLEyeToQuakeAxis(const float e[9], float out[9])
{
    for (int k = 0; k < 3; ++k)
        VRM_GLEyeToQuakeVec(&e[k*3], &out[k*3]);
}

// Apply rotation A within frame B (row-basis convention). This is the same
// composition vr.cpp's ComposeAxis performs; kept here so controller math can
// be tested against the head math instead of drifting from it.
static inline void VRM_MulRows(const float a[9], const float b[9], float out[9])
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out[r*3 + c] = a[r*3 + 0] * b[0*3 + c]
                         + a[r*3 + 1] * b[1*3 + c]
                         + a[r*3 + 2] * b[2*3 + c];
}

static inline void VRM_YawRows(float yawRad, float rows[9])
{
    const float c = cosf(yawRad), s = sinf(yawRad);
    rows[0] = c;  rows[1] = s;  rows[2] = 0;
    rows[3] = -s; rows[4] = c;  rows[5] = 0;
    rows[6] = 0;  rows[7] = 0;  rows[8] = 1;
}

// Rotate every row of a basis about Quake +Z (used to cancel the neutral yaw
// out of a room pose, exactly as the head path does).
static inline void VRM_RotateRowsZ(float yawRad, const float in[9], float out[9])
{
    const float c = cosf(yawRad), s = sinf(yawRad);
    for (int r = 0; r < 3; ++r) {
        const float x = in[r*3 + 0], y = in[r*3 + 1];
        out[r*3 + 0] = x * c - y * s;
        out[r*3 + 1] = x * s + y * c;
        out[r*3 + 2] = in[r*3 + 2];
    }
}

static inline void VRM_RotateVecZ(float yawRad, const float in[3], float out[3])
{
    const float c = cosf(yawRad), s = sinf(yawRad);
    out[0] = in[0] * c - in[1] * s;
    out[1] = in[0] * s + in[1] * c;
    out[2] = in[2];
}

static inline float VRM_WrapDeg(float d)
{
    while (d >  180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

// Yaw/pitch of a row basis's forward vector, degrees (+yaw left, +pitch up).
static inline void VRM_AxisYawPitchDeg(const float axis[9], float* yawDeg, float* pitchDeg)
{
    const float r2d = 57.2957795f;
    float fz = axis[2];
    if (fz >  1.0f) fz =  1.0f;
    if (fz < -1.0f) fz = -1.0f;
    if (yawDeg)   *yawDeg   = atan2f(axis[1], axis[0]) * r2d;
    if (pitchDeg) *pitchDeg = asinf(fz) * r2d;
}

// Row basis from yaw/pitch/roll degrees (Quake convention: +yaw left,
// +pitch up, +roll clockwise about forward). Matches the head path's
// yaw*pitch*roll factorisation.
static inline void VRM_AnglesToRows(float yawDeg, float pitchDeg, float rollDeg, float rows[9])
{
    const float d2r = 0.01745329252f;
    const float cy = cosf(yawDeg * d2r),  sy = sinf(yawDeg * d2r);
    const float cp = cosf(pitchDeg * d2r), sp = sinf(pitchDeg * d2r);
    const float cr = cosf(rollDeg * d2r),  sr = sinf(rollDeg * d2r);
    float yaw[9]   = { cy, sy, 0,  -sy, cy, 0,  0, 0, 1 };
    float pitch[9] = { cp, 0, sp,  0, 1, 0,  -sp, 0, cp };
    float roll[9]  = { 1, 0, 0,  0, cr, sr,  0, -sr, cr };
    float pr[9];
    VRM_MulRows(roll, pitch, pr);
    VRM_MulRows(pr, yaw, rows);
}

// Exact inverse of VRM_AnglesToRows for |pitch| < 90: recover (yaw, pitch,
// roll) degrees from a row basis. Needed because CoD's entity struct carries
// BOTH an axis (+12) and Quake-order euler angles (+56): the engine may
// consume either, so a rewrite must keep the two representations consistent.
// Verified against a live-captured viewmodel entity: axis rows exactly equal
// VRM_AnglesToRows(yaw, -quakePitch, roll) for its angles (27.169, -146.096).
static inline void VRM_AxisToAnglesDeg(const float rows[9],
    float* yawDeg, float* pitchDeg, float* rollDeg)
{
    const float r2d = 57.2957795f;
    float fz = rows[2];
    if (fz >  1.0f) fz =  1.0f;
    if (fz < -1.0f) fz = -1.0f;
    if (yawDeg)   *yawDeg   = atan2f(rows[1], rows[0]) * r2d;
    if (pitchDeg) *pitchDeg = asinf(fz) * r2d;
    // From the factorisation: left.z = sin(roll)*cos(pitch),
    // up.z = cos(roll)*cos(pitch); cos(pitch) > 0 cancels in atan2.
    if (rollDeg)  *rollDeg  = atan2f(rows[5], rows[8]) * r2d;
}

// Build an orthonormal row basis from a forward vector, borrowing roll from a
// reference up vector (two-handed grip: forward comes from the hands, roll
// stays with the dominant controller).
static inline int VRM_BasisFromForwardUp(const float fwd[3], const float refUp[3], float rows[9])
{
    float f[3] = { fwd[0], fwd[1], fwd[2] };
    const float fl = sqrtf(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
    if (fl < 1e-6f)
        return 0;
    f[0] /= fl; f[1] /= fl; f[2] /= fl;
    // up = refUp minus its component along forward
    const float d = refUp[0]*f[0] + refUp[1]*f[1] + refUp[2]*f[2];
    float u[3] = { refUp[0] - d*f[0], refUp[1] - d*f[1], refUp[2] - d*f[2] };
    const float ul = sqrtf(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]);
    if (ul < 1e-6f)
        return 0;
    u[0] /= ul; u[1] /= ul; u[2] /= ul;
    // left = up x forward (Quake: forward x left = up  =>  left = up x forward)
    const float l[3] = { u[1]*f[2] - u[2]*f[1],
                         u[2]*f[0] - u[0]*f[2],
                         u[0]*f[1] - u[1]*f[0] };
    rows[0] = f[0]; rows[1] = f[1]; rows[2] = f[2];
    rows[3] = l[0]; rows[4] = l[1]; rows[5] = l[2];
    rows[6] = u[0]; rows[7] = u[1]; rows[8] = u[2];
    return 1;
}

// Coverage checks for the optics diagnostic: does a viewport/scissor rect
// cover the whole render target?
static inline int VRM_ViewportCovers(int x, int y, int w, int h, int fbW, int fbH)
{
    return x <= 0 && y <= 0 && x + w >= fbW && y + h >= fbH;
}

static inline int VRM_ScissorCovers(int enabled, int x, int y, int w, int h,
                                    int fbW, int fbH)
{
    if (!enabled)
        return 1;
    return VRM_ViewportCovers(x, y, w, h, fbW, fbH);
}
