// 32-bit OpenXR connectivity probe.
//
// Answers, before any of it is wired into the game: can a 32-bit process
// reach the active OpenXR runtime, does the loader negotiate with it, and is
// the OpenGL binding extension offered? A headset does not need to be awake --
// instance creation alone proves the loader/runtime path, and xrGetSystem
// failing with FORM_FACTOR_UNAVAILABLE is a *pass* for our purposes.

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

static const char* Res(XrInstance inst, XrResult r)
{
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (inst && XR_SUCCEEDED(xrResultToString(inst, r, buf)))
        return buf;
    sprintf_s(buf, "XrResult(%d)", (int)r);
    return buf;
}

int main()
{
    printf("=== OpenXR 32-bit probe ===\n");
    printf("compiled against API %u.%u.%u\n",
           XR_VERSION_MAJOR(XR_CURRENT_API_VERSION),
           XR_VERSION_MINOR(XR_CURRENT_API_VERSION),
           XR_VERSION_PATCH(XR_CURRENT_API_VERSION));

    uint32_t extCount = 0;
    XrResult r = xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    if (XR_FAILED(r)) {
        printf("FAIL: xrEnumerateInstanceExtensionProperties -> %s\n", Res(XR_NULL_HANDLE, r));
        printf("      (no runtime reachable from a 32-bit process)\n");
        return 1;
    }
    printf("runtime advertises %u extensions\n", extCount);

    XrExtensionProperties* props = new XrExtensionProperties[extCount];
    for (uint32_t i = 0; i < extCount; ++i) {
        props[i] = {};
        props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    }
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, props);

    bool haveGL = false, haveGLES = false;
    for (uint32_t i = 0; i < extCount; ++i) {
        if (!strcmp(props[i].extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME)) haveGL = true;
        if (!strcmp(props[i].extensionName, "XR_KHR_opengl_es_enable"))           haveGLES = true;
        printf("   %s (v%u)\n", props[i].extensionName, props[i].extensionVersion);
    }
    printf("\nXR_KHR_opengl_enable present: %s\n", haveGL ? "YES" : "NO  <-- blocker");

    if (!haveGL) {
        printf("Without the OpenGL binding we cannot hand CoD1's GL textures to\n"
               "the compositor directly; frames would need a GL->D3D copy.\n");
    }

    const char* exts[1] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo ici = {};
    ici.type = XR_TYPE_INSTANCE_CREATE_INFO;
    ici.enabledExtensionCount = haveGL ? 1 : 0;
    ici.enabledExtensionNames = haveGL ? exts : nullptr;
    strcpy_s(ici.applicationInfo.applicationName, "CODVR probe");
    ici.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

    XrInstance inst = XR_NULL_HANDLE;
    r = xrCreateInstance(&ici, &inst);
    if (XR_FAILED(r)) {
        printf("FAIL: xrCreateInstance -> %s\n", Res(XR_NULL_HANDLE, r));
        return 2;
    }
    printf("xrCreateInstance OK\n");

    XrInstanceProperties ip = {};
    ip.type = XR_TYPE_INSTANCE_PROPERTIES;
    if (XR_SUCCEEDED(xrGetInstanceProperties(inst, &ip)))
        printf("runtime: %s  (version %u.%u.%u)\n", ip.runtimeName,
               XR_VERSION_MAJOR(ip.runtimeVersion),
               XR_VERSION_MINOR(ip.runtimeVersion),
               XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo sgi = {};
    sgi.type = XR_TYPE_SYSTEM_GET_INFO;
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys = XR_NULL_SYSTEM_ID;
    r = xrGetSystem(inst, &sgi, &sys);
    if (XR_SUCCEEDED(r)) {
        XrSystemProperties sp = {};
        sp.type = XR_TYPE_SYSTEM_PROPERTIES;
        xrGetSystemProperties(inst, sys, &sp);
        printf("HEADSET PRESENT: %s\n", sp.systemName);
        printf("  max swapchain %ux%u, %u views\n",
               sp.graphicsProperties.maxSwapchainImageWidth,
               sp.graphicsProperties.maxSwapchainImageHeight,
               sp.graphicsProperties.maxLayerCount);

        uint32_t viewCount = 0;
        xrEnumerateViewConfigurationViews(inst, sys,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
        if (viewCount) {
            XrViewConfigurationView* vv = new XrViewConfigurationView[viewCount];
            for (uint32_t i = 0; i < viewCount; ++i) {
                vv[i] = {};
                vv[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
            }
            xrEnumerateViewConfigurationViews(inst, sys,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, vv);
            for (uint32_t i = 0; i < viewCount; ++i)
                printf("  view %u recommended %ux%u (max %ux%u), %u samples\n", i,
                       vv[i].recommendedImageRectWidth, vv[i].recommendedImageRectHeight,
                       vv[i].maxImageRectWidth, vv[i].maxImageRectHeight,
                       vv[i].recommendedSwapchainSampleCount);
            delete[] vv;
        }
    } else {
        printf("xrGetSystem -> %s\n", Res(inst, r));
        printf("  (expected when the headset is asleep; the loader/runtime\n"
               "   path is already proven by instance creation above)\n");
    }

    xrDestroyInstance(inst);
    delete[] props;
    printf("=== probe complete ===\n");
    return 0;
}

