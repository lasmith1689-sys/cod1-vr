// CODVR VR OPTIONS -- see vr_menu.h.
#include "vr_menu.h"
#include "vr_input.h"
#include "vr.h"
#include "codvr.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

// ---------------------------------------------------------------- the model

static VRMenuItem g_items[] = {
    // MOVEMENT
    { "MOVEMENT", "Turn mode",           "vr_turn_mode",          2, 0, 0, 0, 0, { "smooth", "snap", "off" }, 3, 0, 0, 0, 0 },
    { "MOVEMENT", "Smooth turn speed",   "vr_smooth_turn_speed",  1, 30, 180, 15, 0, { 0, 0, 0 }, 0, 0, 90, 0, 0 },
    { "MOVEMENT", "Snap angle",          "vr_snap_angle",         1, 15, 90, 15, 0, { 0, 0, 0 }, 0, 0, 30, 0, 0 },
    { "MOVEMENT", "Move direction",      "vr_move_direction",     2, 0, 0, 0, 0, { "hmd", "offhand", 0 }, 2, 0, 0, 0, 0 },
    { "MOVEMENT", "Comfort vignette",    "vr_comfort_vignette",   0, 0, 1, 1, 0, { 0, 0, 0 }, 0, 0, 0, 0, 0 },
    { "COMFORT", "Seated height offset m", "vr_height_adjust", 1, -0.5f, 1.0f, 0.05f, 2, {0,0,0}, 0,0,0,0,0 },
    { "COMFORT", "Physical crouching", "vr_physical_crouch", 0, 0,1,1,0, {0,0,0}, 0,0,0,0,0 },
    { "COMFORT", "Crouch drop m", "vr_crouch_drop", 1, 0.15f,0.60f,0.05f,2, {0,0,0}, 0,0,0.32f,0,0 },
    { "CONTROLS", "Reload / weapon switch", "vr_weapon_cycle_binding", 2, 0,0,0,0, {"x_hold","offhand_x","stick"}, 3,0,0,0,0 },
    { "CONTROLS", "Grenade hold seconds", "vr_grenade_hold", 1, 0.25f,1.0f,0.05f,2, {0,0,0}, 0,0,0.45f,0,0 },
    { "CONTROLS", "Controller vibration", "vr_haptics", 0, 0,1,1,0, {0,0,0}, 0,0,1,0,0 },
    { "CONTROLS", "Vibration strength", "vr_haptics_strength", 1, 0,1,0.1f,1, {0,0,0}, 0,0,0.6f,0,0 },
    { "CONTROLS", "Controls reference", 0, 4, 0,0,0,0, {0,0,0}, 0,0,0,0,0 },
    { "HANDS / WEAPON", "Arm visibility", "vr_sleeve_visible", 1, 0,2,1,0, {0,0,0}, 0,0,0,0,0 },
    { "HANDS / WEAPON", "Support stabilization", "vr_twohand_stability", 1, 0,1,0.05f,2, {0,0,0}, 0,0,0.75f,0,0 },
    { "HANDS / WEAPON", "Supported kick reduction", "vr_twohand_recoil_reduction", 1, 0,1,0.05f,2, {0,0,0}, 0,0,0.80f,0,0 },
    { "HANDS / WEAPON", "Support hand rides the gun", "vr_support_pose_lock", 0, 0,1,1,0, {0,0,0}, 0,0,1,0,0 },
    { "HANDS / WEAPON", "Trigger hand rides the gun", "vr_trigger_hand_lock", 0, 0,1,1,0, {0,0,0}, 0,0,1,0,0 },
    { "HANDS / WEAPON", "Physical reload gesture", "vr_native_hands_reload", 0, 0,1,1,0, {0,0,0}, 0,0,1,0,0 },
    // HANDS / WEAPON
    { "HANDS / WEAPON", "Native hands",          "vr_native_hands",          0, 0, 1, 1, 0, { 0, 0, 0 }, 0, 0, 1, 0, 0 },
    { "HANDS / WEAPON", "Right hand trim fwd",   "vr_native_hands_r_off_fwd",  1, -0.10f, 0.10f, 0.01f, 2, { 0, 0, 0 }, 0, 0, 0, 0, 0 },
    { "HANDS / WEAPON", "Right hand trim left",  "vr_native_hands_r_off_left", 1, -0.10f, 0.10f, 0.01f, 2, { 0, 0, 0 }, 0, 0, 0, 0, 0 },
    { "HANDS / WEAPON", "Right hand trim up",    "vr_native_hands_r_off_up",   1, -0.10f, 0.10f, 0.01f, 2, { 0, 0, 0 }, 0, 0, 0, 0, 0 },
    { "HANDS / WEAPON", "Right hand pitch",      "vr_native_hands_r_pitch",    1, -45, 45, 1, 2, { 0, 0, 0 }, 0, 0, 0, 0, 0 },
    { "HANDS / WEAPON", "Right hand yaw",        "vr_native_hands_r_yaw",      1, -45, 45, 1, 2, { 0, 0, 0 }, 0, 0, 0, 0, 0 },
    { "HANDS / WEAPON", "Right hand roll",       "vr_native_hands_r_roll",     1, -45, 45, 1, 2, { 0, 0, 0 }, 0, 0, 0, 0, 0 },
    { "HANDS / WEAPON", "Left hand trim fwd",    "vr_native_hands_l_off_fwd",  1, -0.10f, 0.10f, 0.01f, 2, { 0, 0, 0 }, 0, 0, 0, 0, 0 },
    { "HANDS / WEAPON", "Left hand trim left",   "vr_native_hands_l_off_left", 1, -0.10f, 0.10f, 0.01f, 2, { 0, 0, 0 }, 0, 0, 0, 0, 0 },
    { "HANDS / WEAPON", "Left hand trim up",     "vr_native_hands_l_off_up",   1, -0.10f, 0.10f, 0.01f, 2, { 0, 0, 0 }, 0, 0, 0, 0, 0 },
    { "HANDS / WEAPON", "Left hand pitch",       "vr_native_hands_l_pitch",    1, -45, 45, 1, 2, { 0, 0, 0 }, 0, 0, 0, 0, 0 },
    { "HANDS / WEAPON", "Left hand yaw",         "vr_native_hands_l_yaw",      1, -45, 45, 1, 2, { 0, 0, 0 }, 0, 0, 0, 0, 0 },
    { "HANDS / WEAPON", "Left hand roll",        "vr_native_hands_l_roll",     1, -45, 45, 1, 2, { 0, 0, 0 }, 0, 0, 0, 0, 0 },
    { "HANDS / WEAPON", "Gun trim fwd",          "vr_gun_off_fwd",   1, -0.10f, 0.10f, 0.01f, 2, { 0, 0, 0 }, 0, 0, 0, 0, 0 },
    { "HANDS / WEAPON", "Gun trim left",         "vr_gun_off_left",  1, -0.10f, 0.10f, 0.01f, 2, { 0, 0, 0 }, 0, 0, 0, 0, 0 },
    { "HANDS / WEAPON", "Gun trim up",           "vr_gun_off_up",    1, -0.10f, 0.10f, 0.01f, 2, { 0, 0, 0 }, 0, 0, 0, 0, 0 },
    { "HANDS / WEAPON", "Gun pitch",             "vr_gun_pitch",     1, -45, 45, 1, 2, { 0, 0, 0 }, 0, 0, 0, 0, 0 },
    { "HANDS / WEAPON", "Gun yaw",               "vr_gun_yaw",       1, -45, 45, 1, 2, { 0, 0, 0 }, 0, 0, 0, 0, 0 },
    { "HANDS / WEAPON", "Gun roll",              "vr_gun_roll",      1, -45, 45, 1, 2, { 0, 0, 0 }, 0, 0, 0, 0, 0 },
    { "HANDS / WEAPON", "Two-hand support",      "vr_native_hands_twohand",  0, 0, 1, 1, 0, { 0, 0, 0 }, 0, 0, 1, 0, 0 },
    { "HANDS / WEAPON", "Two-hand grab radius cm", "vr_twohand_grab_cm",     1, 6, 25, 1, 0, { 0, 0, 0 }, 0, 0, 12, 0, 0 },
    { "HANDS / WEAPON", "Two-hand blend s",      "vr_twohand_blend_s",       1, 0, 0.30f, 0.02f, 2, { 0, 0, 0 }, 0, 0, 0.08f, 0, 0 },
    // AIM
    { "AIM", "Physical aim",             "vr_physical_aim",       0, 0, 1, 1, 0, { 0, 0, 0 }, 0, 0, 1, 0, 0 },
    { "AIM", "Aim crosshair (next launch)", "vr_crosshair",       0, 0, 1, 1, 0, { 0, 0, 0 }, 0, 0, 0, 0, 0 },
    // DISPLAY
    { "DISPLAY", "HMD brightness",       "vr_brightness",         1, 0.30f, 1.00f, 0.05f, 2, { 0, 0, 0 }, 0, 0, 0.85f, 0, 0 },
    // UI
    { "UI", "Menu / loading size", "vr_menu_scale", 1, 0.50f,0.90f,0.025f,3, {0,0,0}, 0,0,0.65f,0,0 },
    { "UI", "Menu distance m (0=far)", "vr_menu_distance", 1, 0,5,0.5f,1, {0,0,0}, 0,0,0,0,0 },
    { "UI", "UI scale",                  "vr_ui_scale",           1, 0.70f, 1.00f, 0.02f, 2, { 0, 0, 0 }, 0, 0, 0.92f, 0, 0 },
    { "UI", "UI safe width",             "vr_ui_safe_x",          1, 0.70f, 1.00f, 0.02f, 2, { 0, 0, 0 }, 0, 0, 1.00f, 0, 0 },
    { "UI", "UI safe height",            "vr_ui_safe_y",          1, 0.70f, 1.00f, 0.02f, 2, { 0, 0, 0 }, 0, 0, 1.00f, 0, 0 },
    { "UI", "HUD scale",                 "vr_hud_scale",          1, 0.50f, 1.00f, 0.05f, 2, { 0, 0, 0 }, 0, 0, 0.73f, 0, 0 },
    // QUALITY
    { "QUALITY", "Supersample",          "supersample",           1, 1.00f, 2.50f, 0.25f, 2, { 0, 0, 0 }, 0, 1, 2.00f, 0, 0 },
    // ACTIONS
    { "", "Save and close",              0,                       3, 0, 0, 0, 0, { 0, 0, 0 }, 0, 0, 0, 0, 0 },
};
enum { N_ITEMS = (int)(sizeof(g_items) / sizeof(g_items[0])) };

static int           g_open;
static int           g_controlsHelp;
static int           g_selected;
static char          g_cfgPath[MAX_PATH];
static VRMenuApplyFn g_apply;
static unsigned      g_applyCount, g_saveCount;
static char          g_status[96];
static double        g_statusUntil = -1.0;

// Input edge state.
static double g_holdSince = -1.0;      // left stick click held since
static int    g_holdFired;
static int    g_prevUp, g_prevDown, g_prevLeft, g_prevRight, g_prevA, g_prevB;
static double g_repeatAt = -1.0;

int  VRMenu_ItemCount() { return N_ITEMS; }
const VRMenuItem* VRMenu_Item(int i) { return (i >= 0 && i < N_ITEMS) ? &g_items[i] : nullptr; }
int  VRMenu_Selected() { return g_selected; }
int  VRMenu_Open() { return g_open; }
unsigned VRMenu_ApplyCount() { return g_applyCount; }
unsigned VRMenu_SaveCount() { return g_saveCount; }
int  VRMenu_Dirty()
{
    for (int i = 0; i < N_ITEMS; ++i) if (g_items[i].dirty) return 1;
    return 0;
}

void VRMenu_Init(const char* cfgPath, VRMenuApplyFn apply)
{
    g_cfgPath[0] = 0;
    if (cfgPath) strncpy_s(g_cfgPath, cfgPath, _TRUNCATE);
    g_apply = apply;
    g_open = 0;
    g_controlsHelp = 0;
    g_selected = 0;
    g_holdSince = -1.0;
    g_holdFired = 0;
    g_prevUp = g_prevDown = g_prevLeft = g_prevRight = g_prevA = g_prevB = 0;
    for (int i = 0; i < N_ITEMS; ++i) g_items[i].dirty = 0;
}

void VRMenu_NoteConfig(const char* key, const char* text, float val)
{
    if (!key) return;
    for (int i = 0; i < N_ITEMS; ++i) {
        VRMenuItem& it = g_items[i];
        if (!it.key || _stricmp(it.key, key) != 0) continue;
        if (it.kind == 2) {
            for (int e = 0; e < it.nEnum; ++e)
                if (text && _stricmp(it.enumText[e], text) == 0) it.enumIdx = e;
        } else if (it.kind == 0) {
            it.value = (val != 0.0f) ? 1.0f : 0.0f;
        } else if (it.kind == 1) {
            it.value = val;
        }
        it.dirty = 0;
    }
}

void VRMenu_ItemValueText(int i, char* buf, int n)
{
    if (!buf || n <= 0) return;
    buf[0] = 0;
    const VRMenuItem* it = VRMenu_Item(i);
    if (!it) return;
    if (it->key && !strcmp(it->key,"vr_sleeve_visible")) {
        snprintf(buf,n,"%s",it->value<1 ? "None" : it->value<2 ? "Right" : "Both"); return;
    }
    if (it->key && (!strcmp(it->key,"vr_twohand_stability") || !strcmp(it->key,"vr_twohand_recoil_reduction")
        || !strcmp(it->key,"vr_haptics_strength"))) {
        snprintf(buf,n,"%.0f%%",it->value*100); return;
    }
    if (it->kind==2 && it->key && !strcmp(it->key,"vr_weapon_cycle_binding")) {
        const char* t=it->enumText[it->enumIdx];
        snprintf(buf,n,"%s",!strcmp(t,"x_hold") ? "X tap / hold" : !strcmp(t,"stick") ? "Stick cycle" : "X cycle"); return;
    }
    if (it->kind==2 && it->key && !strcmp(it->key,"vr_move_direction")) {
        snprintf(buf,n,"%s",it->enumIdx ? "Left hand" : "Head"); return;
    }
    switch (it->kind) {
        case 0: snprintf(buf, n, "%s", it->value != 0.0f ? "ON" : "OFF"); break;
        case 1: snprintf(buf, n, "%.*f%s", it->decimals, it->value, it->restart ? " (restart)" : ""); break;
        case 2: snprintf(buf, n, "%s", it->enumText[it->enumIdx]); break;
        default: buf[0] = 0; break;
    }
}

static void ApplyItem(VRMenuItem& it)
{
    it.dirty = 1;
    if (!g_apply || !it.key || it.restart) return;
    char text[32];
    if (it.kind == 2) snprintf(text, sizeof(text), "%s", it.enumText[it.enumIdx]);
    else              snprintf(text, sizeof(text), "%.*f", it.decimals, it.value);
    g_apply(it.key, text, (float)atof(text));
    ++g_applyCount;
}

static void Adjust(int dir)
{
    VRMenuItem& it = g_items[g_selected];
    switch (it.kind) {
        case 0: it.value = it.value != 0.0f ? 0.0f : 1.0f; ApplyItem(it); break;
        case 1: {
            float v = it.value + dir * it.step;
            // snap to the step grid so repeated presses cannot accumulate drift
            v = floorf(v / it.step + 0.5f) * it.step;
            if (v < it.minV - 1e-6f) v = it.minV;
            if (v > it.maxV + 1e-6f) v = it.maxV;
            if (fabsf(v - it.value) < 1e-6f) return;
            it.value = v;
            ApplyItem(it);
            break;
        }
        case 2: {
            int e = it.enumIdx + (dir >= 0 ? 1 : -1);
            if (e < 0) e = it.nEnum - 1;
            if (e >= it.nEnum) e = 0;
            it.enumIdx = e;
            ApplyItem(it);
            break;
        }
        default: break;
    }
}

static void SetStatus(const char* s, double now)
{
    strncpy_s(g_status, s, _TRUNCATE);
    g_statusUntil = now + 2.5;
}

static void SaveAndClose(double now)
{
    const int n = VRMenu_SaveToFile(nullptr);
    if (n >= 0) SetStatus("saved to codvr.cfg", now);
    else        SetStatus("SAVE FAILED (codvr.cfg not writable)", now);
    g_open = 0;
}

static void Activate(double now)
{
    VRMenuItem& it = g_items[g_selected];
    if (it.kind == 3) { SaveAndClose(now); return; }
    if (it.kind == 4) { g_controlsHelp=1; return; }
    Adjust(+1);
}

static void Step(int up, int down, int left, int right, int accept, int back, double now)
{
    if (!g_open) return;
    if (g_controlsHelp) { if (back || accept) g_controlsHelp=0; return; }
    if (back) { SaveAndClose(now); return; }
    if (up)   { g_selected = (g_selected + N_ITEMS - 1) % N_ITEMS; }
    if (down) { g_selected = (g_selected + 1) % N_ITEMS; }
    if (left)  Adjust(-1);
    if (right) Adjust(+1);
    if (accept) Activate(now);
}

void VRMenu_TestFeed(int up, int down, int left, int right, int accept, int back, double now)
{
    Step(up, down, left, right, accept, back, now);
}

void VRMenu_TestSetOpen(int on)
{
    g_open = on ? 1 : 0;
    if (on) { g_selected = 0; g_controlsHelp=0; }
}

void VRMenu_Update(const VRInputState* in, double now)
{
    if (!in || !in->valid) {
        g_holdSince = -1.0;
        return;
    }
    const VRHandState& L = in->hand[VRI_LEFT];
    const VRHandState& R = in->hand[VRI_RIGHT];
    // Open / close: hold the LEFT stick click.
    if (L.active && L.stickClick) {
        if (g_holdSince < 0.0) { g_holdSince = now; g_holdFired = 0; }
        else if (!g_holdFired && now - g_holdSince >= VRMENU_HOLD_S) {
            g_holdFired = 1;
            if (g_open) SaveAndClose(now);
            else { g_open = 1; g_controlsHelp=0; g_selected = 0; g_repeatAt = -1.0; }
        }
    } else {
        g_holdSince = -1.0;
        g_holdFired = 0;
    }
    if (!g_open) return;
    // Either stick navigates; either A adjusts; either B saves and closes.
    const float sy = fmaxf(fabsf(L.stickY), fabsf(R.stickY)) > 0.0f
                   ? (fabsf(L.stickY) > fabsf(R.stickY) ? L.stickY : R.stickY) : 0.0f;
    const float sx = fabsf(L.stickX) > fabsf(R.stickX) ? L.stickX : R.stickX;
    const int up = sy > 0.6f, down = sy < -0.6f, right = sx > 0.6f, left = sx < -0.6f;
    const int a = (L.active && L.btnPrimary) || (R.active && R.btnPrimary);
    const int b = (L.active && L.btnSecondary) || (R.active && R.btnSecondary);
    int eUp = up && !g_prevUp, eDown = down && !g_prevDown;
    int eLeft = left && !g_prevLeft, eRight = right && !g_prevRight;
    const int eA = a && !g_prevA, eB = b && !g_prevB;
    // Held stick directions repeat every 0.25 s.
    if ((up || down || left || right) && !(eUp || eDown || eLeft || eRight)) {
        if (g_repeatAt < 0.0) g_repeatAt = now + 0.35;
        else if (now >= g_repeatAt) {
            g_repeatAt = now + 0.25;
            eUp = up; eDown = down; eLeft = left; eRight = right;
        }
    } else if (!(up || down || left || right)) {
        g_repeatAt = -1.0;
    }
    g_prevUp = up; g_prevDown = down; g_prevLeft = left; g_prevRight = right;
    g_prevA = a; g_prevB = b;
    Step(eUp, eDown, eLeft, eRight, eA, eB, now);
}

// ---------------------------------------------------------------- persist

int VRMenu_Serialize(char* buf, int n)
{
    if (!buf || n <= 0) return 0;
    int used = 0, count = 0;
    buf[0] = 0;
    for (int i = 0; i < N_ITEMS; ++i) {
        const VRMenuItem& it = g_items[i];
        if (!it.key) continue;
        char text[32];
        if (it.kind == 2) snprintf(text, sizeof(text), "%s", it.enumText[it.enumIdx]);
        else              snprintf(text, sizeof(text), "%.*f", it.decimals, it.value);
        const int w = snprintf(buf + used, n - used, "%s %s\n", it.key, text);
        if (w < 0 || used + w >= n) break;
        used += w;
        ++count;
    }
    return count;
}

static int LineKeyMatches(const char* line, const char* key)
{
    // "key value" -- the key ends at the first space/tab.
    const size_t kl = strlen(key);
    if (_strnicmp(line, key, kl) != 0) return 0;
    return line[kl] == ' ' || line[kl] == '\t';
}

int VRMenu_SaveToFile(const char* path)
{
    const char* p = (path && path[0]) ? path : g_cfgPath;
    if (!p || !p[0]) return -1;
    // Read the whole file (it is small), rewrite it with the menu's keys
    // replaced in place and the rest untouched; append what was missing.
    char* old = nullptr;
    long oldLen = 0;
    FILE* f = nullptr;
    if (fopen_s(&f, p, "rb") == 0 && f) {
        fseek(f, 0, SEEK_END);
        oldLen = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (oldLen > 0 && oldLen < 4 * 1024 * 1024) {
            old = (char*)malloc((size_t)oldLen + 1);
            if (old) {
                const size_t got = fread(old, 1, (size_t)oldLen, f);
                old[got] = 0;
                oldLen = (long)got;
            }
        }
        fclose(f);
    }
    int written[N_ITEMS];
    memset(written, 0, sizeof(written));
    char tmp[MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    if (fopen_s(&f, tmp, "wb") != 0 || !f) { free(old); return -1; }
    int count = 0;
    if (old) {
        const char* s = old;
        while (*s) {
            const char* e = strchr(s, '\n');
            const size_t len = e ? (size_t)(e - s + 1) : strlen(s);
            int replaced = 0;
            if (s[0] != '#' && s[0] != '/') {
                for (int i = 0; i < N_ITEMS; ++i) {
                    const VRMenuItem& it = g_items[i];
                    if (!it.key || written[i] || !LineKeyMatches(s, it.key)) continue;
                    char text[32];
                    if (it.kind == 2) snprintf(text, sizeof(text), "%s", it.enumText[it.enumIdx]);
                    else              snprintf(text, sizeof(text), "%.*f", it.decimals, it.value);
                    const int crlf = len >= 2 && s[len - 2] == '\r';
                    fprintf(f, "%s %s%s", it.key, text, crlf ? "\r\n" : "\n");
                    written[i] = 1;
                    replaced = 1;
                    ++count;
                    break;
                }
            }
            if (!replaced) fwrite(s, 1, len, f);
            s += len;
        }
        free(old);
    }
    int missing = 0;
    for (int i = 0; i < N_ITEMS; ++i)
        if (g_items[i].key && !written[i]) ++missing;
    if (missing) {
        fprintf(f, "\n// VR Options saved in-game\n");
        for (int i = 0; i < N_ITEMS; ++i) {
            const VRMenuItem& it = g_items[i];
            if (!it.key || written[i]) continue;
            char text[32];
            if (it.kind == 2) snprintf(text, sizeof(text), "%s", it.enumText[it.enumIdx]);
            else              snprintf(text, sizeof(text), "%.*f", it.decimals, it.value);
            fprintf(f, "%s %s\n", it.key, text);
            ++count;
        }
    }
    fclose(f);
    if (!MoveFileExA(tmp, p, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(tmp);
        return -1;
    }
    for (int i = 0; i < N_ITEMS; ++i) g_items[i].dirty = 0;
    ++g_saveCount;
    Log("VR options: saved %d keys to %s", count, p);
    return count;
}

// ---------------------------------------------------------------- drawing
//
// Fixed-function GL into the finished eye texture through our own FBO, the
// vignette's snapshot/restore discipline, and a font atlas rasterised once
// with GDI (no bitmap font is shipped with the game we can read safely).

typedef void (WINAPI *PFN_GenTextures)(int, unsigned*);
typedef void (WINAPI *PFN_BindTexture)(unsigned, unsigned);
typedef void (WINAPI *PFN_TexImage2D)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*);
typedef void (WINAPI *PFN_TexParameteri)(unsigned,unsigned,int);
typedef void (WINAPI *PFN_TexEnvi)(unsigned,unsigned,int);
typedef void (WINAPI *PFN_Enable)(unsigned);
typedef void (WINAPI *PFN_Disable)(unsigned);
typedef unsigned char (WINAPI *PFN_IsEnabled)(unsigned);
typedef void (WINAPI *PFN_BlendFunc)(unsigned,unsigned);
typedef void (WINAPI *PFN_MatrixMode)(unsigned);
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
typedef void (WINAPI *PFN_GetFloatv)(unsigned, float*);
typedef void (WINAPI *PFN_LoadMatrixf)(const float*);
typedef void (WINAPI *PFN_ActiveTexture)(unsigned);
typedef void (WINAPI *PFN_ColorMask)(unsigned char, unsigned char, unsigned char, unsigned char);
typedef void (WINAPI *PFN_ShadeModel)(unsigned);

static PFN_GenTextures gl_GenTextures; static PFN_BindTexture gl_BindTexture;
static PFN_TexImage2D gl_TexImage2D; static PFN_TexParameteri gl_TexParameteri;
static PFN_TexEnvi gl_TexEnvi; static PFN_Enable gl_Enable; static PFN_Disable gl_Disable;
static PFN_IsEnabled gl_IsEnabled; static PFN_BlendFunc gl_BlendFunc;
static PFN_MatrixMode gl_MatrixMode; static PFN_LoadIdentity gl_LoadIdentity;
static PFN_Ortho gl_Ortho; static PFN_Begin gl_Begin; static PFN_End gl_End;
static PFN_Color4f gl_Color4f; static PFN_TexCoord2f gl_TexCoord2f; static PFN_Vertex2f gl_Vertex2f;
static PFN_Viewport gl_Viewport; static PFN_GenFramebuffers gl_GenFramebuffers;
static PFN_BindFramebuffer gl_BindFramebuffer; static PFN_FramebufferTexture2D gl_FramebufferTexture2D;
static PFN_GetIntegerv gl_GetIntegerv; static PFN_GetFloatv gl_GetFloatv;
static PFN_LoadMatrixf gl_LoadMatrixf; static PFN_ActiveTexture gl_ActiveTexture;
static PFN_ColorMask gl_ColorMask; static PFN_ShadeModel gl_ShadeModel;
static bool g_glTried, g_glReady;

enum {
    M_GL_TEXTURE_2D = 0x0DE1, M_GL_BLEND = 0x0BE2, M_GL_SRC_ALPHA = 0x0302,
    M_GL_ONE_MINUS_SRC_ALPHA = 0x0303, M_GL_DEPTH_TEST = 0x0B71, M_GL_SCISSOR_TEST = 0x0C11,
    M_GL_CULL_FACE = 0x0B44, M_GL_LIGHTING = 0x0B50, M_GL_PROJECTION = 0x1701,
    M_GL_MODELVIEW = 0x1700, M_GL_QUADS = 0x0007, M_GL_RGBA = 0x1908, M_GL_RGBA8 = 0x8058,
    M_GL_UNSIGNED_BYTE = 0x1401, M_GL_LINEAR = 0x2601, M_GL_CLAMP_TO_EDGE = 0x812F,
    M_GL_TEXTURE_MIN_FILTER = 0x2801, M_GL_TEXTURE_MAG_FILTER = 0x2800,
    M_GL_TEXTURE_WRAP_S = 0x2802, M_GL_TEXTURE_WRAP_T = 0x2803, M_GL_FRAMEBUFFER = 0x8D40,
    M_GL_COLOR_ATTACHMENT0 = 0x8CE0, M_GL_VIEWPORT = 0x0BA2, M_GL_TEXTURE_BINDING_2D = 0x8069,
    M_GL_BLEND_SRC = 0x0BE1, M_GL_BLEND_DST = 0x0BE0, M_GL_SHADE_MODEL = 0x0B54,
    M_GL_SMOOTH = 0x1D01, M_GL_MATRIX_MODE = 0x0BA0, M_GL_CURRENT_COLOR = 0x0B00,
    M_GL_MODELVIEW_MATRIX = 0x0BA6, M_GL_PROJECTION_MATRIX = 0x0BA7, M_GL_ALPHA_TEST = 0x0BC0,
    M_GL_FOG = 0x0B60, M_GL_COLOR_WRITEMASK = 0x0C23, M_GL_ACTIVE_TEXTURE = 0x84E0,
    M_GL_TEXTURE0 = 0x84C0, M_GL_DRAW_FRAMEBUFFER_BINDING = 0x8CA6,
    M_GL_READ_FRAMEBUFFER_BINDING = 0x8CAA, M_GL_TEXTURE_ENV = 0x2300,
    M_GL_TEXTURE_ENV_MODE = 0x2200, M_GL_MODULATE = 0x2100, M_GL_TEXTURE_ENV_MODE_GET = 0x2200,
};

static bool LoadGL()
{
    if (g_glTried) return g_glReady;
    g_glTried = true;
    #define GETP(v, n) v = (decltype(v))CODVR_GetGLProc(n)
    GETP(gl_GenTextures, "glGenTextures"); GETP(gl_BindTexture, "glBindTexture");
    GETP(gl_TexImage2D, "glTexImage2D"); GETP(gl_TexParameteri, "glTexParameteri");
    GETP(gl_TexEnvi, "glTexEnvi"); GETP(gl_Enable, "glEnable"); GETP(gl_Disable, "glDisable");
    GETP(gl_IsEnabled, "glIsEnabled"); GETP(gl_BlendFunc, "glBlendFunc");
    GETP(gl_MatrixMode, "glMatrixMode"); GETP(gl_LoadIdentity, "glLoadIdentity");
    GETP(gl_Ortho, "glOrtho"); GETP(gl_Begin, "glBegin"); GETP(gl_End, "glEnd");
    GETP(gl_Color4f, "glColor4f"); GETP(gl_TexCoord2f, "glTexCoord2f"); GETP(gl_Vertex2f, "glVertex2f");
    GETP(gl_Viewport, "glViewport"); GETP(gl_GenFramebuffers, "glGenFramebuffers");
    GETP(gl_BindFramebuffer, "glBindFramebuffer"); GETP(gl_FramebufferTexture2D, "glFramebufferTexture2D");
    GETP(gl_GetIntegerv, "glGetIntegerv"); GETP(gl_GetFloatv, "glGetFloatv");
    GETP(gl_LoadMatrixf, "glLoadMatrixf"); GETP(gl_ActiveTexture, "glActiveTexture");
    GETP(gl_ColorMask, "glColorMask"); GETP(gl_ShadeModel, "glShadeModel");
    #undef GETP
    g_glReady = gl_GenTextures && gl_BindTexture && gl_TexImage2D && gl_TexParameteri
             && gl_Enable && gl_Disable && gl_BlendFunc && gl_MatrixMode && gl_LoadIdentity
             && gl_Ortho && gl_Begin && gl_End && gl_Color4f && gl_TexCoord2f && gl_Vertex2f
             && gl_Viewport && gl_GenFramebuffers && gl_BindFramebuffer
             && gl_FramebufferTexture2D && gl_GetIntegerv && gl_GetFloatv && gl_LoadMatrixf;
    return g_glReady;
}

// The atlas: ASCII 32..126 in a 16 x 6 grid of ATLAS_CELL_W x ATLAS_CELL_H
// cells on a 512 x 256 texture, alpha = the glyph's coverage.
enum { ATLAS_W = 512, ATLAS_H = 256, ATLAS_CELL_W = 32, ATLAS_CELL_H = 40, ATLAS_COLS = 16 };
static unsigned g_fontTex;
static int      g_fontAdvance = 18;   // glyph advance in atlas pixels
static bool     g_fontTried, g_fontReady;
static unsigned g_menuFbo;

static bool BuildFontAtlas()
{
    if (g_fontTried) return g_fontReady;
    g_fontTried = true;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = ATLAS_W;
    bmi.bmiHeader.biHeight = -ATLAS_H;        // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) { DeleteDC(dc); return false; }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HFONT font = CreateFontA(-30, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             FIXED_PITCH | FF_MODERN, "Consolas");
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    memset(bits, 0, ATLAS_W * ATLAS_H * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    for (int c = 32; c < 127; ++c) {
        const int i = c - 32;
        const int x = (i % ATLAS_COLS) * ATLAS_CELL_W;
        const int y = (i / ATLAS_COLS) * ATLAS_CELL_H;
        const char s[2] = { (char)c, 0 };
        TextOutA(dc, x + 2, y + 2, s, 1);
    }
    SIZE sz = { 0, 0 };
    if (GetTextExtentPoint32A(dc, "M", 1, &sz) && sz.cx > 4 && sz.cx < ATLAS_CELL_W)
        g_fontAdvance = sz.cx;
    GdiFlush();
    // alpha = luminance (GDI wrote white on black; the DIB's alpha byte is 0)
    unsigned char* px = (unsigned char*)bits;
    for (int i = 0; i < ATLAS_W * ATLAS_H; ++i) {
        const unsigned char l = px[i * 4 + 1];
        px[i * 4 + 0] = 255; px[i * 4 + 1] = 255; px[i * 4 + 2] = 255; px[i * 4 + 3] = l;
    }
    int oldTex = 0;
    if (gl_GetIntegerv) gl_GetIntegerv(M_GL_TEXTURE_BINDING_2D, &oldTex);
    gl_GenTextures(1, &g_fontTex);
    gl_BindTexture(M_GL_TEXTURE_2D, g_fontTex);
    gl_TexParameteri(M_GL_TEXTURE_2D, M_GL_TEXTURE_MIN_FILTER, M_GL_LINEAR);
    gl_TexParameteri(M_GL_TEXTURE_2D, M_GL_TEXTURE_MAG_FILTER, M_GL_LINEAR);
    gl_TexParameteri(M_GL_TEXTURE_2D, M_GL_TEXTURE_WRAP_S, M_GL_CLAMP_TO_EDGE);
    gl_TexParameteri(M_GL_TEXTURE_2D, M_GL_TEXTURE_WRAP_T, M_GL_CLAMP_TO_EDGE);
    gl_TexImage2D(M_GL_TEXTURE_2D, 0, M_GL_RGBA8, ATLAS_W, ATLAS_H, 0, M_GL_RGBA,
                  M_GL_UNSIGNED_BYTE, bits);
    gl_BindTexture(M_GL_TEXTURE_2D, (unsigned)oldTex);
    if (oldFont) SelectObject(dc, oldFont);
    if (font) DeleteObject(font);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    g_fontReady = g_fontTex != 0;
    return g_fontReady;
}

// Text in pixel space (origin bottom-left, y up), scale = pixels per atlas pixel.
static void MenuText(float x, float y, float scale, const char* s, float r, float g, float b, float a)
{
    gl_Color4f(r, g, b, a);
    gl_Begin(M_GL_QUADS);
    for (; *s; ++s) {
        int c = (unsigned char)*s;
        if (c < 32 || c > 126) c = '?';
        const int i = c - 32;
        const float u0 = (float)((i % ATLAS_COLS) * ATLAS_CELL_W) / ATLAS_W;
        const float v0 = (float)((i / ATLAS_COLS) * ATLAS_CELL_H) / ATLAS_H;
        const float u1 = u0 + (float)ATLAS_CELL_W / ATLAS_W;
        const float v1 = v0 + (float)ATLAS_CELL_H / ATLAS_H;
        const float w = ATLAS_CELL_W * scale, h = ATLAS_CELL_H * scale;
        // the atlas is top-down: v0 is the glyph's top
        gl_TexCoord2f(u0, v0); gl_Vertex2f(x, y + h);
        gl_TexCoord2f(u1, v0); gl_Vertex2f(x + w, y + h);
        gl_TexCoord2f(u1, v1); gl_Vertex2f(x + w, y);
        gl_TexCoord2f(u0, v1); gl_Vertex2f(x, y);
        x += g_fontAdvance * scale;
    }
    gl_End();
}

static void MenuRect(float x0, float y0, float x1, float y1, float r, float g, float b, float a)
{
    gl_Color4f(r, g, b, a);
    gl_Begin(M_GL_QUADS);
    gl_Vertex2f(x0, y0); gl_Vertex2f(x1, y0); gl_Vertex2f(x1, y1); gl_Vertex2f(x0, y1);
    gl_End();
}

int VRMenu_Draw(int eye)
{
    if (!g_open) return 0;
    if (!LoadGL()) return 0;
    const unsigned tex = VR_EyeTexture(eye);
    if (!tex) return 0;
    int w = 0, h = 0;
    VR_EyeTextureSizeFor(eye, &w, &h);
    if (w < 2 || h < 2) return 0;
    if (!BuildFontAtlas()) return 0;
    if (!g_menuFbo) gl_GenFramebuffers(1, &g_menuFbo);

    // --- snapshot (the vignette's discipline) ---
    int oldViewport[4] = { 0, 0, 0, 0 };
    int oldMatrixMode = M_GL_MODELVIEW, oldColorMask[4] = { 1, 1, 1, 1 };
    int oldDrawFbo = 0, oldReadFbo = 0, oldActive = M_GL_TEXTURE0, oldTex = 0;
    int oldBlendSrc = M_GL_SRC_ALPHA, oldBlendDst = M_GL_ONE_MINUS_SRC_ALPHA, oldShade = M_GL_SMOOTH;
    float oldColor[4] = { 1, 1, 1, 1 }, oldProj[16], oldModel[16];
    gl_GetIntegerv(M_GL_VIEWPORT, oldViewport);
    gl_GetIntegerv(M_GL_MATRIX_MODE, &oldMatrixMode);
    if (gl_ColorMask) gl_GetIntegerv(M_GL_COLOR_WRITEMASK, oldColorMask);
    gl_GetIntegerv(M_GL_DRAW_FRAMEBUFFER_BINDING, &oldDrawFbo);
    gl_GetIntegerv(M_GL_READ_FRAMEBUFFER_BINDING, &oldReadFbo);
    if (gl_ActiveTexture) gl_GetIntegerv(M_GL_ACTIVE_TEXTURE, &oldActive);
    gl_GetIntegerv(M_GL_BLEND_SRC, &oldBlendSrc);
    gl_GetIntegerv(M_GL_BLEND_DST, &oldBlendDst);
    gl_GetIntegerv(M_GL_SHADE_MODEL, &oldShade);
    gl_GetFloatv(M_GL_CURRENT_COLOR, oldColor);
    gl_GetFloatv(M_GL_PROJECTION_MATRIX, oldProj);
    gl_GetFloatv(M_GL_MODELVIEW_MATRIX, oldModel);
    const bool hadBlend = gl_IsEnabled && gl_IsEnabled(M_GL_BLEND) != 0;
    const bool hadDepth = gl_IsEnabled && gl_IsEnabled(M_GL_DEPTH_TEST) != 0;
    const bool hadScissor = gl_IsEnabled && gl_IsEnabled(M_GL_SCISSOR_TEST) != 0;
    const bool hadCull = gl_IsEnabled && gl_IsEnabled(M_GL_CULL_FACE) != 0;
    const bool hadLight = gl_IsEnabled && gl_IsEnabled(M_GL_LIGHTING) != 0;
    const bool hadAlpha = gl_IsEnabled && gl_IsEnabled(M_GL_ALPHA_TEST) != 0;
    const bool hadFog = gl_IsEnabled && gl_IsEnabled(M_GL_FOG) != 0;
    unsigned char hadTexUnit[2] = { 0, 0 };
    if (gl_ActiveTexture) {
        for (int u = 0; u < 2; ++u) {
            gl_ActiveTexture(M_GL_TEXTURE0 + (unsigned)u);
            hadTexUnit[u] = gl_IsEnabled ? gl_IsEnabled(M_GL_TEXTURE_2D) : 0;
            gl_Disable(M_GL_TEXTURE_2D);
        }
        gl_ActiveTexture(M_GL_TEXTURE0);
    } else {
        hadTexUnit[0] = gl_IsEnabled ? gl_IsEnabled(M_GL_TEXTURE_2D) : 0;
    }
    gl_GetIntegerv(M_GL_TEXTURE_BINDING_2D, &oldTex);

    // --- set up ---
    gl_BindFramebuffer(M_GL_FRAMEBUFFER, g_menuFbo);
    gl_FramebufferTexture2D(M_GL_FRAMEBUFFER, M_GL_COLOR_ATTACHMENT0, M_GL_TEXTURE_2D, tex, 0);
    gl_Viewport(0, 0, w, h);
    if (hadScissor) gl_Disable(M_GL_SCISSOR_TEST);
    if (hadDepth) gl_Disable(M_GL_DEPTH_TEST);
    if (hadCull) gl_Disable(M_GL_CULL_FACE);
    if (hadLight) gl_Disable(M_GL_LIGHTING);
    if (hadAlpha) gl_Disable(M_GL_ALPHA_TEST);
    if (hadFog) gl_Disable(M_GL_FOG);
    gl_Enable(M_GL_BLEND);
    gl_BlendFunc(M_GL_SRC_ALPHA, M_GL_ONE_MINUS_SRC_ALPHA);
    if (gl_ColorMask) gl_ColorMask(1, 1, 1, 1);
    if (gl_ShadeModel) gl_ShadeModel(M_GL_SMOOTH);
    gl_MatrixMode(M_GL_PROJECTION);
    gl_LoadIdentity();
    gl_Ortho(0.0, (double)w, 0.0, (double)h, -1.0, 1.0);
    gl_MatrixMode(M_GL_MODELVIEW);
    gl_LoadIdentity();

    // Layout: a panel ~55 % of the eye width, centred on this eye's optical
    // axis (the principal-point shift, so both eyes fuse it), rows of text.
    int dx = 0, dy = 0;
    VR_GetHudPrincipalShift(eye, w, h, &dx, &dy);
    const float scale = (float)h / 1800.0f;             // text ~30 px at 1800 px eyes
    const float rowH = ATLAS_CELL_H * scale * 0.95f;
    const int visibleRows = 16;
    const float panelW = w * 0.55f, panelH = rowH * (visibleRows + 4);
    const float cx = w * 0.5f + dx, cy = h * 0.5f + dy;
    const float x0 = cx - panelW * 0.5f, y1 = cy + panelH * 0.5f;
    MenuRect(x0, y1 - panelH, x0 + panelW, y1, 0.05f, 0.06f, 0.08f, 0.88f);
    MenuRect(x0, y1 - rowH * 1.4f, x0 + panelW, y1, 0.20f, 0.22f, 0.10f, 0.95f);
    gl_Enable(M_GL_TEXTURE_2D);
    gl_BindTexture(M_GL_TEXTURE_2D, g_fontTex);
    if (gl_TexEnvi) gl_TexEnvi(M_GL_TEXTURE_ENV, M_GL_TEXTURE_ENV_MODE, M_GL_MODULATE);
    float y = y1 - rowH * 1.2f;
    MenuText(x0 + rowH * 0.5f, y, scale, "CODVR  VR OPTIONS", 1.0f, 0.93f, 0.6f, 1.0f);
    y -= rowH * 1.6f;
    // Scroll so the selection stays visible; group headers take a row each.
    int first = 0;
    // Count headers as rows while scrolling, so the selected item cannot
    // disappear below the panel when several small groups share a page.
    first=g_selected;
    int usedRows=2;
    while (first>0) {
        const int cost=1+(strcmp(g_items[first-1].group,g_items[first].group)!=0);
        if (usedRows+cost>visibleRows-1) break;
        usedRows+=cost; --first;
    }
    const char* lastGroup = nullptr;
    int rows = 0;
    for (int i = first; !g_controlsHelp && i < N_ITEMS && rows < visibleRows; ++i) {
        const VRMenuItem& it = g_items[i];
        if (it.group[0] && (!lastGroup || strcmp(lastGroup, it.group) != 0)) {
            MenuText(x0 + rowH * 0.5f, y, scale * 0.9f, it.group, 0.6f, 0.8f, 1.0f, 1.0f);
            y -= rowH; ++rows; lastGroup = it.group;
            if (rows >= visibleRows) break;
        }
        char val[64];
        VRMenu_ItemValueText(i, val, sizeof(val));
        const int sel = (i == g_selected);
        if (sel) {
            gl_Disable(M_GL_TEXTURE_2D);
            MenuRect(x0 + rowH * 0.3f, y - rowH * 0.15f, x0 + panelW - rowH * 0.3f, y + rowH * 0.85f,
                     0.9f, 0.75f, 0.2f, 0.35f);
            gl_Enable(M_GL_TEXTURE_2D);
        }
        const float br = sel ? 1.0f : 0.85f;
        MenuText(x0 + rowH * 1.0f, y, scale, it.label, br, br, br, 1.0f);
        if (it.kind >= 3) {
            MenuText(x0 + panelW * 0.62f, y, scale, "[ A ]", 1.0f, 0.93f, 0.6f, 1.0f);
        } else {
            char line[80];
            snprintf(line, sizeof(line), "%s%s%s", sel ? "< " : "  ", val, sel ? " >" : "");
            MenuText(x0 + panelW * 0.62f, y, scale, line, br, 0.95f * br, 0.7f * br, 1.0f);
        }
        y -= rowH; ++rows;
    }
    if (g_controlsHelp) {
        const char* lines[] = {
            "LEFT stick: move | tap click: recenter",
            "LEFT click hold: VR options",
            "RIGHT stick: turn | click: melee",
            "RIGHT trigger: fire | grip: aim",
            "LEFT grip near fore-end: steady support",
            "A: jump | B tap: crouch | hold: prone",
            "X tap: reload | X hold: next weapon*",
            "Y: use / interact",
            "LEFT trigger hold: grenade slot",
            "LEFT menu button: pause / back",
            "Game menu: right stick cursor, trigger click",
            "* x_hold layout; other layouts change X",
            "A or B: return to settings"
        };
        for (int i=0;i<(int)(sizeof(lines)/sizeof(lines[0]));++i) {
            MenuText(x0+rowH*0.5f,y,scale*0.9f,lines[i],0.9f,0.9f,0.9f,1);
            y-=rowH;
        }
    }
    y = y1 - panelH + rowH * 0.4f;
    MenuText(x0 + rowH * 0.5f, y, scale * 0.8f,
             g_controlsHelp ? "A or B: back to settings" : "stick: move / adjust   A: change   B: save and close", 0.7f, 0.7f, 0.7f, 1.0f);
    if (g_status[0]) {
        MenuText(x0 + rowH * 0.5f, y + rowH * 0.9f, scale * 0.8f, g_status, 0.6f, 1.0f, 0.6f, 1.0f);
    }

    // --- restore, exactly ---
    gl_BindTexture(M_GL_TEXTURE_2D, (unsigned)oldTex);
    gl_MatrixMode(M_GL_PROJECTION);
    gl_LoadMatrixf(oldProj);
    gl_MatrixMode(M_GL_MODELVIEW);
    gl_LoadMatrixf(oldModel);
    gl_MatrixMode((unsigned)oldMatrixMode);
    if (!hadBlend) gl_Disable(M_GL_BLEND);
    gl_BlendFunc((unsigned)oldBlendSrc, (unsigned)oldBlendDst);
    if (hadScissor) gl_Enable(M_GL_SCISSOR_TEST);
    if (hadDepth) gl_Enable(M_GL_DEPTH_TEST);
    if (hadCull) gl_Enable(M_GL_CULL_FACE);
    if (hadLight) gl_Enable(M_GL_LIGHTING);
    if (hadAlpha) gl_Enable(M_GL_ALPHA_TEST);
    if (hadFog) gl_Enable(M_GL_FOG);
    if (gl_ActiveTexture) {
        for (int u = 0; u < 2; ++u) {
            gl_ActiveTexture(M_GL_TEXTURE0 + (unsigned)u);
            if (hadTexUnit[u]) gl_Enable(M_GL_TEXTURE_2D); else gl_Disable(M_GL_TEXTURE_2D);
        }
        gl_ActiveTexture((unsigned)oldActive);
    } else {
        if (hadTexUnit[0]) gl_Enable(M_GL_TEXTURE_2D); else gl_Disable(M_GL_TEXTURE_2D);
    }
    if (gl_ShadeModel) gl_ShadeModel((unsigned)oldShade);
    if (gl_ColorMask) gl_ColorMask((unsigned char)oldColorMask[0], (unsigned char)oldColorMask[1],
                                   (unsigned char)oldColorMask[2], (unsigned char)oldColorMask[3]);
    gl_Color4f(oldColor[0], oldColor[1], oldColor[2], oldColor[3]);
    gl_Viewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);
    gl_BindFramebuffer(0x8CA9 /* GL_DRAW_FRAMEBUFFER */, (unsigned)oldDrawFbo);
    gl_BindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, (unsigned)oldReadFbo);
    return 1;
}
