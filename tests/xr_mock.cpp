#include "xr_mock.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

MockXr g_mock;

// ------------------------------------------------------------ fake handles
//
// Constant fake pointer values; liveness is tracked in g_mock, not in the
// values. The cop checks every call against the live set, so use-after-destroy
// and wrong-handle bugs surface as violations rather than silent success.
static XrInstance MOCK_INSTANCE() { return (XrInstance)(uintptr_t)0x11110001; }
static XrSession  MOCK_SESSION()  { return (XrSession)(uintptr_t)0x22220001; }
static XrSpace    MOCK_SPACE()    { return (XrSpace)(uintptr_t)0x33330001; }
static XrSwapchain MOCK_CHAIN(int i) { return (XrSwapchain)(uintptr_t)(0x44440001 + i); }
static int ChainIndex(XrSwapchain h)
{
    uintptr_t v = (uintptr_t)h;
    if (v < 0x44440001 || v >= 0x44440001 + MOCK_MAX_CHAINS) return -1;
    return (int)(v - 0x44440001);
}

static void Violation(const char* fmt, ...)
{
    ++g_mock.violations;
    va_list ap;
    va_start(ap, fmt);
    vsprintf_s(g_mock.lastViolation, fmt, ap);
    va_end(ap);
    printf("  [mock cop] VIOLATION: %s\n", g_mock.lastViolation);
}

static bool InstanceLive(XrInstance h)
{
    return g_mock.liveInstances > 0 && h == MOCK_INSTANCE();
}
static bool SessionLive(XrSession h)
{
    return g_mock.liveSessions > 0 && h == MOCK_SESSION();
}

void MockXr_Reset()
{
    memset(&g_mock, 0, sizeof(g_mock));
    g_mock.failCreateSwapchainAt = -1;
    g_mock.failEnumImagesAt      = -1;
    g_mock.shouldRender    = true;
    g_mock.nextDisplayTime = 1000000000;   // 1 s, arbitrary epoch
    g_mock.displayPeriod   = 11111111;     // 90 Hz
    g_mock.viewFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT
                     | XR_VIEW_STATE_POSITION_VALID_BIT
                     | XR_VIEW_STATE_ORIENTATION_TRACKED_BIT
                     | XR_VIEW_STATE_POSITION_TRACKED_BIT;

    const float d2r = 0.01745329252f;
    for (int e = 0; e < MOCK_EYES; ++e) {
        g_mock.viewPose[e].orientation.w = 1.0f;
        g_mock.viewPose[e].position.x = (e == 0) ? -0.032f : 0.032f;
        g_mock.recommendW[e] = 3072;
        g_mock.recommendH[e] = 3264;
        g_mock.viewFov[e].angleUp   =  44.0f * d2r;
        g_mock.viewFov[e].angleDown = -55.0f * d2r;
    }
    // Temporal side is wider than nasal, mirrored between the eyes.
    g_mock.viewFov[0].angleLeft  = -54.0f * d2r;   // left eye: temporal on the left
    g_mock.viewFov[0].angleRight =  40.0f * d2r;
    g_mock.viewFov[1].angleLeft  = -40.0f * d2r;
    g_mock.viewFov[1].angleRight =  54.0f * d2r;   // right eye: temporal on the right

    g_mock.formats[0] = 0x8058;   // GL_RGBA8
    g_mock.formatCount = 1;
    g_mock.blendModes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    g_mock.blendModeCount = 1;

    // Action system: focused with both controllers alive, Touch profile.
    g_mock.focused = true;
    g_mock.knownProfiles[0] = "/interaction_profiles/oculus/touch_controller";
    g_mock.knownProfiles[1] = "/interaction_profiles/khr/simple_controller";
    g_mock.knownProfileCount = 2;
    strcpy_s(g_mock.activeProfile, "/interaction_profiles/oculus/touch_controller");
    for (int h = 0; h < 2; ++h) {
        g_mock.handActive[h] = true;
        g_mock.handAim[h].orientation.w = 1.0f;
        g_mock.handAim[h].position.x = (h == 0) ? -0.18f : 0.18f;
        g_mock.handAim[h].position.y = -0.25f;
        g_mock.handAim[h].position.z = -0.35f;
        g_mock.handGrip[h] = g_mock.handAim[h];
        g_mock.handGrip[h].position.z = -0.30f;
    }
}

void MockXr_QueueState(XrSessionState s)
{
    if (g_mock.eventCount < MOCK_MAX_EVENTS) {
        g_mock.events[g_mock.eventCount].kind  = MOCK_EV_STATE;
        g_mock.events[g_mock.eventCount].state = s;
        ++g_mock.eventCount;
    }
}
void MockXr_QueueRefSpaceChange()
{
    if (g_mock.eventCount < MOCK_MAX_EVENTS) {
        g_mock.events[g_mock.eventCount].kind = MOCK_EV_REFSPACE;
        ++g_mock.eventCount;
    }
}
void MockXr_QueueInstanceLoss()
{
    if (g_mock.eventCount < MOCK_MAX_EVENTS) {
        g_mock.events[g_mock.eventCount].kind = MOCK_EV_INSTANCE_LOSS;
        ++g_mock.eventCount;
    }
}

void MockXr_QueueProfileChange()
{
    if (g_mock.eventCount < MOCK_MAX_EVENTS) {
        g_mock.events[g_mock.eventCount].kind = MOCK_EV_PROFILE;
        ++g_mock.eventCount;
    }
}

int MockXr_LiveActionSpaces()
{
    int n = 0;
    for (int i = 0; i < g_mock.aspaceCount; ++i)
        if (g_mock.aspaces[i].live) ++n;
    return n;
}

int MockXr_LiveSessionObjects()
{
    int n = g_mock.liveSessions + g_mock.liveSpaces;
    for (int i = 0; i < g_mock.chainCount; ++i)
        if (g_mock.chains[i].live) ++n;
    return n;
}

// ------------------------------------------------------------- entry points

XRAPI_ATTR XrResult XRAPI_CALL xrCreateInstance(
    const XrInstanceCreateInfo* createInfo, XrInstance* instance)
{
    (void)createInfo;
    if (g_mock.failCreateInstance) return XR_ERROR_RUNTIME_FAILURE;
    if (g_mock.liveInstances > 0) Violation("second live instance created");
    ++g_mock.liveInstances;
    *instance = MOCK_INSTANCE();
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrDestroyInstance(XrInstance instance)
{
    if (!InstanceLive(instance)) { Violation("destroy of dead/wrong instance"); return XR_ERROR_HANDLE_INVALID; }
    // Destroying a handle destroys all children.
    g_mock.liveInstances = 0;
    g_mock.liveSessions = 0;
    g_mock.liveSpaces = 0;
    for (int i = 0; i < g_mock.chainCount; ++i) g_mock.chains[i].live = false;
    g_mock.sessionRunning = false;
    g_mock.frameOpen = false;
    g_mock.waitsNotBegun = 0;
    g_mock.actionSetLive = false;
    for (int i = 0; i < g_mock.actionCount; ++i) g_mock.actions[i].live = false;
    for (int i = 0; i < g_mock.aspaceCount; ++i) g_mock.aspaces[i].live = false;
    g_mock.attached = false;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProperties(
    XrInstance instance, XrInstanceProperties* instanceProperties)
{
    if (!InstanceLive(instance)) return XR_ERROR_HANDLE_INVALID;
    strcpy_s(instanceProperties->runtimeName, "MockXR");
    instanceProperties->runtimeVersion = 1;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetSystem(
    XrInstance instance, const XrSystemGetInfo* getInfo, XrSystemId* systemId)
{
    (void)getInfo;
    if (!InstanceLive(instance)) return XR_ERROR_HANDLE_INVALID;
    if (g_mock.failGetSystem) return XR_ERROR_FORM_FACTOR_UNAVAILABLE;
    *systemId = 1;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(
    XrInstance instance, const char* name, PFN_xrVoidFunction* function)
{
    (void)instance; (void)name;
    if (function) *function = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

XRAPI_ATTR XrResult XRAPI_CALL xrResultToString(
    XrInstance instance, XrResult value, char buffer[XR_MAX_RESULT_STRING_SIZE])
{
    (void)instance;
    sprintf_s(buffer, XR_MAX_RESULT_STRING_SIZE, "XR_MOCK_RESULT_%d", (int)value);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateEnvironmentBlendModes(
    XrInstance instance, XrSystemId systemId, XrViewConfigurationType viewConfigurationType,
    uint32_t environmentBlendModeCapacityInput, uint32_t* environmentBlendModeCountOutput,
    XrEnvironmentBlendMode* environmentBlendModes)
{
    (void)systemId; (void)viewConfigurationType;
    if (!InstanceLive(instance)) return XR_ERROR_HANDLE_INVALID;
    if (g_mock.failEnumBlendModes) return XR_ERROR_RUNTIME_FAILURE;
    *environmentBlendModeCountOutput = g_mock.blendModeCount;
    if (environmentBlendModeCapacityInput == 0) return XR_SUCCESS;
    if (environmentBlendModeCapacityInput < g_mock.blendModeCount)
        return XR_ERROR_SIZE_INSUFFICIENT;
    for (uint32_t i = 0; i < g_mock.blendModeCount; ++i)
        environmentBlendModes[i] = g_mock.blendModes[i];
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateSession(
    XrInstance instance, const XrSessionCreateInfo* createInfo, XrSession* session)
{
    (void)createInfo;
    if (!InstanceLive(instance)) return XR_ERROR_HANDLE_INVALID;
    if (g_mock.failCreateSession) return XR_ERROR_RUNTIME_FAILURE;
    if (g_mock.liveSessions > 0) Violation("second live session created");
    ++g_mock.liveSessions;
    g_mock.sessionRunning = false;
    *session = MOCK_SESSION();
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrDestroySession(XrSession session)
{
    if (!SessionLive(session)) { Violation("destroy of dead/wrong session"); return XR_ERROR_HANDLE_INVALID; }
    g_mock.liveSessions = 0;
    g_mock.liveSpaces = 0;      // children die with the parent
    for (int i = 0; i < g_mock.chainCount; ++i) g_mock.chains[i].live = false;
    g_mock.sessionRunning = false;
    g_mock.frameOpen = false;
    g_mock.waitsNotBegun = 0;
    // Action spaces and the attach are session children; the action set and
    // its actions belong to the instance and survive.
    for (int i = 0; i < g_mock.aspaceCount; ++i) g_mock.aspaces[i].live = false;
    g_mock.attached = false;
    return XR_SUCCESS;
}

// Project invariant, not a spec rule: CODVR must never put a game-world (or
// any non-identity) pose into a space create info -- reference transforms
// describe physical XR space only, and the 2026-08-20 -197 m investigation
// hinged on being able to PROVE the absurd origin was the runtime's, not
// ours. The cop makes any future leak a suite failure.
static bool PoseIsIdentity(const XrPosef& p)
{
    return p.position.x == 0.0f && p.position.y == 0.0f && p.position.z == 0.0f
        && p.orientation.x == 0.0f && p.orientation.y == 0.0f
        && p.orientation.z == 0.0f && p.orientation.w == 1.0f;
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateReferenceSpace(
    XrSession session, const XrReferenceSpaceCreateInfo* createInfo, XrSpace* space)
{
    if (!SessionLive(session)) return XR_ERROR_HANDLE_INVALID;
    if (createInfo && !PoseIsIdentity(createInfo->poseInReferenceSpace))
        Violation("reference space created with a non-identity pose "
                  "(%.2f %.2f %.2f) -- game units leaking into XrPosef?",
                  createInfo->poseInReferenceSpace.position.x,
                  createInfo->poseInReferenceSpace.position.y,
                  createInfo->poseInReferenceSpace.position.z);
    if (g_mock.failCreateSpace) return XR_ERROR_RUNTIME_FAILURE;
    ++g_mock.liveSpaces;
    *space = MOCK_SPACE();
    return XR_SUCCESS;
}

static XrSpace MOCK_ASPACE(int i) { return (XrSpace)(uintptr_t)(0x70700001 + i); }
static int ASpaceIndex(XrSpace h)
{
    uintptr_t v = (uintptr_t)h;
    if (v < 0x70700001 || v >= 0x70700001 + MOCK_MAX_ASPACES) return -1;
    return (int)(v - 0x70700001);
}

XRAPI_ATTR XrResult XRAPI_CALL xrDestroySpace(XrSpace space)
{
    const int ai = ASpaceIndex(space);
    if (ai >= 0) {
        if (ai >= g_mock.aspaceCount || !g_mock.aspaces[ai].live) {
            Violation("destroy of dead/wrong action space");
            return XR_ERROR_HANDLE_INVALID;
        }
        g_mock.aspaces[ai].live = false;
        return XR_SUCCESS;
    }
    if (g_mock.liveSpaces <= 0 || space != MOCK_SPACE()) {
        Violation("destroy of dead/wrong space");
        return XR_ERROR_HANDLE_INVALID;
    }
    --g_mock.liveSpaces;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateViewConfigurationViews(
    XrInstance instance, XrSystemId systemId, XrViewConfigurationType viewConfigurationType,
    uint32_t viewCapacityInput, uint32_t* viewCountOutput, XrViewConfigurationView* views)
{
    (void)systemId; (void)viewConfigurationType;
    if (!InstanceLive(instance)) return XR_ERROR_HANDLE_INVALID;
    *viewCountOutput = MOCK_EYES;
    if (viewCapacityInput == 0) return XR_SUCCESS;
    if (viewCapacityInput < MOCK_EYES) return XR_ERROR_SIZE_INSUFFICIENT;
    for (int i = 0; i < MOCK_EYES; ++i) {
        views[i].recommendedImageRectWidth  = g_mock.recommendW[i];
        views[i].recommendedImageRectHeight = g_mock.recommendH[i];
        views[i].maxImageRectWidth  = g_mock.recommendW[i] * 2;
        views[i].maxImageRectHeight = g_mock.recommendH[i] * 2;
        views[i].recommendedSwapchainSampleCount = 1;
        views[i].maxSwapchainSampleCount = 1;
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainFormats(
    XrSession session, uint32_t formatCapacityInput, uint32_t* formatCountOutput,
    int64_t* formats)
{
    if (!SessionLive(session)) return XR_ERROR_HANDLE_INVALID;
    *formatCountOutput = g_mock.formatCount;
    if (formatCapacityInput == 0) return XR_SUCCESS;
    if (formatCapacityInput < g_mock.formatCount) return XR_ERROR_SIZE_INSUFFICIENT;
    for (uint32_t i = 0; i < g_mock.formatCount; ++i) formats[i] = g_mock.formats[i];
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateSwapchain(
    XrSession session, const XrSwapchainCreateInfo* createInfo, XrSwapchain* swapchain)
{
    if (!SessionLive(session)) return XR_ERROR_HANDLE_INVALID;
    if (g_mock.chainCount >= MOCK_MAX_CHAINS) return XR_ERROR_LIMIT_REACHED;
    if (g_mock.failCreateSwapchainAt == g_mock.chainCount) return XR_ERROR_RUNTIME_FAILURE;

    MockSwapchain& c = g_mock.chains[g_mock.chainCount];
    memset(&c, 0, sizeof(c));
    c.live   = true;
    c.width  = createInfo->width;
    c.height = createInfo->height;
    c.format = createInfo->format;
    for (int k = 0; k < MOCK_IMAGES; ++k)
        c.images[k] = g_mock.createImage ? g_mock.createImage(c.width, c.height)
                                         : 0xBEEF0000u + g_mock.chainCount * 16 + k;
    *swapchain = MOCK_CHAIN(g_mock.chainCount);
    ++g_mock.chainCount;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrDestroySwapchain(XrSwapchain swapchain)
{
    int i = ChainIndex(swapchain);
    if (i < 0 || i >= g_mock.chainCount || !g_mock.chains[i].live) {
        Violation("destroy of dead/wrong swapchain");
        return XR_ERROR_HANDLE_INVALID;
    }
    g_mock.chains[i].live = false;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainImages(
    XrSwapchain swapchain, uint32_t imageCapacityInput, uint32_t* imageCountOutput,
    XrSwapchainImageBaseHeader* images)
{
    int i = ChainIndex(swapchain);
    if (i < 0 || i >= g_mock.chainCount || !g_mock.chains[i].live)
        return XR_ERROR_HANDLE_INVALID;
    *imageCountOutput = MOCK_IMAGES;
    if (imageCapacityInput == 0) return XR_SUCCESS;
    if (g_mock.failEnumImagesAt == i) return XR_ERROR_RUNTIME_FAILURE;
    if (imageCapacityInput < MOCK_IMAGES) return XR_ERROR_SIZE_INSUFFICIENT;
    XrSwapchainImageOpenGLKHR* gl = (XrSwapchainImageOpenGLKHR*)images;
    for (int k = 0; k < MOCK_IMAGES; ++k)
        gl[k].image = g_mock.chains[i].images[k];
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrAcquireSwapchainImage(
    XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* acquireInfo, uint32_t* index)
{
    (void)acquireInfo;
    int i = ChainIndex(swapchain);
    if (i < 0 || i >= g_mock.chainCount || !g_mock.chains[i].live) {
        Violation("acquire on dead/wrong swapchain");
        return XR_ERROR_HANDLE_INVALID;
    }
    if (g_mock.failAcquire) return XR_ERROR_RUNTIME_FAILURE;
    MockSwapchain& c = g_mock.chains[i];
    if (c.acquired - c.released >= MOCK_IMAGES) {
        // Not a cop violation: this is the runtime's documented answer for a
        // wedged chain (e.g. after a failed wait, which correctly must NOT be
        // "balanced" with a release). The code under test is expected to see
        // this error, count it, and escalate to a session rebuild.
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    *index = (uint32_t)(c.acquired % MOCK_IMAGES);
    ++c.acquired;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrWaitSwapchainImage(
    XrSwapchain swapchain, const XrSwapchainImageWaitInfo* waitInfo)
{
    (void)waitInfo;
    int i = ChainIndex(swapchain);
    if (i < 0 || i >= g_mock.chainCount || !g_mock.chains[i].live) {
        Violation("wait on dead/wrong swapchain");
        return XR_ERROR_HANDLE_INVALID;
    }
    MockSwapchain& c = g_mock.chains[i];
    if (c.waited >= c.acquired) {
        Violation("wait with no acquired image (chain %d)", i);
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    if (g_mock.failWaitImage) return XR_ERROR_RUNTIME_FAILURE;   // NOT waited
    ++c.waited;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrReleaseSwapchainImage(
    XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* releaseInfo)
{
    (void)releaseInfo;
    int i = ChainIndex(swapchain);
    if (i < 0 || i >= g_mock.chainCount || !g_mock.chains[i].live) {
        Violation("release on dead/wrong swapchain");
        return XR_ERROR_HANDLE_INVALID;
    }
    MockSwapchain& c = g_mock.chains[i];
    if (c.released >= c.waited) {
        // The rule from the spec: release requires a successfully waited image.
        Violation("release without a successfully waited image (chain %d)", i);
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    if (g_mock.failRelease) return XR_ERROR_RUNTIME_FAILURE;     // NOT released
    ++c.released;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrPollEvent(
    XrInstance instance, XrEventDataBuffer* eventData)
{
    if (!InstanceLive(instance)) return XR_ERROR_HANDLE_INVALID;
    if (g_mock.eventCount == 0) return XR_EVENT_UNAVAILABLE;

    MockXr::Ev ev = g_mock.events[0];
    for (int i = 1; i < g_mock.eventCount; ++i) g_mock.events[i - 1] = g_mock.events[i];
    --g_mock.eventCount;

    memset(eventData, 0, sizeof(*eventData));
    if (ev.kind == MOCK_EV_STATE) {
        XrEventDataSessionStateChanged* s = (XrEventDataSessionStateChanged*)eventData;
        s->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
        s->session = MOCK_SESSION();
        s->state = ev.state;
        s->time = g_mock.nextDisplayTime;
    } else if (ev.kind == MOCK_EV_REFSPACE) {
        XrEventDataReferenceSpaceChangePending* r =
            (XrEventDataReferenceSpaceChangePending*)eventData;
        r->type = XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING;
        r->session = MOCK_SESSION();
        r->referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        r->changeTime = g_mock.nextDisplayTime;
        r->poseValid = XR_FALSE;
    } else if (ev.kind == MOCK_EV_PROFILE) {
        XrEventDataInteractionProfileChanged* p =
            (XrEventDataInteractionProfileChanged*)eventData;
        p->type = XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED;
        p->session = MOCK_SESSION();
    } else {
        XrEventDataInstanceLossPending* l = (XrEventDataInstanceLossPending*)eventData;
        l->type = XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING;
        l->lossTime = g_mock.nextDisplayTime;
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrBeginSession(
    XrSession session, const XrSessionBeginInfo* beginInfo)
{
    (void)beginInfo;
    if (!SessionLive(session)) return XR_ERROR_HANDLE_INVALID;
    if (g_mock.sessionRunning) {
        Violation("beginSession while already running");
        return XR_ERROR_SESSION_RUNNING;
    }
    g_mock.sessionRunning = true;
    ++g_mock.beginSessionCount;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEndSession(XrSession session)
{
    if (!SessionLive(session)) return XR_ERROR_HANDLE_INVALID;
    if (!g_mock.sessionRunning) {
        Violation("endSession while not running");
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    g_mock.sessionRunning = false;
    ++g_mock.endSessionCount;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrWaitFrame(
    XrSession session, const XrFrameWaitInfo* frameWaitInfo, XrFrameState* frameState)
{
    (void)frameWaitInfo;
    if (!SessionLive(session)) return XR_ERROR_HANDLE_INVALID;
    if (!g_mock.sessionRunning) {
        Violation("waitFrame on a session that is not running");
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    if (g_mock.failWaitFrame) return XR_ERROR_RUNTIME_FAILURE;
    if (g_mock.waitsNotBegun > 0)
        Violation("second waitFrame without an intervening beginFrame");
    ++g_mock.waitsNotBegun;
    ++g_mock.waitFrameCount;
    g_mock.nextDisplayTime += g_mock.displayPeriod;
    frameState->predictedDisplayTime   = g_mock.nextDisplayTime;
    frameState->predictedDisplayPeriod = g_mock.displayPeriod;
    frameState->shouldRender = g_mock.shouldRender ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrBeginFrame(
    XrSession session, const XrFrameBeginInfo* frameBeginInfo)
{
    (void)frameBeginInfo;
    if (!SessionLive(session)) return XR_ERROR_HANDLE_INVALID;
    if (g_mock.waitsNotBegun == 0) {
        Violation("beginFrame without a prior waitFrame");
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    --g_mock.waitsNotBegun;
    if (g_mock.failBeginFrame) return XR_ERROR_RUNTIME_FAILURE;
    XrResult r = XR_SUCCESS;
    if (g_mock.frameOpen) {
        ++g_mock.discardedCount;      // previous frame silently discarded
        r = XR_FRAME_DISCARDED;
    }
    if (g_mock.forceDiscardNextBegin) {
        // Scripted: the runtime chose to discard (e.g. it fell behind). A
        // success code -- the frame is open and must still be ended.
        g_mock.forceDiscardNextBegin = false;
        ++g_mock.discardedCount;
        r = XR_FRAME_DISCARDED;
    }
    g_mock.frameOpen = true;
    ++g_mock.beginFrameCount;
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL xrLocateViews(
    XrSession session, const XrViewLocateInfo* viewLocateInfo, XrViewState* viewState,
    uint32_t viewCapacityInput, uint32_t* viewCountOutput, XrView* views)
{
    if (!SessionLive(session)) return XR_ERROR_HANDLE_INVALID;
    if (viewLocateInfo->space != MOCK_SPACE() || g_mock.liveSpaces <= 0) {
        Violation("locateViews with a dead/wrong space handle");
        return XR_ERROR_HANDLE_INVALID;
    }
    ++g_mock.locateViewsCount;
    if (g_mock.failLocateViews) return XR_ERROR_RUNTIME_FAILURE;
    *viewCountOutput = MOCK_EYES;
    if (viewCapacityInput == 0) return XR_SUCCESS;
    if (viewCapacityInput < MOCK_EYES) return XR_ERROR_SIZE_INSUFFICIENT;
    viewState->viewStateFlags = g_mock.viewFlags;
    for (int i = 0; i < MOCK_EYES; ++i) {
        views[i].pose = g_mock.viewPose[i];
        views[i].fov  = g_mock.viewFov[i];
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEndFrame(
    XrSession session, const XrFrameEndInfo* frameEndInfo)
{
    if (!SessionLive(session)) return XR_ERROR_HANDLE_INVALID;
    if (!g_mock.frameOpen) {
        Violation("endFrame without a begun frame");
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    g_mock.frameOpen = false;
    ++g_mock.endFrameCount;
    if (g_mock.failEndFrame) return XR_ERROR_RUNTIME_FAILURE;

    bool blendOk = false;
    for (uint32_t i = 0; i < g_mock.blendModeCount; ++i)
        if (g_mock.blendModes[i] == frameEndInfo->environmentBlendMode) blendOk = true;
    if (!blendOk) {
        Violation("endFrame with un-enumerated blend mode %d",
                  (int)frameEndInfo->environmentBlendMode);
        return XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED;
    }

    memset(&g_mock.lastEnd, 0, sizeof(g_mock.lastEnd));
    g_mock.lastEnd.layerCount  = frameEndInfo->layerCount;
    g_mock.lastEnd.blend       = frameEndInfo->environmentBlendMode;
    g_mock.lastEnd.displayTime = frameEndInfo->displayTime;

    if (frameEndInfo->layerCount == 0) { ++g_mock.endsEmpty; return XR_SUCCESS; }
    ++g_mock.endsWithLayers;

    for (uint32_t li = 0; li < frameEndInfo->layerCount; ++li) {
        const XrCompositionLayerBaseHeader* base = frameEndInfo->layers[li];
        if (!base) { Violation("null layer pointer"); return XR_ERROR_LAYER_INVALID; }
        if (base->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            Violation("unexpected layer type %d", (int)base->type);
            return XR_ERROR_LAYER_INVALID;
        }
        const XrCompositionLayerProjection* proj = (const XrCompositionLayerProjection*)base;
        if (proj->viewCount != MOCK_EYES || !proj->views) {
            Violation("projection layer with %u views", proj->viewCount);
            return XR_ERROR_VALIDATION_FAILURE;
        }
        g_mock.lastEnd.hasProjection = true;
        for (uint32_t v = 0; v < MOCK_EYES; ++v) {
            const XrCompositionLayerProjectionView& pv = proj->views[v];
            g_mock.lastEnd.views[v] = pv;
            if (pv.type != XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW) {
                Violation("projection view %u has type %d (zero-initialised view submitted?)",
                          v, (int)pv.type);
                return XR_ERROR_VALIDATION_FAILURE;
            }
            int ci = ChainIndex(pv.subImage.swapchain);
            if (ci < 0 || ci >= g_mock.chainCount || !g_mock.chains[ci].live) {
                Violation("projection view %u references a dead/null swapchain", v);
                return XR_ERROR_LAYER_INVALID;
            }
            const MockSwapchain& c = g_mock.chains[ci];
            if (c.released == 0)
                Violation("projection view %u submits a swapchain that never released an image", v);
            const XrRect2Di& r = pv.subImage.imageRect;
            if (r.offset.x < 0 || r.offset.y < 0 ||
                r.extent.width <= 0 || r.extent.height <= 0 ||
                r.offset.x + r.extent.width  > (int)c.width ||
                r.offset.y + r.extent.height > (int)c.height) {
                Violation("view %u imageRect %d,%d %dx%d outside %ux%u", v,
                          r.offset.x, r.offset.y, r.extent.width, r.extent.height,
                          c.width, c.height);
                return XR_ERROR_SWAPCHAIN_RECT_INVALID;
            }
        }
    }
    return XR_SUCCESS;
}

// ======================================================== action system ====
//
// Same philosophy as the frame mock above: implement the real protocol,
// enforce the real rules (attach-once, suggest-before-attach, create-before-
// attach, live handles), and expose scripted per-hand values the suite sets
// directly. Actions are identified by the NAME the code under test gives
// them, so the suite scripts semantics ("trigger_value" on the right hand)
// rather than opaque handles.

static XrActionSet MOCK_SET() { return (XrActionSet)(uintptr_t)0x55550001; }
static XrAction MOCK_ACTION(int i) { return (XrAction)(uintptr_t)(0x5A5A0001 + i); }
static int ActionIndex(XrAction h)
{
    uintptr_t v = (uintptr_t)h;
    if (v < 0x5A5A0001 || v >= 0x5A5A0001 + MOCK_MAX_ACTIONS) return -1;
    return (int)(v - 0x5A5A0001);
}
static XrPath MOCK_PATH(int i) { return (XrPath)(0x60600001u + (unsigned)i); }
static int PathIndex(XrPath p)
{
    unsigned long long v = (unsigned long long)p;
    if (v < 0x60600001ull || v >= 0x60600001ull + MOCK_MAX_PATHS) return -1;
    return (int)(v - 0x60600001ull);
}
static const char* PathText(XrPath p)
{
    const int i = PathIndex(p);
    return (i >= 0 && i < g_mock.pathCount) ? g_mock.paths[i] : "";
}
// -1 unknown, 0 left, 1 right (matches or prefixes the hand top-level paths).
static int HandFromPath(XrPath p)
{
    const char* s = PathText(p);
    if (strncmp(s, "/user/hand/left", 15) == 0)  return 0;
    if (strncmp(s, "/user/hand/right", 16) == 0) return 1;
    return -1;
}

XRAPI_ATTR XrResult XRAPI_CALL xrStringToPath(
    XrInstance instance, const char* pathString, XrPath* path)
{
    if (!InstanceLive(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!pathString || pathString[0] != '/') return XR_ERROR_PATH_FORMAT_INVALID;
    for (int i = 0; i < g_mock.pathCount; ++i) {
        if (strcmp(g_mock.paths[i], pathString) == 0) {
            *path = MOCK_PATH(i);
            return XR_SUCCESS;
        }
    }
    if (g_mock.pathCount >= MOCK_MAX_PATHS) return XR_ERROR_PATH_COUNT_EXCEEDED;
    strcpy_s(g_mock.paths[g_mock.pathCount], pathString);
    *path = MOCK_PATH(g_mock.pathCount);
    ++g_mock.pathCount;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrPathToString(
    XrInstance instance, XrPath path, uint32_t bufferCapacityInput,
    uint32_t* bufferCountOutput, char* buffer)
{
    if (!InstanceLive(instance)) return XR_ERROR_HANDLE_INVALID;
    const int i = PathIndex(path);
    if (i < 0 || i >= g_mock.pathCount) return XR_ERROR_PATH_INVALID;
    const uint32_t need = (uint32_t)strlen(g_mock.paths[i]) + 1;
    if (bufferCountOutput) *bufferCountOutput = need;
    if (bufferCapacityInput == 0) return XR_SUCCESS;
    if (bufferCapacityInput < need) return XR_ERROR_SIZE_INSUFFICIENT;
    strcpy_s(buffer, bufferCapacityInput, g_mock.paths[i]);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSet(
    XrInstance instance, const XrActionSetCreateInfo* createInfo, XrActionSet* actionSet)
{
    (void)createInfo;
    if (!InstanceLive(instance)) return XR_ERROR_HANDLE_INVALID;
    if (g_mock.failCreateActionSet) return XR_ERROR_RUNTIME_FAILURE;
    if (g_mock.actionSetLive) Violation("second live action set created");
    g_mock.actionSetLive = true;
    *actionSet = MOCK_SET();
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrDestroyActionSet(XrActionSet actionSet)
{
    if (actionSet != MOCK_SET() || !g_mock.actionSetLive) {
        Violation("destroy of dead/wrong action set");
        return XR_ERROR_HANDLE_INVALID;
    }
    g_mock.actionSetLive = false;
    for (int i = 0; i < g_mock.actionCount; ++i) g_mock.actions[i].live = false;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateAction(
    XrActionSet actionSet, const XrActionCreateInfo* createInfo, XrAction* action)
{
    if (actionSet != MOCK_SET() || !g_mock.actionSetLive)
        return XR_ERROR_HANDLE_INVALID;
    if (g_mock.attached) {
        Violation("xrCreateAction on an ATTACHED action set");
        return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    }
    if (g_mock.actionCount >= MOCK_MAX_ACTIONS) return XR_ERROR_LIMIT_REACHED;
    MockAction& a = g_mock.actions[g_mock.actionCount];
    a.live = true;
    strcpy_s(a.name, createInfo->actionName);
    a.type = createInfo->actionType;
    *action = MOCK_ACTION(g_mock.actionCount);
    ++g_mock.actionCount;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrDestroyAction(XrAction action)
{
    const int i = ActionIndex(action);
    if (i < 0 || i >= g_mock.actionCount || !g_mock.actions[i].live) {
        Violation("destroy of dead/wrong action");
        return XR_ERROR_HANDLE_INVALID;
    }
    g_mock.actions[i].live = false;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrSuggestInteractionProfileBindings(
    XrInstance instance, const XrInteractionProfileSuggestedBinding* suggestedBindings)
{
    if (!InstanceLive(instance)) return XR_ERROR_HANDLE_INVALID;
    if (g_mock.attached) {
        Violation("suggestBindings after attach");
        return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    }
    const char* prof = PathText(suggestedBindings->interactionProfile);
    bool known = false;
    for (int i = 0; i < g_mock.knownProfileCount; ++i)
        if (strcmp(prof, g_mock.knownProfiles[i]) == 0) known = true;
    if (!known) {
        // Expected for optional profiles (e.g. touch_plus on runtimes that
        // predate it) -- an error the caller must tolerate, not a violation.
        ++g_mock.suggestRejected;
        return XR_ERROR_PATH_UNSUPPORTED;
    }
    for (uint32_t k = 0; k < suggestedBindings->countSuggestedBindings; ++k) {
        const int ai = ActionIndex(suggestedBindings->suggestedBindings[k].action);
        if (ai < 0 || ai >= g_mock.actionCount || !g_mock.actions[ai].live) {
            Violation("suggested binding references a dead action");
            return XR_ERROR_HANDLE_INVALID;
        }
    }
    ++g_mock.suggestOk;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrAttachSessionActionSets(
    XrSession session, const XrSessionActionSetsAttachInfo* attachInfo)
{
    if (!SessionLive(session)) return XR_ERROR_HANDLE_INVALID;
    if (g_mock.attached) {
        Violation("second xrAttachSessionActionSets on one session");
        return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    }
    if (g_mock.failAttach) return XR_ERROR_RUNTIME_FAILURE;
    if (attachInfo->countActionSets != 1 || attachInfo->actionSets[0] != MOCK_SET()) {
        Violation("attach with wrong action set");
        return XR_ERROR_HANDLE_INVALID;
    }
    g_mock.attached = true;
    ++g_mock.attachCount;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetCurrentInteractionProfile(
    XrSession session, XrPath topLevelUserPath, XrInteractionProfileState* interactionProfile)
{
    if (!SessionLive(session)) return XR_ERROR_HANDLE_INVALID;
    if (!g_mock.attached) {
        Violation("xrGetCurrentInteractionProfile before attach");
        return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    }
    (void)topLevelUserPath;
    if (g_mock.activeProfile[0] == 0) {
        interactionProfile->interactionProfile = XR_NULL_PATH;
        return XR_SUCCESS;
    }
    XrPath p = XR_NULL_PATH;
    xrStringToPath(MOCK_INSTANCE(), g_mock.activeProfile, &p);
    interactionProfile->interactionProfile = p;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrSyncActions(
    XrSession session, const XrActionsSyncInfo* syncInfo)
{
    if (!SessionLive(session)) return XR_ERROR_HANDLE_INVALID;
    if (!g_mock.sessionRunning) {
        Violation("xrSyncActions on a session that is not running");
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    if (!g_mock.attached) {
        Violation("xrSyncActions before attach");
        return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    }
    if (syncInfo->countActiveActionSets != 1
        || syncInfo->activeActionSets[0].actionSet != MOCK_SET()) {
        Violation("sync with wrong active action set");
        return XR_ERROR_HANDLE_INVALID;
    }
    if (g_mock.failSyncActions) return XR_ERROR_RUNTIME_FAILURE;
    ++g_mock.syncCount;
    return g_mock.focused ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED;
}

// Common lookup: which action/hand is this get-state for? Returns action
// index or -1; *hand is 0/1 (a XR_NULL_PATH subaction reads the left hand,
// which is all the code under test uses it for -- the menu action).
static int LookupAction(XrAction action, XrPath sub, int* hand)
{
    const int ai = ActionIndex(action);
    *hand = HandFromPath(sub);
    if (*hand < 0) *hand = 0;
    if (ai < 0 || ai >= g_mock.actionCount || !g_mock.actions[ai].live)
        return -1;
    return ai;
}

static bool ActionsReadable()
{
    return g_mock.attached && g_mock.focused;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateFloat(
    XrSession session, const XrActionStateGetInfo* getInfo, XrActionStateFloat* state)
{
    if (!SessionLive(session)) return XR_ERROR_HANDLE_INVALID;
    int hand = 0;
    const int ai = LookupAction(getInfo->action, getInfo->subactionPath, &hand);
    if (ai < 0) return XR_ERROR_HANDLE_INVALID;
    state->isActive = (ActionsReadable() && g_mock.handActive[hand]) ? XR_TRUE : XR_FALSE;
    float v = 0.0f;
    if (state->isActive) {
        const char* n = g_mock.actions[ai].name;
        if (strcmp(n, "trigger_value") == 0) v = g_mock.trigger[hand];
        else if (strcmp(n, "squeeze_value") == 0) v = g_mock.squeeze[hand];
    }
    state->currentState = v;
    state->changedSinceLastSync = XR_FALSE;
    state->lastChangeTime = g_mock.nextDisplayTime;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateBoolean(
    XrSession session, const XrActionStateGetInfo* getInfo, XrActionStateBoolean* state)
{
    if (!SessionLive(session)) return XR_ERROR_HANDLE_INVALID;
    int hand = 0;
    const int ai = LookupAction(getInfo->action, getInfo->subactionPath, &hand);
    if (ai < 0) return XR_ERROR_HANDLE_INVALID;
    state->isActive = (ActionsReadable() && g_mock.handActive[hand]) ? XR_TRUE : XR_FALSE;
    bool v = false;
    if (state->isActive) {
        const char* n = g_mock.actions[ai].name;
        if (strcmp(n, "thumbstick_click") == 0) v = g_mock.stickClick[hand];
        else if (strcmp(n, "btn_primary") == 0)   v = hand ? g_mock.btnA : g_mock.btnX;
        else if (strcmp(n, "btn_secondary") == 0) v = hand ? g_mock.btnB : g_mock.btnY;
        else if (strcmp(n, "menu_click") == 0)    v = (hand == 0) && g_mock.menuClick;
    }
    state->currentState = v ? XR_TRUE : XR_FALSE;
    state->changedSinceLastSync = XR_FALSE;
    state->lastChangeTime = g_mock.nextDisplayTime;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateVector2f(
    XrSession session, const XrActionStateGetInfo* getInfo, XrActionStateVector2f* state)
{
    if (!SessionLive(session)) return XR_ERROR_HANDLE_INVALID;
    int hand = 0;
    const int ai = LookupAction(getInfo->action, getInfo->subactionPath, &hand);
    if (ai < 0) return XR_ERROR_HANDLE_INVALID;
    state->isActive = (ActionsReadable() && g_mock.handActive[hand]) ? XR_TRUE : XR_FALSE;
    state->currentState.x = state->isActive ? g_mock.stickX[hand] : 0.0f;
    state->currentState.y = state->isActive ? g_mock.stickY[hand] : 0.0f;
    state->changedSinceLastSync = XR_FALSE;
    state->lastChangeTime = g_mock.nextDisplayTime;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStatePose(
    XrSession session, const XrActionStateGetInfo* getInfo, XrActionStatePose* state)
{
    if (!SessionLive(session)) return XR_ERROR_HANDLE_INVALID;
    int hand = 0;
    const int ai = LookupAction(getInfo->action, getInfo->subactionPath, &hand);
    if (ai < 0) return XR_ERROR_HANDLE_INVALID;
    state->isActive = (ActionsReadable() && g_mock.handActive[hand]) ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSpace(
    XrSession session, const XrActionSpaceCreateInfo* createInfo, XrSpace* space)
{
    if (!SessionLive(session)) return XR_ERROR_HANDLE_INVALID;
    if (!PoseIsIdentity(createInfo->poseInActionSpace))
        Violation("action space created with a non-identity pose -- game "
                  "units leaking into XrPosef?");
    const int ai = ActionIndex(createInfo->action);
    if (ai < 0 || ai >= g_mock.actionCount || !g_mock.actions[ai].live)
        return XR_ERROR_HANDLE_INVALID;
    if (g_mock.aspaceCount >= MOCK_MAX_ASPACES) return XR_ERROR_LIMIT_REACHED;
    int hand = HandFromPath(createInfo->subactionPath);
    if (hand < 0) hand = 0;
    MockActionSpace& s = g_mock.aspaces[g_mock.aspaceCount];
    s.live = true;
    s.action = ai;
    s.hand = hand;
    *space = MOCK_ASPACE(g_mock.aspaceCount);
    ++g_mock.aspaceCount;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrLocateSpace(
    XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation* location)
{
    (void)time;
    const int ai = ASpaceIndex(space);
    if (ai < 0 || ai >= g_mock.aspaceCount || !g_mock.aspaces[ai].live) {
        Violation("xrLocateSpace on a dead/wrong action space");
        return XR_ERROR_HANDLE_INVALID;
    }
    if (baseSpace != MOCK_SPACE() || g_mock.liveSpaces <= 0) {
        Violation("xrLocateSpace against a dead/wrong base space");
        return XR_ERROR_HANDLE_INVALID;
    }
    const MockActionSpace& s = g_mock.aspaces[ai];
    location->locationFlags = 0;
    location->pose.orientation.w = 1.0f;
    if (g_mock.handActive[s.hand] && g_mock.focused) {
        const bool grip = strcmp(g_mock.actions[s.action].name, "grip_pose") == 0;
        location->pose = grip ? g_mock.handGrip[s.hand] : g_mock.handAim[s.hand];
        location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT
                                | XR_SPACE_LOCATION_POSITION_VALID_BIT
                                | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT
                                | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrApplyHapticFeedback(
    XrSession session, const XrHapticActionInfo* hapticActionInfo,
    const XrHapticBaseHeader* hapticFeedback)
{
    if (!SessionLive(session)) return XR_ERROR_HANDLE_INVALID;
    int hand = 0;
    const int ai = LookupAction(hapticActionInfo->action, hapticActionInfo->subactionPath, &hand);
    if (ai < 0) return XR_ERROR_HANDLE_INVALID;
    if (hapticFeedback->type == XR_TYPE_HAPTIC_VIBRATION) {
        const XrHapticVibration* v = (const XrHapticVibration*)hapticFeedback;
        if (v->amplitude < 0.0f || v->amplitude > 1.0f)
            Violation("haptic amplitude %f out of range", v->amplitude);
        ++g_mock.hapticCount[hand];
        g_mock.hapticLastAmp[hand] = v->amplitude;
        g_mock.hapticLastNs[hand] = (long long)v->duration;
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrStopHapticFeedback(
    XrSession session, const XrHapticActionInfo* hapticActionInfo)
{
    (void)hapticActionInfo;
    if (!SessionLive(session)) return XR_ERROR_HANDLE_INVALID;
    return XR_SUCCESS;
}
