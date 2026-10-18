// vr_snapshot -- in-engine eye snapshots (request of 2026-09-06).
//
// Writes the FINISHED eye image (after the vignette and the VR Options
// overlay, i.e. exactly what the headset shows) to a PNG in a `snapshots`
// folder beside the DLL, plus a small text sidecar with the controller and
// hand/gun telemetry of that pair, so the render can be reviewed offline
// next to the numbers.
//
// Production-safe by construction: it never arms probes, captures, or
// autoquit; it reads the eye texture back with glGetTexImage, touches no
// engine state, and is rate limited (interval timer, trigger debounce, a
// hard count cap). Everything is off unless `vr_snapshot 1`.
#pragma once

struct VRInputState;

// Config (all through ApplyConfigKey): returns 1 when the key was consumed.
int  VRSnap_ConfigKey(const char* key, float val);

// Pure decision logic, test seam. Called once per pair from the frame
// boundary; decides whether the coming pair is snapshotted.
struct VRSnapConfig {
    int   enabled;        // vr_snapshot
    float intervalS;      // vr_snapshot_interval_s (0 = no timer)
    int   onTrigger;      // vr_snapshot_on_trigger: right trigger press edge
    int   onButton;       // vr_snapshot_on_button: right A press edge
    int   maxCount;       // vr_snapshot_max (files per session, pairs)
    int   scaleDiv;       // vr_snapshot_scale: integer downsample (1..8)
    float debounceS;      // minimum spacing between any two snapshots
};
struct VRSnapState {
    double lastSnapS;     // -1 until the first
    int    taken;         // pairs written
    int    prevTrigger;   // for edge detection
    int    prevButton;
};
// Returns a bitmask of reasons (VRSNAP_WHY_*), 0 = no snapshot this pair.
enum { VRSNAP_WHY_TIMER = 1, VRSNAP_WHY_TRIGGER = 2, VRSNAP_WHY_BUTTON = 4, VRSNAP_WHY_FIRST = 8 };
int  VRSnap_Decide(const VRSnapConfig* cfg, VRSnapState* st, double nowS,
                   int triggerDown, int buttonDown);

// Runtime hooks.
void VRSnap_Init(const char* dllPath);                    // folder beside the DLL
void VRSnap_Update(const VRInputState* in, double nowS);  // once per pair, frame boundary
void VRSnap_AfterEye(int eye);                            // after the overlay, per eye
void VRSnap_GetConfig(VRSnapConfig* out);

// PNG encoder (stored deflate, no library), test seam. rgb is w*h*3, top
// row first. Returns bytes written, 0 on failure.
int  VRSnap_WritePng(const char* path, const unsigned char* rgb, int w, int h);
// Box downsample by an integer divisor into out (sized (w/div)*(h/div)*3).
void VRSnap_Downsample(const unsigned char* rgb, int w, int h, int div,
                       unsigned char* out, int* outW, int* outH);
