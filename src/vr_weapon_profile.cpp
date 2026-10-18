// Per-weapon VR profile lookup, live identification, and the calibration
// report block. Pure logic -- no OpenXR, no GL, no Windows -- so the whole
// module runs in the offline suite.
#include "vr_weapon_profile.h"
#include "vr_weapon.h"
#include "vr_binds.h"

#include <string.h>
#include <math.h>
#include <stdio.h>

extern void Log(const char* fmt, ...);

#include "vr_weapon_profile_table.inc"

// ------------------------------------------------------------------- lookup

int VRWProfile_Count() { return (int)(sizeof(kVrWeaponProfiles) / sizeof(kVrWeaponProfiles[0])); }

const VrWeaponProfile* VRWProfile_ByIndex(int i)
{
    if (i < 0 || i >= VRWProfile_Count())
        return &kVrWeaponProfiles[0];
    return &kVrWeaponProfiles[i];
}

const VrWeaponProfile* VRWProfile_ById(int id)
{
    for (int i = 0; i < VRWProfile_Count(); ++i)
        if (kVrWeaponProfiles[i].id == id)
            return &kVrWeaponProfiles[i];
    return &kVrWeaponProfiles[0];
}

const VrWeaponProfile* VRWProfile_ByName(const char* name)
{
    if (!name)
        return &kVrWeaponProfiles[0];
    for (int i = 0; i < VRWProfile_Count(); ++i)
        if (kVrWeaponProfiles[i].name && strcmp(kVrWeaponProfiles[i].name, name) == 0)
            return &kVrWeaponProfiles[i];
    return &kVrWeaponProfiles[0];
}

// ------------------------------------------------------------- confidence
//
// The gate that keeps an asset-derived prior from being spent as if it were a
// hardware measurement. There is exactly one way to lower it -- the config
// key -- and no code path that promotes a value on its own.

static int g_experimentalCalibration;

void VRWProfile_SetExperimentalCalibration(int on) { g_experimentalCalibration = on ? 1 : 0; }
int  VRWProfile_ExperimentalCalibration() { return g_experimentalCalibration; }

const char* VRWProfile_ConfName(int conf)
{
    switch (conf) {
    case VRWP_CONF_PROVEN:  return "PROVEN";
    case VRWP_CONF_STRONG:  return "STRONG";
    case VRWP_CONF_WEAK:    return "WEAK";
    case VRWP_CONF_NA:      return "N/A";
    default:                return "UNKNOWN";
    }
}

int VRWProfile_ConfidenceUsable(int conf)
{
    // N/A is never usable however the flag is set: "this weapon has no barrel"
    // does not become a barrel because someone turned on experiments.
    if (conf == VRWP_CONF_NA || conf == VRWP_CONF_UNKNOWN)
        return 0;
    if (conf >= VRWP_CONF_STRONG)
        return 1;
    return g_experimentalCalibration ? 1 : 0;
}

static const float* ProfileField3(const VrWeaponProfile* p, int field)
{
    switch (field) {
    case VRWP_F_MUZZLE_ORIGIN:      return p->muzzleOriginM;
    case VRWP_F_BARREL_FORWARD:     return p->barrelForward;
    case VRWP_F_REAR_GRIP_OFFSET:   return p->rearGripOffsetM;
    case VRWP_F_REAR_GRIP_ROTATION: return p->rearGripRotationDeg;
    case VRWP_F_SIGHT_OFFSET:       return p->sightOffsetM;
    case VRWP_F_SIGHT_FORWARD:      return p->sightForward;
    default:                        return 0;
    }
}

int VRWProfile_Value3(const VrWeaponProfile* p, int field, float out[3])
{
    if (!p || !out || field < 0 || field >= VRWP_F_COUNT)
        return 0;
    if (!VRWProfile_ConfidenceUsable(p->conf[field]))
        return 0;
    const float* v = ProfileField3(p, field);
    if (!v || VRWP_IsAuto3(v))
        return 0;
    out[0] = v[0]; out[1] = v[1]; out[2] = v[2];
    return 1;
}

int VRWProfile_Value1(const VrWeaponProfile* p, int field, float* out)
{
    if (!p || !out || field < 0 || field >= VRWP_F_COUNT)
        return 0;
    if (!VRWProfile_ConfidenceUsable(p->conf[field]))
        return 0;
    float v;
    if (field == VRWP_F_SUPPORT_ALONG)       v = p->supportAlongAxisM;
    else if (field == VRWP_F_SUPPORT_RADIUS) v = p->supportRadiusM;
    else                                     return 0;
    if (VRWP_IsAuto(v))
        return 0;
    *out = v;
    return 1;
}

// ----------------------------------------------------------- identification
//
// Thresholds. The tightest distinct pair in the generated table is 0.023 in
// apart (stalingrad_ammo vs sten), which no live measurement will ever
// separate -- so the margin test exists precisely to say "ambiguous" instead
// of picking one at random. The match radius is generous (1.5 in) because a
// wrong-but-flagged low-confidence guess is useful data, while a silent miss
// is not; ENFORCE mode applies its own much stricter gate.

static const float kMatchRadiusU   = 1.5f;   // beyond this, no match at all
static const float kFullMarginU    = 0.75f;  // margin that earns full credit
static const int   kEnforceMinConf = 80;     // routing needs near-certainty

static float Dist3(const float a[3], const float b[3])
{
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

int VRWProfile_Identify(const float hipU[3], int haveAds, const float adsU[3],
                        VrWeaponMatch* out)
{
    VrWeaponMatch m;
    memset(&m, 0, sizeof(m));
    m.bestDistU = m.runnerUpDistU = 1.0e9f;
    m.usedAdsDelta = haveAds ? 1 : 0;

    if (!hipU) {
        if (out) *out = m;
        return 0;
    }

    for (int i = 0; i < VRWProfile_Count(); ++i) {
        const VrWeaponProfile& p = kVrWeaponProfiles[i];
        if (!p.sightKnown || VRWP_IsAuto3(p.hipOffsetU))
            continue;
        float d = Dist3(hipU, p.hipOffsetU);
        if (haveAds && adsU && !VRWP_IsAuto3(p.sightOffsetU)) {
            // Combine hip and sighted poses in quadrature: two independent
            // 3-vectors agreeing is a far stronger claim than one.
            const float ds = Dist3(adsU, p.sightOffsetU);
            d = sqrtf(d * d + ds * ds);
        }
        if (d < m.bestDistU) {
            m.runnerUpDistU = m.bestDistU;
            m.runnerUpId = m.id;
            m.bestDistU = d;
            m.id = p.id;
        } else if (d < m.runnerUpDistU) {
            m.runnerUpDistU = d;
            m.runnerUpId = p.id;
        }
    }

    if (m.bestDistU > kMatchRadiusU) {
        m.id = 0;
        m.confidence = 0;
    } else {
        m.marginU = (m.runnerUpDistU < 1.0e8f) ? m.runnerUpDistU - m.bestDistU : kFullMarginU;
        const float distScore = Clamp01(1.0f - m.bestDistU / kMatchRadiusU);
        const float marginScore = Clamp01(m.marginU / kFullMarginU);
        m.confidence = (int)(100.0f * distScore * (0.35f + 0.65f * marginScore));
        if (m.confidence < 0) m.confidence = 0;
        if (m.confidence > 100) m.confidence = 100;
    }
    if (out) *out = m;
    return m.id;
}

// ------------------------------------------------------------- live tracking

// A measurement window: the viewmodel offset wobbles with idle sway and bob,
// so a single frame proves nothing. Identification waits for the offset to
// hold within a tight spread for a real interval -- the same "held steadily
// for several seconds" gate the calibration report needs.
enum { WIN_MIN_SAMPLES = 24 };
static const double kWinMinSeconds  = 1.0;
static const float  kWinMaxSpreadU  = 0.60f;   // idle sway is well inside this

static int    g_mode = VRWP_MODE_OBSERVE;

struct PoseWindow {
    int    n;
    double since;
    float  sum[3];
    float  mean[3];
    float  minv[3], maxv[3];
    int    stable;
};

static PoseWindow g_hipWin, g_adsWin;
static VrWeaponMatch g_match;
static int    g_activeId;
static int    g_identifications;
static int    g_resets;
static int    g_calibBlocks;
static int    g_calibLoggedId = -1;

// Support-geometry accumulators: what the two-hand attach actually looked
// like, so the AUTO fields get a measured RECOMMENDATION (never an automatic
// value) after a hardware run.
static int    g_supportSamples;
static float  g_supportAlongSum, g_supportPerpSum;
static float  g_supportAlongMin, g_supportAlongMax;
static int    g_adsSamples;
static float  g_adsEyeDistSum;

static void WindowReset(PoseWindow* w)
{
    memset(w, 0, sizeof(*w));
    w->since = -1.0;
}

static void WindowAdd(PoseWindow* w, const float v[3], double now)
{
    if (w->n == 0) {
        w->since = now;
        for (int i = 0; i < 3; ++i) {
            w->minv[i] = w->maxv[i] = v[i];
            w->sum[i] = 0.0f;
        }
    }
    for (int i = 0; i < 3; ++i) {
        w->sum[i] += v[i];
        if (v[i] < w->minv[i]) w->minv[i] = v[i];
        if (v[i] > w->maxv[i]) w->maxv[i] = v[i];
    }
    ++w->n;
    for (int i = 0; i < 3; ++i)
        w->mean[i] = w->sum[i] / (float)w->n;

    float spread = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float s = w->maxv[i] - w->minv[i];
        if (s > spread) spread = s;
    }
    if (spread > kWinMaxSpreadU) {
        // The pose moved: this is a different hip pose (or a switch mid-flight).
        // Start a fresh window at the current sample rather than averaging
        // across the discontinuity.
        const float keep[3] = { v[0], v[1], v[2] };
        WindowReset(w);
        w->since = now;
        w->n = 1;
        for (int i = 0; i < 3; ++i) {
            w->sum[i] = keep[i];
            w->mean[i] = keep[i];
            w->minv[i] = w->maxv[i] = keep[i];
        }
    }
    w->stable = (w->n >= WIN_MIN_SAMPLES && w->since >= 0.0
                 && (now - w->since) >= kWinMinSeconds) ? 1 : 0;
}

static float WindowSpread(const PoseWindow* w)
{
    float spread = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float s = w->maxv[i] - w->minv[i];
        if (s > spread) spread = s;
    }
    return w->n ? spread : 0.0f;
}

void VRWProfile_SetMode(int mode)
{
    if (mode < VRWP_MODE_OFF)     mode = VRWP_MODE_OFF;
    if (mode > VRWP_MODE_ENFORCE) mode = VRWP_MODE_ENFORCE;
    g_mode = mode;
}

int VRWProfile_Mode() { return g_mode; }

void VRWProfile_NoteWeaponMayHaveChanged(const char* reason)
{
    const int had = g_activeId;
    WindowReset(&g_hipWin);
    WindowReset(&g_adsWin);
    memset(&g_match, 0, sizeof(g_match));
    g_activeId = 0;
    g_supportSamples = 0;
    g_supportAlongSum = g_supportPerpSum = 0.0f;
    g_supportAlongMin = g_supportAlongMax = 0.0f;
    g_adsSamples = 0;
    g_adsEyeDistSum = 0.0f;
    g_calibLoggedId = -1;
    ++g_resets;
    if (had && reason) {
        static int logged = 0;
        if (logged < 8) {
            ++logged;
            Log("VR profile: identification dropped (%s)", reason);
        }
    }
}

void VRWProfile_Observe(int bakedValid, const float bakedU[3], int adsActive,
                        double now)
{
    if (g_mode == VRWP_MODE_OFF || !bakedValid || !bakedU)
        return;

    PoseWindow* w = adsActive ? &g_adsWin : &g_hipWin;
    WindowAdd(w, bakedU, now);

    // Support-hand geometry, sampled only while actually attached: this is the
    // measurement that would fill supportAlongAxis / supportRadius.
    VRWeaponDebug wd;
    VRWeapon_GetDebug(&wd);
    if (wd.twoHand && wd.twoHandHandDistM > 0.0f) {
        if (g_supportSamples == 0) {
            g_supportAlongMin = g_supportAlongMax = wd.twoHandAlongM;
        } else {
            if (wd.twoHandAlongM < g_supportAlongMin) g_supportAlongMin = wd.twoHandAlongM;
            if (wd.twoHandAlongM > g_supportAlongMax) g_supportAlongMax = wd.twoHandAlongM;
        }
        g_supportAlongSum += wd.twoHandAlongM;
        g_supportPerpSum  += wd.twoHandPerpM;
        ++g_supportSamples;
    }
    if (adsActive && wd.adsEyeDistM > 0.0f) {
        g_adsEyeDistSum += wd.adsEyeDistM;
        ++g_adsSamples;
    }

    if (!g_hipWin.stable)
        return;

    VrWeaponMatch m;
    VRWProfile_Identify(g_hipWin.mean, g_adsWin.stable ? 1 : 0,
                        g_adsWin.stable ? g_adsWin.mean : 0, &m);
    const int changed = m.id != g_match.id;
    g_match = m;
    if (changed) {
        g_activeId = m.id;
        ++g_identifications;
        const VrWeaponProfile* p = VRWProfile_ById(m.id);
        Log("VR profile: identified '%s' (id %d, conf %d%%, dist %.2f u, "
            "margin %.2f u%s)",
            p->name, m.id, m.confidence, m.bestDistU, m.marginU,
            m.usedAdsDelta ? ", hip+sight" : ", hip only");
    } else {
        g_activeId = m.id;
    }
}

const VrWeaponProfile* VRWProfile_Active()
{
    if (g_mode != VRWP_MODE_ENFORCE)
        return &kVrWeaponProfiles[0];
    if (!g_activeId || g_match.confidence < kEnforceMinConf)
        return &kVrWeaponProfiles[0];
    return VRWProfile_ById(g_activeId);
}

void VRWProfile_GetMatch(VrWeaponMatch* out)
{
    if (out) *out = g_match;
}

// ------------------------------------------------------------------ routing

int VRWProfile_TwoHandAllowed()
{
    const VrWeaponProfile* p = VRWProfile_Active();
    return p->twoHandAllowed != VRW2H_NO;
}

int VRWProfile_AdsAccuracyAllowed()
{
    const VrWeaponProfile* p = VRWProfile_Active();
    return p->adsAccuracyAllowed ? 1 : 0;
}

int VRWProfile_EnginePosed()
{
    return (VRWProfile_Active()->specialHandling & VRWSH_ENGINE_POSED) ? 1 : 0;
}

int VRWProfile_Thrown()
{
    return (VRWProfile_Active()->specialHandling & VRWSH_THROWN) ? 1 : 0;
}

// --------------------------------------------------------------- diagnostics

void VRWProfile_GetDebug(VRWProfileDebug* out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->mode = g_mode;
    const VrWeaponProfile* p = VRWProfile_Active();
    out->activeId = p->id;
    strncpy(out->activeName, p->name ? p->name : "?", sizeof(out->activeName) - 1);
    out->activeClass = p->vrClass;
    out->twoHandAllowed = VRWProfile_TwoHandAllowed();
    out->adsAllowed = VRWProfile_AdsAccuracyAllowed();
    out->samples = g_hipWin.n;
    out->stable = g_hipWin.stable;
    memcpy(out->meanHipU, g_hipWin.mean, sizeof(out->meanHipU));
    out->spreadHipU = WindowSpread(&g_hipWin);
    out->haveAdsPose = g_adsWin.stable;
    memcpy(out->meanAdsU, g_adsWin.mean, sizeof(out->meanAdsU));
    out->match = g_match;
    out->identifications = g_identifications;
    out->resets = g_resets;
}

enum { CALIB_MAX_BLOCKS = 10 };

int VRWProfile_MaybeLogCalibration(double now)
{
    if (g_mode == VRWP_MODE_OFF)
        return 0;
    if (g_calibBlocks >= CALIB_MAX_BLOCKS)
        return 0;
    if (!g_hipWin.stable)
        return 0;
    if (g_calibLoggedId == g_match.id)
        return 0;               // already reported this identification

    g_calibLoggedId = g_match.id;
    ++g_calibBlocks;

    VRWeaponDebug wd;
    VRWeapon_GetDebug(&wd);
    const VrWeaponProfile* p = VRWProfile_ById(g_match.id);

    // The point of this block is a SIDE-BY-SIDE: what the game's own assets
    // predict, what the headset actually observed, and the difference. An
    // error line only appears where the two are genuinely the same physical
    // quantity. Subtracting a weapon-frame vector from a room-space one and
    // printing the result would look like a measurement and be arithmetic on
    // unlike things, which is worse than an honest blank.
    const char* cn[VRWP_F_COUNT];
    for (int f = 0; f < VRWP_F_COUNT; ++f)
        cn[f] = VRWProfile_ConfName(p->conf[f]);

    Log("======= VR WEAPON CALIBRATION =======");
    Log("  weapon          : %s (id %d)  match confidence %d%%",
        p->name, g_match.id, g_match.confidence);
    Log("  profile mode    : %d (%s), experimental calibration %s",
        g_mode, g_mode == VRWP_MODE_ENFORCE ? "ENFORCE" : "observe",
        VRWProfile_ExperimentalCalibration() ? "ON" : "off");
    Log("  weapon frame    : %s", p->frameBone ? p->frameBone
                                                : "(none -- no asset geometry)");
    Log("  match distance  : best %.3f u, runner-up %.3f u (id %d), margin %.3f u",
        g_match.bestDistU, g_match.runnerUpDistU, g_match.runnerUpId, g_match.marginU);

    Log("  --- ASSET PREDICTION (from the shipped rig; NOT hardware-verified) ---");
    if (!VRWP_IsAuto3(p->rearGripOffsetM))
        Log("    rear grip     : %+.3f %+.3f %+.3f m   [%s]",
            p->rearGripOffsetM[0], p->rearGripOffsetM[1], p->rearGripOffsetM[2],
            cn[VRWP_F_REAR_GRIP_OFFSET]);
    else
        Log("    rear grip     : AUTO   [%s]", cn[VRWP_F_REAR_GRIP_OFFSET]);
    if (!VRWP_IsAuto3(p->supportOffsetM))
        Log("    support grip  : %+.3f %+.3f %+.3f m (along %.3f, radius %.3f) [%s]",
            p->supportOffsetM[0], p->supportOffsetM[1], p->supportOffsetM[2],
            p->supportAlongAxisM, p->supportRadiusM, cn[VRWP_F_SUPPORT_ALONG]);
    else
        Log("    support grip  : AUTO   [%s]", cn[VRWP_F_SUPPORT_ALONG]);
    if (!VRWP_IsAuto3(p->muzzleOriginM))
        Log("    muzzle        : %+.3f %+.3f %+.3f m   [%s]",
            p->muzzleOriginM[0], p->muzzleOriginM[1], p->muzzleOriginM[2],
            cn[VRWP_F_MUZZLE_ORIGIN]);
    else
        Log("    muzzle        : AUTO   [%s]", cn[VRWP_F_MUZZLE_ORIGIN]);
    if (!VRWP_IsAuto3(p->barrelForward))
        Log("    barrel forward: %+.4f %+.4f %+.4f   [%s]",
            p->barrelForward[0], p->barrelForward[1], p->barrelForward[2],
            cn[VRWP_F_BARREL_FORWARD]);
    else
        Log("    barrel forward: AUTO   [%s]", cn[VRWP_F_BARREL_FORWARD]);
    if (!VRWP_IsAuto3(p->sightForward))
        Log("    sight line    : fwd %+.4f %+.4f %+.4f, eye %+.3f %+.3f %+.3f m [%s]",
            p->sightForward[0], p->sightForward[1], p->sightForward[2],
            p->sightOffsetM[0], p->sightOffsetM[1], p->sightOffsetM[2],
            cn[VRWP_F_SIGHT_FORWARD]);
    else
        Log("    sight line    : AUTO   [%s]", cn[VRWP_F_SIGHT_FORWARD]);

    Log("  --- LIVE OBSERVATION (this session) ---");
    Log("    dominant grip : %+.3f %+.3f %+.3f m (room)",
        wd.rearAnchorRoomM[0], wd.rearAnchorRoomM[1], wd.rearAnchorRoomM[2]);
    Log("    support grip  : %+.3f %+.3f %+.3f m (room)",
        wd.frontAnchorRoomM[0], wd.frontAnchorRoomM[1], wd.frontAnchorRoomM[2]);
    Log("    rendered parent: %.2f %.2f %.2f  (vieworg %.2f %.2f %.2f)",
        wd.weaponWorldOrigin[0], wd.weaponWorldOrigin[1], wd.weaponWorldOrigin[2],
        wd.gameVieworg[0], wd.gameVieworg[1], wd.gameVieworg[2]);
    Log("    baked offset  : %.3f %.3f %.3f u (valid %d)",
        wd.bakedOffsetU[0], wd.bakedOffsetU[1], wd.bakedOffsetU[2],
        wd.bakedOffsetValid);
    Log("    signature     : hip %.3f %.3f %.3f u (spread %.3f, %d samples)",
        g_hipWin.mean[0], g_hipWin.mean[1], g_hipWin.mean[2],
        WindowSpread(&g_hipWin), g_hipWin.n);
    if (g_adsWin.stable)
        Log("                    sighted %.3f %.3f %.3f u (%d samples)",
            g_adsWin.mean[0], g_adsWin.mean[1], g_adsWin.mean[2], g_adsWin.n);
    else
        Log("                    sighted UNKNOWN (no stable ADS window)");
    Log("    table hip/ADS : %s", p->sightKnown ? "known"
                                                : "UNKNOWN (no ADS animation)");
    if (p->sightKnown)
        Log("                    hip %.3f %.3f %.3f  sight %.3f %.3f %.3f u",
            p->hipOffsetU[0], p->hipOffsetU[1], p->hipOffsetU[2],
            p->sightOffsetU[0], p->sightOffsetU[1], p->sightOffsetU[2]);
    Log("    controller fwd: %.4f %.4f %.4f (world)",
        wd.weaponWorldFwd[0], wd.weaponWorldFwd[1], wd.weaponWorldFwd[2]);
    Log("    gameplay fire : yaw %.2f pitch %.2f deg (target yaw %.2f pitch %.2f)",
        wd.gameYawDeg, wd.gamePitchDeg, wd.targetYawDeg, wd.targetPitchDeg);

    Log("  --- ERROR (only where asset and live measure the SAME thing) ---");
    const int haveSupportAsset = !VRWP_IsAuto(p->supportAlongAxisM)
                                 && !VRWP_IsAuto(p->supportRadiusM);
    if (g_supportSamples > 0 && haveSupportAsset) {
        const float along = g_supportAlongSum / (float)g_supportSamples;
        const float perp  = g_supportPerpSum  / (float)g_supportSamples;
        Log("    support along : asset %.3f m, live %.3f m -> %+.1f cm",
            p->supportAlongAxisM, along, (along - p->supportAlongAxisM) * 100.0f);
        Log("    support radius: asset %.3f m, live %.3f m -> %+.1f cm",
            p->supportRadiusM, perp, (perp - p->supportRadiusM) * 100.0f);
    } else if (!haveSupportAsset) {
        Log("    support point : asset AUTO -- nothing to compare");
    } else {
        Log("    support point : live UNKNOWN (support hand never attached)");
    }
    if (g_adsSamples > 0 && !VRWP_IsAuto3(p->sightOffsetM)) {
        const float liveEye = g_adsEyeDistSum / (float)g_adsSamples;
        const float assetEye = sqrtf(p->sightOffsetM[0] * p->sightOffsetM[0]
                                     + p->sightOffsetM[1] * p->sightOffsetM[1]
                                     + p->sightOffsetM[2] * p->sightOffsetM[2]);
        Log("    sight align   : asset %.3f m, live %.3f m -> %+.1f cm",
            assetEye, liveEye, (liveEye - assetEye) * 100.0f);
    } else {
        Log("    sight align   : %s", g_adsSamples > 0
            ? "asset AUTO" : "live UNKNOWN (no ADS samples)");
    }
    if (!VRWP_IsAuto3(p->barrelForward)) {
        // How far the model's own bore departs from "forward is the frame's
        // +X". The solver assumes +X; this says what that assumption costs,
        // which is the asset half of the barrel/fire angular error.
        float bx = p->barrelForward[0];
        if (bx > 1.0f) bx = 1.0f;
        if (bx < -1.0f) bx = -1.0f;
        Log("    barrel vs +X  : %.2f deg -- the solver's standing assumption",
            (float)(acos((double)bx) * 57.29577951308232));
    } else {
        Log("    barrel vs +X  : asset AUTO");
    }
    Log("    rear grip     : NOT COMPARABLE -- the asset value is in the weapon");
    Log("                    frame and the live one is in room space, and");
    Log("                    nothing measured this session relates the two.");

    Log("  --- RECOMMEND (for review; NOTHING here is applied) ---");
    if (g_supportSamples > 0) {
        const float along = g_supportAlongSum / (float)g_supportSamples;
        const float perp  = g_supportPerpSum  / (float)g_supportSamples;
        Log("    supportAlongAxis %.3f  supportRadius %.3f  (live, %d samples, "
            "range %.3f..%.3f)", along, perp > 0.05f ? perp : 0.05f,
            g_supportSamples, g_supportAlongMin, g_supportAlongMax);
    } else {
        Log("    supportAlongAxis AUTO  supportRadius AUTO  (never attached)");
    }
    Log("    Asset values are priors from the rig, not measurements. Where the");
    Log("    error is small the prior held; where it is large or blank, that");
    Log("    field still needs the headset.");
    Log("=====================================");
    return 1;
}

// ------------------------------------------------- vr_weapon_debug (phase 7)
//
// PORTED AS A STUB, DELIBERATELY.
//
// The offline-hardening tree's version of this block formatted a live
// telemetry report by reading VRWeaponDebug and VRBindsDebug. Those structs
// have since diverged: that tree's carry fields this one does not
// (steerLagFrames, cpdHip, VRB_ADS_OWNER_*), because they belong to the
// controller-steering design the camera-stable baseline RETIRED.
//
// The instruction for this port was to bring across the DATA and leave the
// stale code behind, and this block is stale code. Reviving it would mean
// reviving the steering fields it reads, which is the one thing the
// camera-stable baseline exists to prevent. The profile TABLE above -- which
// is what the port was for -- is complete and untouched.
//
// The seam is kept so callers still link, and so a future telemetry block can
// be written against THIS tree's structs rather than resurrected against
// another tree's.

static int             g_dbgEnabled;
static float           g_dbgPeriod = 2.0f;
static int             g_dbgBlocks;
static VRWDebugExtraFn g_dbgExtra;

void VRWDebug_SetExtraLogger(VRWDebugExtraFn fn) { g_dbgExtra = fn; }
int  VRWDebug_Enabled()                          { return g_dbgEnabled; }
void VRWDebug_SetEnabled(int on)                 { g_dbgEnabled = on ? 1 : 0; }
void VRWDebug_SetPeriod(float seconds)
{
    if (seconds > 0.05f && seconds < 60.0f)
        g_dbgPeriod = seconds;
}
int VRWDebug_BlocksEmitted() { return g_dbgBlocks; }

int VRWDebug_MaybeLog(double now)
{
    (void)now;
    return 0;      // nothing is emitted until a block is written for THIS tree
}

void VRWProfile_TestReset()
{
    g_mode = VRWP_MODE_OBSERVE;
    g_dbgEnabled = 0;
    g_dbgPeriod = 2.0f;
    g_dbgBlocks = 0;
    WindowReset(&g_hipWin);
    WindowReset(&g_adsWin);
    memset(&g_match, 0, sizeof(g_match));
    g_activeId = 0;
    g_identifications = 0;
    g_resets = 0;
    g_calibBlocks = 0;
    g_calibLoggedId = -1;
    g_supportSamples = 0;
    g_supportAlongSum = g_supportPerpSum = 0.0f;
    g_supportAlongMin = g_supportAlongMax = 0.0f;
    g_adsSamples = 0;
    g_adsEyeDistSum = 0.0f;
}
