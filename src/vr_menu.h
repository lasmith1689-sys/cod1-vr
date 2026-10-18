// CODVR VR OPTIONS -- the in-headset settings overlay (project-lead brief
// 2026-09-04, P4, sections 17-19).
//
// A CODVR-owned overlay, not a patch of the game's own menu: injecting a
// "VR Options" entry into the original menu architecture would mean editing
// the protected executable or its menu scripts, which is out of scope. The
// overlay is drawn by the proxy into each finished eye image (the same
// hook the comfort vignette uses), navigated with the controllers alone,
// shows every current value, applies a change immediately through the same
// code path codvr.cfg takes at load, and saves back to codvr.cfg.
//
// OPEN / CLOSE: hold the LEFT stick click for VRMENU_HOLD_S. While open the
// overlay owns the controllers (vr_binds injects nothing): stick up/down
// moves the selection, stick left/right or A adjusts, B saves and closes.
// Settings that need a restart say so and are saved without being applied.
#pragma once

struct VRInputState;

#define VRMENU_HOLD_S 0.6

// One settings row. kind: 0 toggle, 1 number, 2 enum, 3 save, 4 controls reference.
struct VRMenuItem {
    const char* group;
    const char* label;
    const char* key;          // codvr.cfg key (null for actions)
    int         kind;
    float       minV, maxV, step;
    int         decimals;
    const char* enumText[3];  // enum: the config words; enumIdx indexes them
    int         nEnum;
    int         restart;      // 1 = takes effect at the next launch (saved only)
    float       value;        // toggle 0/1, number value
    int         enumIdx;
    int         dirty;        // changed since the last save
};

typedef void (*VRMenuApplyFn)(const char* key, const char* text, float val);

void VRMenu_Init(const char* cfgPath, VRMenuApplyFn apply);
// Every key codvr.cfg applied at load, so the overlay shows current values.
void VRMenu_NoteConfig(const char* key, const char* text, float val);
// Once per game frame, before vr_binds: reads the controllers, drives the
// overlay, and decides whether the overlay owns the input this frame.
void VRMenu_Update(const VRInputState* in, double now);
int  VRMenu_Open();
// Draw into a finished eye image (after capture, like the vignette). Safe to
// call when closed (returns immediately). Returns 1 when something was drawn.
int  VRMenu_Draw(int eye);

// ---- the model, for tests and diagnostics --------------------------------
int  VRMenu_ItemCount();
const VRMenuItem* VRMenu_Item(int i);
int  VRMenu_Selected();
void VRMenu_ItemValueText(int i, char* buf, int n);
// Drive the model without a controller: each flag is one edge / level.
void VRMenu_TestFeed(int up, int down, int left, int right, int accept, int back, double now);
void VRMenu_TestSetOpen(int on);
// Serialise the current values as "key value" lines; returns the count.
int  VRMenu_Serialize(char* buf, int n);
// Write the values into a codvr.cfg: lines whose key matches are replaced
// in place, unknown keys are appended under a marker. Returns the number of
// keys written, -1 on failure. `path` null = the path given to Init.
int  VRMenu_SaveToFile(const char* path);
unsigned VRMenu_ApplyCount();     // live applies delivered through the hook
unsigned VRMenu_SaveCount();
int  VRMenu_Dirty();
