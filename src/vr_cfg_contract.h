// ---------------------------------------------------------------------------
// PRODUCTION PROBE CONTRACT
// ---------------------------------------------------------------------------
//
// Why this file exists.
//
// On 2026-08-25 a build reached the game folder with the attended flat
// surface-probe still armed. codvr.cfg carried, appended at the end:
//
//     vr_disable 1
//     vr_glprobe_sweep 1
//     vr_glcap_autoquit 1
//
// so the game came up FLAT and closed itself a few seconds into gameplay.
// From the player's chair that is indistinguishable from "VR is broken and
// the mod crashes" -- but nothing was broken. The probe did exactly what it
// was told, in a build nobody meant to keep.
//
// The lesson is not "remember to clean the cfg". Config hygiene is a habit,
// and habits fail. The lesson is that the DANGEROUS configuration must not be
// reachable by accident: a stale line in a text file must not be able to turn
// a shipping build into a self-closing flat capture rig.
//
// So probe settings are now ARMED, not merely set. Every probe key is inert
// unless the config has already said, explicitly:
//
//     vr_probe_arm 1
//
// A leftover override block without that line is logged and ignored. And
// because key ordering is another thing that fails quietly, the contract is
// re-asserted after the whole file is parsed -- an unarmed config ends at the
// production values no matter what any individual key did on the way through.
//
// The production contract, in full:
//
//     vr disabled for probe = false     (an XR session may be created)
//     glsurf sweep          = false     (no staged transform sweep)
//     glsurf autoquit       = false     (the process never closes itself)
//     probe class           = none      (no surface is transformed)
//
// This is what you get from a missing codvr.cfg, an unreadable one, a
// malformed one, and an unarmed one. There is no path from "no configuration"
// to "flat self-closing probe".

#pragma once

#include <stddef.h>

// Kept numerically in step with GLSurfClass::GLSC_NONE. Declared here as a
// plain int so the contract can be reasoned about, and tested, without
// dragging in the renderer.
#define CODVR_PROBE_CLASS_NONE 0

struct CodvrProbeCfg {
    // Arming. Nothing below can leave its production value while this is 0.
    int   armed;

    // The four contract values. These are the ones that decide whether the
    // build a player launches is a VR build or a capture rig.
    int   vrDisabled;      // block XR session creation entirely
    int   sweep;           // staged A/B transform sweep
    int   autoQuit;        // post WM_CLOSE when the capture completes
    int   probeClass;      // which surface class the probe transforms

    // Probe detail. Inert while probeClass is none / sweep is off, but reset
    // alongside the contract values so an unarmed config cannot leave a
    // half-configured capture behind.
    float delta[4];        // dx, dy, dz, yaw
    int   capFrames;       // draw-stream capture length (0 = no capture)
    int   capSkip;         // scene frames to let the map settle first
    int   quitAfter;       // scene frames before a no-capture quit (0 = never)
    int   sweepFrames;     // scene frames per sweep stage
    int   flatFocus;       // flat-run focus assist
    int   handSolveProbe;  // flat diag: zero-delta hand targets so the palm
                           // solve runs on real draws and reports in the log
    char  tag[48];         // capture file tag
};

// The hard defaults. Every field is set; nothing is left to whatever the
// caller's memory happened to contain.
void ProbeCfg_Production(CodvrProbeCfg* c);

// Offer one config key to the contract.
//
// Returns 1 if the key belongs to the probe system and was consumed (whether
// or not it was allowed to take effect), 0 if the caller should keep looking.
// When a probe key arrives unarmed it is consumed and DROPPED -- the reason
// text explains that in the log rather than leaving the reader to wonder why
// their setting did nothing.
//
// reason/reasonCap receive a human-readable line for the log. reason may be
// null if the caller does not want one.
int ProbeCfg_Apply(CodvrProbeCfg* c, const char* key, const char* text,
                   float val, char* reason, size_t reasonCap);

// Re-assert the contract after the whole file is parsed. Unarmed configs are
// forced back to the production values here, so no ordering of keys -- and no
// key this file has not heard of -- can leave a probe armed.
void ProbeCfg_Enforce(CodvrProbeCfg* c);

// The contract predicate: is this configuration safe to hand a player?
// True exactly when VR is enabled, the sweep is off, autoquit is off and no
// surface class is being transformed.
int ProbeCfg_IsProduction(const CodvrProbeCfg* c);
