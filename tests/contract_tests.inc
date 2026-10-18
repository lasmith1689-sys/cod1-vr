// ---------------------------------------------------------------------------
// PRODUCTION PROBE CONTRACT
// ---------------------------------------------------------------------------
//
// Regression cover for the 2026-08-25 deploy, where the attended flat
// surface-probe was left armed in a build a player launched. The game came up
// flat and posted itself WM_CLOSE about five seconds into gameplay. Nothing
// had broken -- the probe did what codvr.cfg told it to, in a configuration
// nobody meant to ship.
//
// These tests hold the line that the dangerous configuration is unreachable
// by accident: a stale override block, a missing file, a corrupt file, or an
// out-of-order file all end at VR-on / sweep-off / autoquit-off / no-probe.

#include "../src/vr_cfg_contract.h"
#include <stdlib.h>

// Feed a config body through the same three calls LoadConfig makes, in the
// same order, and hand back what the DLL would end up running.
static CodvrProbeCfg RunCfg(const char* body)
{
    CodvrProbeCfg c;
    ProbeCfg_Production(&c);
    if (body) {
        // Same tokenizer shape as LoadConfig: "key value", one per line,
        // lines that do not yield two tokens are skipped.
        const char* p = body;
        char line[256];
        while (*p) {
            size_t n = 0;
            while (p[n] && p[n] != '\n' && n < sizeof(line) - 1) ++n;
            memcpy(line, p, n);
            line[n] = 0;
            p += n;
            if (*p == '\n') ++p;
            if (line[0] == '#' || line[0] == '/' || line[0] == 0)
                continue;
            char key[64] = {0}, text[64] = {0};
            if (sscanf(line, "%63s %63s", key, text) != 2)
                continue;
            ProbeCfg_Apply(&c, key, text, (float)atof(text), nullptr, 0);
        }
    }
    ProbeCfg_Enforce(&c);
    return c;
}

// The exact block flat_run.ps1 appended to the game's codvr.cfg on
// 2026-08-25, reproduced verbatim minus the arming line that did not exist
// yet. This is the input that shipped a capture rig to a player.
static const char* const kStaleSweepBlock =
    "// ---- flat_run overrides: sweep ----\n"
    "vr_disable 1\n"
    "vr_glprobe_sweep 1\n"
    "vr_glprobe_sweep_frames 30\n"
    "vr_glcap_tag sweep\n"
    "vr_glcap_autoquit 1\n";

static void S_ProbeProductionContract()
{
    printf("--- production probe contract (the 2026-08-25 flat-deploy guard) ---\n");

    // ---- 1-3: the defaults themselves -----------------------------------
    {
        CodvrProbeCfg c;
        ProbeCfg_Production(&c);
        CHECK(c.vrDisabled == 0, "production defaults enable VR");
        CHECK(c.sweep == 0,      "production defaults disable the glsurf sweep");
        CHECK(c.autoQuit == 0,   "production defaults disable capture auto-quit");
        CHECK(c.probeClass == CODVR_PROBE_CLASS_NONE,
              "production defaults transform no surface class");
        CHECK(c.armed == 0,      "production defaults leave probes disarmed");
        CHECK(ProbeCfg_IsProduction(&c) == 1,
              "the production defaults satisfy the contract predicate");
    }

    // ---- 4: no config at all --------------------------------------------
    {
        CodvrProbeCfg c = RunCfg(nullptr);
        CHECK(ProbeCfg_IsProduction(&c) == 1,
              "a missing codvr.cfg cannot enable the flat probe");
        CHECK(c.vrDisabled == 0, "missing cfg leaves VR enabled");
        CHECK(c.autoQuit == 0,   "missing cfg leaves auto-quit off");
    }

    // ---- 5: malformed config --------------------------------------------
    {
        // Truncated keys, a value with no key, a key with no value, binary
        // noise, and half a probe line. None of it may arm anything.
        CodvrProbeCfg c = RunCfg(
            "vr_dis\n"
            "1\n"
            "vr_glcap_autoquit\n"
            "\x01\x02\x03 \x04\n"
            "vr_glprobe_sweep\n"
            "= = =\n"
            "vr_probe_ar 1\n"
            "vr_disable\n");
        CHECK(ProbeCfg_IsProduction(&c) == 1,
              "a malformed codvr.cfg cannot enable the flat probe");
        CHECK(c.armed == 0, "a near-miss arming key does not arm probes");
    }

    // ---- the actual incident input --------------------------------------
    {
        CodvrProbeCfg c = RunCfg(kStaleSweepBlock);
        CHECK(c.vrDisabled == 0,
              "stale sweep block: vr_disable 1 is ignored, VR stays enabled");
        CHECK(c.sweep == 0,
              "stale sweep block: vr_glprobe_sweep 1 is ignored");
        CHECK(c.autoQuit == 0,
              "stale sweep block: vr_glcap_autoquit 1 is ignored -- no self-close");
        CHECK(ProbeCfg_IsProduction(&c) == 1,
              "the 2026-08-25 override block now leaves a production build");
    }

    // ---- a whole realistic config that ends with the stale block --------
    {
        CodvrProbeCfg c = RunCfg(
            "renderSyscall 71\n"
            "supersample 2.25\n"
            "probe 0\n"
            "vr_twohand_enabled 0\n"
            "// ---- flat_run overrides: sweep ----\n"
            "vr_disable 1\n"
            "vr_glprobe_sweep 1\n"
            "vr_glcap_autoquit 1\n"
            "vr_glprobe_class 3\n"
            "vr_glprobe_dx 4.0\n");
        CHECK(ProbeCfg_IsProduction(&c) == 1,
              "a real config with the stale block appended still ships production");
        CHECK(c.probeClass == CODVR_PROBE_CLASS_NONE,
              "an unarmed probe class never reaches the renderer");
        CHECK(c.delta[0] == 0.0f, "an unarmed probe delta is cleared");
    }

    // ---- arming still works, or the capture tooling is dead -------------
    {
        CodvrProbeCfg c = RunCfg(
            "vr_probe_arm 1\n"
            "vr_disable 1\n"
            "vr_glprobe_sweep 1\n"
            "vr_glcap_autoquit 1\n"
            "vr_glprobe_sweep_frames 30\n"
            "vr_glcap_tag sweep\n");
        CHECK(c.armed == 1,       "vr_probe_arm 1 arms the probe system");
        CHECK(c.vrDisabled == 1,  "an armed config may disable VR");
        CHECK(c.sweep == 1,       "an armed config may run the sweep");
        CHECK(c.autoQuit == 1,    "an armed config may auto-quit");
        CHECK(c.sweepFrames == 30, "an armed config sets the sweep stage length");
        CHECK(strcmp(c.tag, "sweep") == 0, "an armed config sets the capture tag");
        CHECK(ProbeCfg_IsProduction(&c) == 0,
              "an armed capture config is correctly NOT a production one");
    }

    // ---- ordering cannot defeat the contract ----------------------------
    {
        // Probe keys BEFORE the arm line. They are dropped on the way past,
        // and arming afterwards does not retroactively apply them.
        CodvrProbeCfg c = RunCfg(
            "vr_disable 1\n"
            "vr_glcap_autoquit 1\n"
            "vr_probe_arm 1\n");
        CHECK(c.armed == 1, "the late arm line is still honoured");
        CHECK(c.vrDisabled == 0,
              "probe keys ahead of the arm line do not take effect");
        CHECK(c.autoQuit == 0,
              "auto-quit ahead of the arm line does not take effect");
    }
    {
        // Armed, then disarmed again further down. Enforce must sweep up.
        CodvrProbeCfg c = RunCfg(
            "vr_probe_arm 1\n"
            "vr_disable 1\n"
            "vr_glprobe_sweep 1\n"
            "vr_glcap_autoquit 1\n"
            "vr_probe_arm 0\n");
        CHECK(c.armed == 0, "vr_probe_arm 0 disarms");
        CHECK(ProbeCfg_IsProduction(&c) == 1,
              "disarming after the fact restores every contract value");
    }

    // ---- key routing ----------------------------------------------------
    {
        CodvrProbeCfg c;
        ProbeCfg_Production(&c);
        CHECK(ProbeCfg_Apply(&c, "vr_disable", "1", 1.0f, nullptr, 0) == 1,
              "vr_disable is claimed by the contract");
        CHECK(ProbeCfg_Apply(&c, "vr_glcap_autoquit", "1", 1.0f, nullptr, 0) == 1,
              "vr_glcap_autoquit is claimed by the contract");
        CHECK(ProbeCfg_Apply(&c, "vr_glprobe_yaw", "15", 15.0f, nullptr, 0) == 1,
              "vr_glprobe_yaw is claimed by the contract");
        CHECK(ProbeCfg_Apply(&c, "supersample", "2.25", 2.25f, nullptr, 0) == 0,
              "an unrelated key is left for the rest of the parser");
        CHECK(ProbeCfg_Apply(&c, "vr_glsurf_route", "1", 1.0f, nullptr, 0) == 0,
              "vr_glsurf_route is not a probe key and passes through");

        char reason[256] = {0};
        ProbeCfg_Apply(&c, "vr_disable", "1", 1.0f, reason, sizeof(reason));
        CHECK(strstr(reason, "IGNORED") != nullptr,
              "a dropped probe key says so in the log rather than failing silently");
    }

    // ---- MUTANT: skipping Enforce must be detectable --------------------
    {
        // Simulates the pre-fix build: probe keys applied unconditionally,
        // no post-parse contract. The predicate has to call this out, or the
        // tests above would pass against a broken implementation.
        CodvrProbeCfg mutant;
        ProbeCfg_Production(&mutant);
        mutant.vrDisabled = 1;
        mutant.sweep      = 1;
        mutant.autoQuit   = 1;
        CHECK(ProbeCfg_IsProduction(&mutant) == 0,
              "MUTANT: the pre-fix flat/sweep/autoquit state fails the contract");
        ProbeCfg_Enforce(&mutant);
        CHECK(ProbeCfg_IsProduction(&mutant) == 1,
              "MUTANT: post-parse enforcement repairs it");
    }
    {
        // A probe class alone -- no sweep, no autoquit, VR still on -- is
        // still not production: it silently moves a surface in a player's view.
        CodvrProbeCfg mutant;
        ProbeCfg_Production(&mutant);
        mutant.probeClass = 3;
        CHECK(ProbeCfg_IsProduction(&mutant) == 0,
              "MUTANT: a live probe class alone already breaks the contract");
    }
}
