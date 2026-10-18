// ===========================================================================
// THE VR OPTIONS OVERLAY (project-lead brief 2026-09-04, P4, sections 17-19)
//
// A CODVR-owned overlay: shows current values, takes effect immediately
// where safe, marks what needs a restart, saves persistently to codvr.cfg,
// is controller-navigable, and exposes nothing dangerous.
// ===========================================================================

enum { MENU_LOG_MAX = 64 };
static char     g_menuLogKey[MENU_LOG_MAX][64];
static char     g_menuLogText[MENU_LOG_MAX][64];
static float    g_menuLogVal[MENU_LOG_MAX];
static int      g_menuLogCount;

static void MenuTestApply(const char* key, const char* text, float val)
{
    if (g_menuLogCount >= MENU_LOG_MAX) return;
    strncpy_s(g_menuLogKey[g_menuLogCount], key ? key : "", _TRUNCATE);
    strncpy_s(g_menuLogText[g_menuLogCount], text ? text : "", _TRUNCATE);
    g_menuLogVal[g_menuLogCount] = val;
    ++g_menuLogCount;
}

static int MenuFindItem(const char* key)
{
    for (int i = 0; i < VRMenu_ItemCount(); ++i) {
        const VRMenuItem* it = VRMenu_Item(i);
        if (it && it->key && strcmp(it->key, key) == 0) return i;
    }
    return -1;
}

// Move the selection to `key` with stick-down presses alone (the controller's
// only navigation), so the test exercises the real input path.
static int MenuSelect(const char* key)
{
    const int want = MenuFindItem(key);
    if (want < 0) return 0;
    for (int guard = 0; guard <= VRMenu_ItemCount(); ++guard) {
        if (VRMenu_Selected() == want) return 1;
        VRMenu_TestFeed(0, 1, 0, 0, 0, 0, 0.0);
    }
    return 0;
}

static void MenuTempPath(char* buf, int n, const char* tag)
{
    char dir[MAX_PATH];
    const DWORD got = GetTempPathA(MAX_PATH, dir);
    if (!got || got >= MAX_PATH) strcpy_s(dir, ".\\");
    snprintf(buf, n, "%scodvr_menu_%s.cfg", dir, tag);
}

// ---------------------------------------------------------------------------
static void S_MenuModel()
{
    printf("--- VR options: the model shows every current value ---\n");
    VRMenu_Init(nullptr, &MenuTestApply);
    const int n = VRMenu_ItemCount();
    CHECK(n >= 25, "%d settings exposed", n);
    int toggles = 0, numbers = 0, enums = 0, actions = 0, restarts = 0, bad = 0, groups = 0;
    const char* lastGroup = "";
    for (int i = 0; i < n; ++i) {
        const VRMenuItem* it = VRMenu_Item(i);
        if (!it) { ++bad; continue; }
        if (!it->label || !it->label[0]) ++bad;
        switch (it->kind) {
            case 0: ++toggles; break;
            case 1: ++numbers; if (!(it->maxV > it->minV) || !(it->step > 0.0f)) ++bad; break;
            case 2: ++enums;   if (it->nEnum < 2 || !it->enumText[0] || !it->enumText[1]) ++bad; break;
            case 4: case 3: ++actions; if (it->key) ++bad; break;      // an action writes no key
            default: ++bad; break;
        }
        if (it->kind < 3 && !it->key) ++bad;
        if (it->restart) ++restarts;
        if (it->group && it->group[0] && strcmp(lastGroup, it->group) != 0) { ++groups; lastGroup = it->group; }
    }
    CHECK(bad == 0, "every item is well formed (%d toggles, %d numbers, %d enums, %d actions)",
          toggles, numbers, enums, actions);
    CHECK(groups >= 5, "%d groups: movement, hands/weapon, aim, ui, quality", groups);
    CHECK(restarts >= 1, "%d setting(s) marked restart-only", restarts);
    // Every group the brief names is present, and nothing internal is exposed.
    const char* wanted[] = { "vr_turn_mode", "vr_snap_angle", "vr_smooth_turn_speed",
                             "vr_move_direction", "vr_comfort_vignette", "vr_native_hands",
                             "vr_native_hands_r_pitch", "vr_native_hands_l_off_up",
                             "vr_gun_pitch", "vr_native_hands_twohand", "vr_twohand_grab_cm",
                             "vr_twohand_blend_s", "vr_physical_aim", "vr_ui_scale",
                             "vr_ui_safe_x", "vr_ui_safe_y", "vr_hud_scale", "supersample" };
    int missing = 0;
    for (int i = 0; i < (int)(sizeof(wanted) / sizeof(wanted[0])); ++i)
        if (MenuFindItem(wanted[i]) < 0) { ++missing; printf("      missing: %s\n", wanted[i]); }
    CHECK(missing == 0, "every setting the brief asks for is exposed");
    const char* forbidden[] = { "sweep", "autoquit", "probe", "vr_disable", "replay",
                                "renderSyscall", "cgameAdsHintRva", "vr_glsurf_route" };
    int leaked = 0;
    for (int i = 0; i < (int)(sizeof(forbidden) / sizeof(forbidden[0])); ++i)
        if (MenuFindItem(forbidden[i]) >= 0) ++leaked;
    CHECK(leaked == 0, "no probe / diagnostic / experimental key is exposed");

    // Values come from the config, exactly as loaded.
    VRMenu_NoteConfig("vr_snap_angle", "45", 45.0f);
    VRMenu_NoteConfig("vr_turn_mode", "snap", 0.0f);
    VRMenu_NoteConfig("vr_native_hands", "1", 1.0f);
    char buf[64];
    VRMenu_ItemValueText(MenuFindItem("vr_snap_angle"), buf, sizeof(buf));
    CHECK(strcmp(buf, "45") == 0, "a number shows its configured value (%s)", buf);
    VRMenu_ItemValueText(MenuFindItem("vr_turn_mode"), buf, sizeof(buf));
    CHECK(strcmp(buf, "snap") == 0, "an enum shows its configured word (%s)", buf);
    VRMenu_ItemValueText(MenuFindItem("vr_native_hands"), buf, sizeof(buf));
    CHECK(strcmp(buf, "ON") == 0, "a toggle shows ON/OFF (%s)", buf);
    VRMenu_ItemValueText(MenuFindItem("supersample"), buf, sizeof(buf));
    CHECK(strstr(buf, "restart") != nullptr, "a restart-only setting says so (%s)", buf);
    CHECK(VRMenu_Dirty() == 0, "nothing is dirty after loading the config");
}

// ---------------------------------------------------------------------------
static void S_MenuNavigateAndApply()
{
    printf("--- VR options: controller-navigable, changes take effect at once ---\n");
    VRMenu_Init(nullptr, &MenuTestApply);
    VRMenu_NoteConfig("vr_snap_angle", "30", 30.0f);
    VRMenu_NoteConfig("vr_turn_mode", "smooth", 0.0f);
    VRMenu_NoteConfig("vr_native_hands", "1", 1.0f);
    VRMenu_NoteConfig("supersample", "2.00", 2.0f);
    VRMenu_TestSetOpen(1);
    g_menuLogCount = 0;

    // Navigation: down moves, up moves back, and it wraps.
    CHECK(VRMenu_Selected() == 0, "selection starts at the top");
    VRMenu_TestFeed(0, 1, 0, 0, 0, 0, 0.0);
    CHECK(VRMenu_Selected() == 1, "stick down moves down one row");
    VRMenu_TestFeed(1, 0, 0, 0, 0, 0, 0.0);
    CHECK(VRMenu_Selected() == 0, "stick up moves back");
    VRMenu_TestFeed(1, 0, 0, 0, 0, 0, 0.0);
    CHECK(VRMenu_Selected() == VRMenu_ItemCount() - 1, "up from the top wraps to the last row");
    VRMenu_TestFeed(0, 1, 0, 0, 0, 0, 0.0);
    CHECK(VRMenu_Selected() == 0, "down from the last row wraps to the top");

    // A number: right adds one step and applies immediately.
    CHECK(MenuSelect("vr_snap_angle"), "navigated to the snap angle");
    g_menuLogCount = 0;
    VRMenu_TestFeed(0, 0, 0, 1, 0, 0, 0.0);
    char buf[64];
    VRMenu_ItemValueText(MenuFindItem("vr_snap_angle"), buf, sizeof(buf));
    CHECK(g_menuLogCount == 1 && strcmp(g_menuLogKey[0], "vr_snap_angle") == 0
          && g_menuLogVal[0] == 45.0f && strcmp(buf, "45") == 0,
          "snap angle 30 -> %s applied through the config path as %s=%.0f",
          buf, g_menuLogKey[0], g_menuLogVal[0]);
    // ...and clamps at its maximum however long it is held.
    for (int i = 0; i < 12; ++i) VRMenu_TestFeed(0, 0, 0, 1, 0, 0, 0.0);
    VRMenu_ItemValueText(MenuFindItem("vr_snap_angle"), buf, sizeof(buf));
    const VRMenuItem* snap = VRMenu_Item(MenuFindItem("vr_snap_angle"));
    CHECK(snap && snap->value == snap->maxV, "held right clamps at the maximum (%s)", buf);
    for (int i = 0; i < 30; ++i) VRMenu_TestFeed(0, 0, 1, 0, 0, 0, 0.0);
    CHECK(snap->value == snap->minV, "held left clamps at the minimum (%.0f)", snap->value);

    // A toggle: A flips it and applies.
    CHECK(MenuSelect("vr_native_hands"), "navigated to native hands");
    g_menuLogCount = 0;
    VRMenu_TestFeed(0, 0, 0, 0, 1, 0, 0.0);
    VRMenu_ItemValueText(MenuFindItem("vr_native_hands"), buf, sizeof(buf));
    CHECK(g_menuLogCount == 1 && strcmp(buf, "OFF") == 0 && g_menuLogVal[0] == 0.0f,
          "A toggles native hands ON -> %s and applies it", buf);
    VRMenu_TestFeed(0, 0, 0, 0, 1, 0, 0.0);
    VRMenu_ItemValueText(MenuFindItem("vr_native_hands"), buf, sizeof(buf));
    CHECK(strcmp(buf, "ON") == 0, "...and back ON");

    // An enum: cycles through its words, wrapping.
    CHECK(MenuSelect("vr_turn_mode"), "navigated to the turn mode");
    g_menuLogCount = 0;
    VRMenu_TestFeed(0, 0, 0, 1, 0, 0, 0.0);
    VRMenu_ItemValueText(MenuFindItem("vr_turn_mode"), buf, sizeof(buf));
    CHECK(g_menuLogCount == 1 && strcmp(buf, "snap") == 0
          && strcmp(g_menuLogText[0], "snap") == 0,
          "turn mode smooth -> %s, applied as the config word", buf);
    VRMenu_TestFeed(0, 0, 0, 1, 0, 0, 0.0);
    VRMenu_ItemValueText(MenuFindItem("vr_turn_mode"), buf, sizeof(buf));
    CHECK(strcmp(buf, "off") == 0, "turning can be disabled");
    VRMenu_TestFeed(0,0,0,1,0,0,0.0);
    VRMenu_ItemValueText(MenuFindItem("vr_turn_mode"),buf,sizeof(buf));
    CHECK(strcmp(buf,"smooth")==0,"...and wraps back to smooth");

    // A restart-only setting changes and is remembered, but is NOT applied live.
    CHECK(MenuSelect("supersample"), "navigated to supersample");
    g_menuLogCount = 0;
    VRMenu_TestFeed(0, 0, 1, 0, 0, 0, 0.0);
    VRMenu_ItemValueText(MenuFindItem("supersample"), buf, sizeof(buf));
    CHECK(g_menuLogCount == 0 && strstr(buf, "1.75") != nullptr,
          "supersample 2.00 -> %s is saved, never applied to a live session", buf);
    CHECK(VRMenu_Dirty() == 1, "unsaved changes are marked dirty");
}

// ---------------------------------------------------------------------------
static void S_MenuPersistence()
{
    printf("--- VR options: saves persistently to codvr.cfg, in place ---\n");
    char path[MAX_PATH];
    MenuTempPath(path, sizeof(path), "save");
    // A config with comments, an unrelated key, and two of ours.
    {
        FILE* f = nullptr;
        if (fopen_s(&f, path, "wb") != 0 || !f) { CHECK(0, "temp config"); return; }
        fputs("// CODVR test config\n"
              "hudAspect 1.777\n"
              "vr_snap_angle 30\n"
              "# a comment\n"
              "vr_turn_mode smooth\n"
              "supersample 2.00\n", f);
        fclose(f);
    }
    VRMenu_Init(path, &MenuTestApply);
    VRMenu_NoteConfig("vr_snap_angle", "30", 30.0f);
    VRMenu_NoteConfig("vr_turn_mode", "smooth", 0.0f);
    VRMenu_NoteConfig("supersample", "2.00", 2.0f);
    VRMenu_TestSetOpen(1);
    CHECK(MenuSelect("vr_snap_angle"), "navigated to the snap angle");
    VRMenu_TestFeed(0, 0, 0, 1, 0, 0, 0.0);        // 30 -> 45
    CHECK(MenuSelect("vr_turn_mode"), "navigated to the turn mode");
    VRMenu_TestFeed(0, 0, 0, 1, 0, 0, 0.0);        // smooth -> snap
    const unsigned saves0 = VRMenu_SaveCount();
    const int wrote = VRMenu_SaveToFile(nullptr);
    CHECK(wrote > 0 && VRMenu_SaveCount() == saves0 + 1, "%d keys written", wrote);
    CHECK(VRMenu_Dirty() == 0, "saving clears the dirty flags");

    // Read it back: our keys updated in place, everything else untouched.
    char buf[8192];
    {
        FILE* f = nullptr;
        if (fopen_s(&f, path, "rb") != 0 || !f) { CHECK(0, "reread"); return; }
        const size_t got = fread(buf, 1, sizeof(buf) - 1, f);
        buf[got] = 0;
        fclose(f);
    }
    CHECK(strstr(buf, "// CODVR test config") && strstr(buf, "# a comment"),
          "comments survive the save");
    CHECK(strstr(buf, "hudAspect 1.777") != nullptr, "unrelated keys are untouched");
    CHECK(strstr(buf, "vr_snap_angle 45") != nullptr, "the changed number is written in place");
    CHECK(strstr(buf, "vr_turn_mode snap") != nullptr, "the changed enum is written as its word");
    CHECK(strstr(buf, "vr_snap_angle 30") == nullptr && strstr(buf, "vr_turn_mode smooth") == nullptr,
          "...and the old values are gone (replaced, not appended twice)");
    CHECK(strstr(buf, "vr_ui_scale") != nullptr && strstr(buf, "VR Options saved in-game") != nullptr,
          "settings the file did not have are appended under a marker");
    // One key per setting, no duplicates.
    {
        int dupes = 0;
        for (int i = 0; i < VRMenu_ItemCount(); ++i) {
            const VRMenuItem* it = VRMenu_Item(i);
            if (!it || !it->key) continue;
            int count = 0;
            const char* p = buf;
            const size_t kl = strlen(it->key);
            while ((p = strstr(p, it->key)) != nullptr) {
                const int atLineStart = (p == buf) || p[-1] == '\n';
                if (atLineStart && (p[kl] == ' ' || p[kl] == '\t')) ++count;
                p += kl;
            }
            if (count != 1) { ++dupes; printf("      %s appears %d times\n", it->key, count); }
        }
        CHECK(dupes == 0, "every setting appears exactly once in the saved file");
    }

    // A second save is idempotent.
    const int wrote2 = VRMenu_SaveToFile(nullptr);
    char buf2[8192];
    {
        FILE* f = nullptr;
        if (fopen_s(&f, path, "rb") != 0 || !f) { CHECK(0, "reread 2"); return; }
        const size_t got = fread(buf2, 1, sizeof(buf2) - 1, f);
        buf2[got] = 0;
        fclose(f);
    }
    CHECK(wrote2 == wrote && strcmp(buf, buf2) == 0, "saving twice changes nothing further");

    // An unwritable path fails loudly instead of pretending.
    CHECK(VRMenu_SaveToFile("Z:\\no\\such\\dir\\codvr.cfg") == -1,
          "an unwritable path returns a failure, not a false success");
    DeleteFileA(path);
}

// ---------------------------------------------------------------------------
static void S_MenuOpenClose()
{
    printf("--- VR options: opened and closed by holding the LEFT stick click ---\n");
    char path[MAX_PATH];
    MenuTempPath(path, sizeof(path), "open");
    { FILE* f = nullptr; if (fopen_s(&f, path, "wb") == 0 && f) { fputs("vr_snap_angle 30\n", f); fclose(f); } }
    VRMenu_Init(path, &MenuTestApply);
    VRInputState in;
    memset(&in, 0, sizeof(in));
    in.valid = 1;
    in.hand[VRI_LEFT].active = 1;
    in.hand[VRI_RIGHT].active = 1;
    double now = 100.0;
    VRMenu_Update(&in, now);
    CHECK(VRMenu_Open() == 0, "closed by default");
    // A brief click does nothing.
    in.hand[VRI_LEFT].stickClick = 1;
    VRMenu_Update(&in, now);
    now += 0.2;
    VRMenu_Update(&in, now);
    in.hand[VRI_LEFT].stickClick = 0;
    VRMenu_Update(&in, now);
    CHECK(VRMenu_Open() == 0, "a brief stick click does not open it");
    // Held past the threshold: open.
    in.hand[VRI_LEFT].stickClick = 1;
    VRMenu_Update(&in, now);
    now += VRMENU_HOLD_S + 0.05;
    VRMenu_Update(&in, now);
    CHECK(VRMenu_Open() == 1, "held %.1f s: open", VRMENU_HOLD_S);
    // It opens once per hold, not every frame.
    now += 1.0;
    VRMenu_Update(&in, now);
    CHECK(VRMenu_Open() == 1, "still open while the click is held");
    in.hand[VRI_LEFT].stickClick = 0;
    VRMenu_Update(&in, now);
    CHECK(VRMenu_Open() == 1, "and after releasing it");

    // Navigation with the sticks, one row per push (no auto-repeat storm).
    const int sel0 = VRMenu_Selected();
    in.hand[VRI_LEFT].stickY = -1.0f;
    VRMenu_Update(&in, now);
    now += 0.02;
    VRMenu_Update(&in, now);
    CHECK(VRMenu_Selected() == sel0 + 1, "stick down moves exactly one row per push");
    in.hand[VRI_LEFT].stickY = 0.0f;
    VRMenu_Update(&in, now);

    // B saves and closes.
    in.hand[VRI_RIGHT].btnSecondary = 1;
    VRMenu_Update(&in, now);
    CHECK(VRMenu_Open() == 0 && VRMenu_SaveCount() > 0, "B saves and closes");
    in.hand[VRI_RIGHT].btnSecondary = 0;

    // No controller, no menu.
    VRMenu_Update(nullptr, now);
    CHECK(VRMenu_Open() == 0, "a null input state leaves it closed");
    DeleteFileA(path);
}
