// Mock OpenXR runtime for the safe renderer test suite.
//
// vr.cpp calls the xr* entry points as ordinary externs. The production DLL
// links them from openxr_loader.lib; this suite instead links xr_mock.cpp,
// which defines the same symbols. That gives failure injection and protocol
// checking with ZERO test seams in production code.
//
// The mock is also a SPEC COP: it enforces the OpenXR rules the real runtime
// enforces (frame call order, acquire/wait/release balance, live handles,
// valid layers) and records a violation whenever the code under test breaks
// one. Every scenario ends by asserting the violation count is still zero, so
// protocol regressions fail the suite even when no explicit CHECK covers them.
#pragma once

#include <windows.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

enum { MOCK_EYES = 2, MOCK_IMAGES = 3, MOCK_MAX_CHAINS = 8, MOCK_MAX_EVENTS = 16,
       MOCK_MAX_PATHS = 96, MOCK_MAX_ACTIONS = 48, MOCK_MAX_ASPACES = 24,
       MOCK_MAX_PROFILES = 4 };

struct MockEndFrame {
    uint32_t layerCount;
    bool     hasProjection;
    XrEnvironmentBlendMode blend;
    XrTime   displayTime;
    XrCompositionLayerProjectionView views[MOCK_EYES];  // deep copy when hasProjection
};

struct MockSwapchain {
    bool     live;
    uint32_t width, height;
    int64_t  format;
    uint32_t images[MOCK_IMAGES];   // texture ids handed to the code under test
    int      acquired, waited, released;   // cumulative; cop enforces ordering
};

// Event kinds queued for xrPollEvent.
enum { MOCK_EV_STATE = 0, MOCK_EV_REFSPACE = 1, MOCK_EV_INSTANCE_LOSS = 2,
       MOCK_EV_PROFILE = 3 };

// ---- action system (the controller-input mock) ----

struct MockAction {
    bool live;
    char name[64];
    XrActionType type;
};

struct MockActionSpace {
    bool live;
    int  action;      // index into actions[]
    int  hand;        // 0 left, 1 right (from subactionPath)
};

struct MockXr {
    // ---------------- failure switches (default: everything succeeds) ----
    bool failCreateInstance;
    bool failGetSystem;
    bool failCreateSession;
    bool failCreateSpace;
    int  failCreateSwapchainAt;   // -1 = never, else index of chain creation
    int  failEnumImagesAt;        // -1 = never, else chain index (fill call)
    bool failWaitFrame;
    bool failBeginFrame;
    bool failLocateViews;
    bool failAcquire;
    bool failWaitImage;
    bool failRelease;
    bool failEndFrame;
    bool failEnumBlendModes;
    bool forceDiscardNextBegin;   // next xrBeginFrame returns XR_FRAME_DISCARDED
                                  // (a success code) while opening normally

    // ---------------- scripted behaviour --------------------------------
    bool       shouldRender;
    XrTime     nextDisplayTime;
    XrDuration displayPeriod;         // ns; default 11111111 (90 Hz)
    XrViewStateFlags viewFlags;
    XrPosef    viewPose[MOCK_EYES];
    XrFovf     viewFov[MOCK_EYES];
    uint32_t   recommendW[MOCK_EYES], recommendH[MOCK_EYES];
    int64_t    formats[8];  uint32_t formatCount;
    XrEnvironmentBlendMode blendModes[4]; uint32_t blendModeCount;

    // Harness-provided factory for swapchain image "textures". When a real GL
    // context exists it creates real GL textures so the blit path runs live;
    // otherwise it returns fake ids and GL calls fail softly.
    uint32_t (*createImage)(uint32_t w, uint32_t h);

    // ---------------- event queue ----------------------------------------
    struct Ev { int kind; XrSessionState state; } events[MOCK_MAX_EVENTS];
    int eventCount;

    // ---------------- live-handle bookkeeping ----------------------------
    int  liveInstances, liveSessions, liveSpaces;
    MockSwapchain chains[MOCK_MAX_CHAINS];
    int  chainCount;              // creation order; slots stay dead after destroy
    bool sessionRunning;          // xrBeginSession .. xrEndSession

    // ---------------- frame protocol state --------------------------------
    int  waitsNotBegun;           // xrWaitFrame issued, xrBeginFrame pending
    bool frameOpen;               // xrBeginFrame .. xrEndFrame
    int  waitFrameCount, beginFrameCount, endFrameCount, discardedCount;
    int  locateViewsCount;        // paired rendering: one per XR frame, never
                                  // between the two eyes of a pair
    int  beginSessionCount, endSessionCount;

    // ---------------- records ---------------------------------------------
    MockEndFrame lastEnd;
    int  endsWithLayers, endsEmpty;

    // ---------------- action system ---------------------------------------
    char paths[MOCK_MAX_PATHS][96];
    int  pathCount;
    MockAction actions[MOCK_MAX_ACTIONS];
    int  actionCount;
    bool actionSetLive;
    bool attached;                       // per session; cleared on destroy
    int  attachCount, syncCount;
    bool focused;                        // xrSyncActions gate
    const char* knownProfiles[MOCK_MAX_PROFILES];
    int  knownProfileCount;
    char activeProfile[96];              // what GetCurrentInteractionProfile says
    int  suggestOk, suggestRejected;
    MockActionSpace aspaces[MOCK_MAX_ASPACES];
    int  aspaceCount;
    bool failCreateActionSet, failAttach, failSyncActions;

    // scripted controller data
    bool    handActive[2];               // 0 left, 1 right
    XrPosef handAim[2], handGrip[2];
    float   trigger[2], squeeze[2], stickX[2], stickY[2];
    bool    btnA, btnB, btnX, btnY;      // A/B right hand, X/Y left hand
    bool    stickClick[2], menuClick;

    // haptic record (per hand): submissions, last amplitude/duration
    int       hapticCount[2];
    float     hapticLastAmp[2];
    long long hapticLastNs[2];

    // ---------------- spec cop --------------------------------------------
    int  violations;
    char lastViolation[256];
};

extern MockXr g_mock;

void MockXr_Reset();                       // defaults: 90 Hz, sane poses, RGBA8, OPAQUE
void MockXr_QueueState(XrSessionState s);
void MockXr_QueueRefSpaceChange();
void MockXr_QueueInstanceLoss();
void MockXr_QueueProfileChange();
int  MockXr_LiveSessionObjects();          // sessions + spaces + live swapchains
int  MockXr_LiveActionSpaces();
