#include "vr_cfg_contract.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#if defined(_MSC_VER)
  #define CODVR_STRICMP _stricmp
#else
  #include <strings.h>
  #define CODVR_STRICMP strcasecmp
#endif

void ProbeCfg_Production(CodvrProbeCfg* c)
{
    if (!c)
        return;
    memset(c, 0, sizeof(*c));
    c->armed      = 0;
    c->vrDisabled = 0;                       // VR ON
    c->sweep      = 0;                       // sweep OFF
    c->autoQuit   = 0;                       // autoquit OFF
    c->probeClass = CODVR_PROBE_CLASS_NONE;  // no surface transformed
    c->delta[0] = c->delta[1] = c->delta[2] = c->delta[3] = 0.0f;
    c->capFrames   = 0;
    c->capSkip     = 240;    // only consulted once a capture is armed
    c->quitAfter   = 0;
    c->sweepFrames = 20;     // only consulted once the sweep is armed
    c->flatFocus   = 0;
    c->handSolveProbe = 0;
    strncpy(c->tag, "flat", sizeof(c->tag) - 1);
    c->tag[sizeof(c->tag) - 1] = 0;
}

static void Say(char* reason, size_t cap, const char* fmt, ...)
{
    if (!reason || cap == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason, cap, fmt, ap);
    va_end(ap);
    reason[cap - 1] = 0;
}

int ProbeCfg_Apply(CodvrProbeCfg* c, const char* key, const char* text,
                   float val, char* reason, size_t reasonCap)
{
    if (!c || !key)
        return 0;
    if (reason && reasonCap)
        reason[0] = 0;

    // The arming key itself. Deliberately the only probe key that works
    // unarmed -- it is what arming means.
    if (!CODVR_STRICMP(key, "vr_probe_arm")) {
        c->armed = (val != 0.0f);
        Say(reason, reasonCap, "config: probe cvars %s",
            c->armed ? "ARMED (this is NOT a production configuration)"
                     : "disarmed");
        return 1;
    }

    // Is this one of ours at all?
    static const char* const kProbeKeys[] = {
        "vr_disable", "vr_glcap_frames", "vr_glcap_skip", "vr_glcap_autoquit",
        "vr_glcap_quit_after", "vr_glcap_tag", "vr_flat_focus",
        "vr_glprobe_sweep", "vr_glprobe_sweep_frames", "vr_glprobe_class",
        "vr_glprobe_dx", "vr_glprobe_dy", "vr_glprobe_dz", "vr_glprobe_yaw",
        "vr_glprobe_handsolve",
    };
    int mine = 0;
    for (size_t i = 0; i < sizeof(kProbeKeys) / sizeof(kProbeKeys[0]); ++i) {
        if (!CODVR_STRICMP(key, kProbeKeys[i])) { mine = 1; break; }
    }
    if (!mine)
        return 0;

    // Consumed but dropped. This is the whole point of the file: a stale
    // override block left in codvr.cfg reaches here, is named in the log, and
    // changes nothing.
    if (!c->armed) {
        Say(reason, reasonCap,
            "config: %s IGNORED -- probe cvars need 'vr_probe_arm 1' first "
            "(production contract: VR on, sweep off, autoquit off, no probe)",
            key);
        return 1;
    }

    if (!CODVR_STRICMP(key, "vr_disable")) {
        c->vrDisabled = (val != 0.0f);
        Say(reason, reasonCap, "config: VR %s",
            c->vrDisabled ? "DISABLED (flat run)" : "enabled");
    } else if (!CODVR_STRICMP(key, "vr_glcap_frames")) {
        c->capFrames = (int)val;
        Say(reason, reasonCap, "config: glsurf capture frames = %d", c->capFrames);
    } else if (!CODVR_STRICMP(key, "vr_glcap_skip")) {
        c->capSkip = (int)val;
        Say(reason, reasonCap, "config: glsurf capture skip = %d scene frames", c->capSkip);
    } else if (!CODVR_STRICMP(key, "vr_glcap_autoquit")) {
        c->autoQuit = (val != 0.0f);
        Say(reason, reasonCap, "config: glsurf capture auto-quit = %d", c->autoQuit);
    } else if (!CODVR_STRICMP(key, "vr_glcap_quit_after")) {
        c->quitAfter = (int)val;
        Say(reason, reasonCap, "config: glsurf quit after %d scene frames", c->quitAfter);
    } else if (!CODVR_STRICMP(key, "vr_glcap_tag")) {
        if (text) {
            strncpy(c->tag, text, sizeof(c->tag) - 1);
            c->tag[sizeof(c->tag) - 1] = 0;
        }
        Say(reason, reasonCap, "config: glsurf capture tag = %s", c->tag);
    } else if (!CODVR_STRICMP(key, "vr_flat_focus")) {
        c->flatFocus = (val != 0.0f);
        Say(reason, reasonCap, "config: flat focus assist = %s",
            c->flatFocus ? "ON" : "off");
    } else if (!CODVR_STRICMP(key, "vr_glprobe_sweep")) {
        c->sweep = (val != 0.0f);
        Say(reason, reasonCap, "config: glsurf probe sweep = %s",
            c->sweep ? "ON" : "off");
    } else if (!CODVR_STRICMP(key, "vr_glprobe_sweep_frames")) {
        c->sweepFrames = (int)val;
        Say(reason, reasonCap, "config: glsurf sweep stage length = %d scene frames",
            c->sweepFrames);
    } else if (!CODVR_STRICMP(key, "vr_glprobe_class")) {
        c->probeClass = (int)val;
        Say(reason, reasonCap, "config: glsurf probe class = %d", c->probeClass);
    } else if (!CODVR_STRICMP(key, "vr_glprobe_dx")) {
        c->delta[0] = val;
        Say(reason, reasonCap, "config: glsurf probe dx = %.3f", val);
    } else if (!CODVR_STRICMP(key, "vr_glprobe_dy")) {
        c->delta[1] = val;
        Say(reason, reasonCap, "config: glsurf probe dy = %.3f", val);
    } else if (!CODVR_STRICMP(key, "vr_glprobe_dz")) {
        c->delta[2] = val;
        Say(reason, reasonCap, "config: glsurf probe dz = %.3f", val);
    } else if (!CODVR_STRICMP(key, "vr_glprobe_yaw")) {
        c->delta[3] = val;
        Say(reason, reasonCap, "config: glsurf probe yaw = %.2f", val);
    } else if (!CODVR_STRICMP(key, "vr_glprobe_handsolve")) {
        c->handSolveProbe = (val != 0.0f);
        Say(reason, reasonCap, "config: glsurf hand-solve probe = %s",
            c->handSolveProbe ? "ON (zero-delta targets, solve every hand draw)"
                              : "off");
    }
    return 1;
}

void ProbeCfg_Enforce(CodvrProbeCfg* c)
{
    if (!c)
        return;
    if (c->armed)
        return;   // an armed config is an attended run; it keeps what it asked for

    // Belt and braces. Reaching here with anything but the production values
    // would mean a probe key slipped past the arming check -- so rather than
    // trusting that it cannot happen, put the contract back.
    c->vrDisabled = 0;
    c->sweep      = 0;
    c->autoQuit   = 0;
    c->probeClass = CODVR_PROBE_CLASS_NONE;
    c->delta[0] = c->delta[1] = c->delta[2] = c->delta[3] = 0.0f;
    c->capFrames = 0;
    c->quitAfter = 0;
    c->flatFocus = 0;
    c->handSolveProbe = 0;
}

int ProbeCfg_IsProduction(const CodvrProbeCfg* c)
{
    if (!c)
        return 0;
    return c->vrDisabled == 0
        && c->sweep      == 0
        && c->autoQuit   == 0
        && c->probeClass == CODVR_PROBE_CLASS_NONE
        && c->handSolveProbe == 0;
}
