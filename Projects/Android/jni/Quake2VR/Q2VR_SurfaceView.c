/************************************************************************************

OpenXR Android entrypoint for Quake2Quest.

This replaces the old Oculus mobile runtime with the generic Khronos
OpenXR loader while keeping the JNI and Quake-facing helper surface stable.

*************************************************************************************/

#include <android/log.h>
#include <android/native_window_jni.h>
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <jni.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>
#include <GLES2/gl2ext.h>

#include "argtable3.h"
#include "VrInput.h"
#include "VrCvars.h"
#include "VrCommon.h"

#include "../quake2/src/client/header/client.h"

#define Q2XR_CHECK_XR(call) q2xr_CheckXr((call), #call, __LINE__)
#define Q2XR_SWAPCHAIN_TIMEOUT 1000000000LL
#define Q2XR_STEREO_OPENXR 8
#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void setWorldPosition(float x, float y, float z);
void setHMDPosition(float x, float y, float z, float yaw);

int CPU_LEVEL = 4;
int GPU_LEVEL = 4;
int NUM_MULTI_SAMPLES = 2;
float SS_MULTIPLIER = 1.1f;
vec2_t cylinderSize = {1280, 720};

struct arg_dbl *ss;
struct arg_int *cpu;
struct arg_int *gpu;
struct arg_end *end;

char **argv;
int argc = 0;

extern cvar_t *r_lefthand;
extern cvar_t *cl_paused;

cvar_t *vr_snapturn_angle;
cvar_t *vr_smoothturn;
cvar_t *vr_walkdirection;
cvar_t *vr_weapon_pitchadjust;
cvar_t *vr_lasersight;
cvar_t *vr_control_scheme;
cvar_t *vr_height_adjust;
cvar_t *vr_worldscale;
cvar_t *vr_weaponscale;
cvar_t *vr_weapon_stabilised;
cvar_t *vr_comfort_mask;
cvar_t *vr_turn_deadzone;
cvar_t *vr_framerate;
cvar_t *vr_use_wheels;
cvar_t *vr_jump_sound;
char **refresh_names;
float *refresh_values;

enum control_scheme {
    RIGHT_HANDED_DEFAULT = 0,
    LEFT_HANDED_DEFAULT = 10,
    LEFT_HANDED_SWITCH_STICKS = 11,
    GAMEPAD = 20
};

typedef struct {
    EGLint MajorVersion;
    EGLint MinorVersion;
    EGLDisplay Display;
    EGLConfig Config;
    EGLSurface TinySurface;
    EGLContext Context;
} q2xrEgl;

typedef struct {
    int Width;
    int Height;
    uint32_t Length;
    uint32_t Index;
    XrSwapchain Handle;
    XrSwapchainImageOpenGLESKHR *Images;
    GLuint *DepthBuffers;
    GLuint *FrameBuffers;
} q2xrFramebuffer;

typedef struct {
    JavaVM *JavaVm;
    jobject ActivityObject;
    jclass ActivityClass;
    pthread_t Thread;
    pthread_mutex_t Mutex;
    pthread_cond_t Cond;
    bool Enabled;
    bool Destroyed;
    bool Resumed;
    ANativeWindow *NativeWindow;
    char *CommandLine;
} q2xrAppThread;

typedef struct {
    q2xrEgl Egl;
    XrInstance Instance;
    XrSession Session;
    XrSystemId SystemId;
    XrSpace LocalSpace;
    XrSpace StageSpace;
    XrSpace ViewSpace;
    XrViewConfigurationView ViewConfig[NUM_EYES];
    XrView Views[NUM_EYES];
    XrFrameState FrameState;
    q2xrFramebuffer Eye[NUM_EYES];
    bool SessionRunning;
    bool Focused;
    int Width;
    int Height;
    int RefreshRate;
} q2xrApp;

static q2xrApp gApp;
static int oldtime = 0;
static int q2xrFrameLogCount = 0;
static XrPosef q2xrHeadPoseStage;
static bool q2xrWasUsingScreenLayer = false;
static XrPosef q2xrScreenLayerPose;

static XrActionSet actionSet = XR_NULL_HANDLE;
static XrAction gripPoseAction = XR_NULL_HANDLE;
static XrAction aimPoseAction = XR_NULL_HANDLE;
static XrAction hapticAction = XR_NULL_HANDLE;
static XrAction triggerAction = XR_NULL_HANDLE;
static XrAction squeezeAction = XR_NULL_HANDLE;
static XrAction thumbstickAction = XR_NULL_HANDLE;
static XrAction thumbstickClickAction = XR_NULL_HANDLE;
static XrAction thumbstickTouchAction = XR_NULL_HANDLE;
static XrAction aAction = XR_NULL_HANDLE;
static XrAction bAction = XR_NULL_HANDLE;
static XrAction xAction = XR_NULL_HANDLE;
static XrAction yAction = XR_NULL_HANDLE;
static XrAction menuAction = XR_NULL_HANDLE;
static XrSpace gripSpace[NUM_EYES] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
static XrSpace aimSpace[NUM_EYES] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
static XrPath handPath[NUM_EYES] = {XR_NULL_PATH, XR_NULL_PATH};

typedef struct {
    void *Library;
    PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC RenderbufferStorageMultisampleEXT;
    PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC FramebufferTexture2DMultisampleEXT;
} q2xrRawGles;

static q2xrRawGles rawGles;

static bool q2xr_LoadRawGles(void)
{
    if (rawGles.Library != NULL) {
        return true;
    }

    rawGles.Library = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
    if (rawGles.Library == NULL) {
        ALOGE("Unable to open libGLESv3.so: %s", dlerror());
        return false;
    }

    rawGles.RenderbufferStorageMultisampleEXT =
        (PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC)eglGetProcAddress("glRenderbufferStorageMultisampleEXT");
    rawGles.FramebufferTexture2DMultisampleEXT =
        (PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC)eglGetProcAddress("glFramebufferTexture2DMultisampleEXT");
    if (rawGles.RenderbufferStorageMultisampleEXT == NULL ||
        rawGles.FramebufferTexture2DMultisampleEXT == NULL) {
        ALOGE("GL_EXT_multisampled_render_to_texture procs are unavailable");
        return false;
    }
    return true;
}

float radians(float deg) { return (deg * M_PI) / 180.0f; }
float degrees(float rad) { return (rad * 180.0f) / M_PI; }

double GetTimeInMilliSeconds()
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec * 1e9 + now.tv_nsec) * (double)(1e-6);
}

static void q2xr_CheckXr(XrResult result, const char *call, int line)
{
    if (XR_FAILED(result)) {
        char buffer[XR_MAX_RESULT_STRING_SIZE] = {0};
        if (gApp.Instance != XR_NULL_HANDLE) {
            xrResultToString(gApp.Instance, result, buffer);
        }
        ALOGE("OpenXR error at line %d: %s -> %s (%d)", line, call, buffer, result);
    }
}

static XrPosef q2xr_IdentityPose(void)
{
    XrPosef pose;
    memset(&pose, 0, sizeof(pose));
    pose.orientation.w = 1.0f;
    return pose;
}

static XrQuaternionf q2xr_QuatFromYaw(float yawDegrees)
{
    const float halfYaw = radians(yawDegrees) * 0.5f;
    XrQuaternionf quat = {0.0f, sinf(halfYaw), 0.0f, cosf(halfYaw)};
    return quat;
}

static XrVector3f q2xr_QuatRotateVector(XrQuaternionf q, XrVector3f v)
{
    XrVector3f u = {q.x, q.y, q.z};
    XrVector3f uv = {
        u.y * v.z - u.z * v.y,
        u.z * v.x - u.x * v.z,
        u.x * v.y - u.y * v.x
    };
    XrVector3f uuv = {
        u.y * uv.z - u.z * uv.y,
        u.z * uv.x - u.x * uv.z,
        u.x * uv.y - u.y * uv.x
    };
    return (XrVector3f){
        v.x + ((uv.x * q.w) + uuv.x) * 2.0f,
        v.y + ((uv.y * q.w) + uuv.y) * 2.0f,
        v.z + ((uv.z * q.w) + uuv.z) * 2.0f
    };
}

static void q2xrEgl_Clear(q2xrEgl *egl)
{
    memset(egl, 0, sizeof(*egl));
    egl->TinySurface = EGL_NO_SURFACE;
    egl->Context = EGL_NO_CONTEXT;
}

static bool q2xrEgl_Create(q2xrEgl *egl)
{
    if (egl->Display != 0) {
        return true;
    }

    egl->Display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl->Display == EGL_NO_DISPLAY || !eglInitialize(egl->Display, &egl->MajorVersion, &egl->MinorVersion)) {
        ALOGE("eglInitialize failed");
        return false;
    }

    EGLConfig configs[1024];
    EGLint numConfigs = 0;
    eglGetConfigs(egl->Display, configs, 1024, &numConfigs);

    const EGLint configAttribs[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_STENCIL_SIZE, 0,
        EGL_SAMPLES, 0,
        EGL_NONE
    };

    for (int i = 0; i < numConfigs; ++i) {
        EGLint value = 0;
        eglGetConfigAttrib(egl->Display, configs[i], EGL_RENDERABLE_TYPE, &value);
        if ((value & EGL_OPENGL_ES3_BIT_KHR) != EGL_OPENGL_ES3_BIT_KHR) {
            continue;
        }

        eglGetConfigAttrib(egl->Display, configs[i], EGL_SURFACE_TYPE, &value);
        if ((value & (EGL_WINDOW_BIT | EGL_PBUFFER_BIT)) != (EGL_WINDOW_BIT | EGL_PBUFFER_BIT)) {
            continue;
        }

        int j = 0;
        for (; configAttribs[j] != EGL_NONE; j += 2) {
            eglGetConfigAttrib(egl->Display, configs[i], configAttribs[j], &value);
            if (value != configAttribs[j + 1]) {
                break;
            }
        }

        if (configAttribs[j] == EGL_NONE) {
            egl->Config = configs[i];
            break;
        }
    }

    if (egl->Config == 0) {
        ALOGE("No suitable EGL config found");
        return false;
    }

    const EGLint contextAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };
    egl->Context = eglCreateContext(egl->Display, egl->Config, EGL_NO_CONTEXT, contextAttribs);
    if (egl->Context == EGL_NO_CONTEXT) {
        ALOGE("eglCreateContext failed");
        return false;
    }

    const EGLint surfaceAttribs[] = {
        EGL_WIDTH, 16,
        EGL_HEIGHT, 16,
        EGL_NONE
    };
    egl->TinySurface = eglCreatePbufferSurface(egl->Display, egl->Config, surfaceAttribs);
    if (egl->TinySurface == EGL_NO_SURFACE) {
        ALOGE("eglCreatePbufferSurface failed");
        return false;
    }

    if (!eglMakeCurrent(egl->Display, egl->TinySurface, egl->TinySurface, egl->Context)) {
        ALOGE("eglMakeCurrent failed");
        return false;
    }

    return true;
}

static void q2xrEgl_Destroy(q2xrEgl *egl)
{
    if (egl->Display != 0) {
        eglMakeCurrent(egl->Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl->Context != EGL_NO_CONTEXT) {
            eglDestroyContext(egl->Display, egl->Context);
        }
        if (egl->TinySurface != EGL_NO_SURFACE) {
            eglDestroySurface(egl->Display, egl->TinySurface);
        }
        eglTerminate(egl->Display);
    }
    q2xrEgl_Clear(egl);
}

static bool q2xrFramebuffer_Create(q2xrFramebuffer *fb, int width, int height)
{
    memset(fb, 0, sizeof(*fb));
    if (!q2xr_LoadRawGles()) {
        return false;
    }

    fb->Width = width;
    fb->Height = height;

    XrSwapchainCreateInfo sci;
    memset(&sci, 0, sizeof(sci));
    sci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    sci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    sci.format = GL_SRGB8_ALPHA8;
    sci.sampleCount = 1;
    sci.width = width;
    sci.height = height;
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;
    ALOGV("Q2XR creating swapchain %dx%d samples=%u format=0x%llx", width, height, sci.sampleCount, (long long)sci.format);
    XrResult result = xrCreateSwapchain(gApp.Session, &sci, &fb->Handle);
    if (XR_FAILED(result)) {
        q2xr_CheckXr(result, "xrCreateSwapchain", __LINE__);
        return false;
    }

    result = xrEnumerateSwapchainImages(fb->Handle, 0, &fb->Length, NULL);
    if (XR_FAILED(result) || fb->Length == 0) {
        q2xr_CheckXr(result, "xrEnumerateSwapchainImages", __LINE__);
        return false;
    }
    ALOGV("Q2XR swapchain image count=%u", fb->Length);
    fb->Images = calloc(fb->Length, sizeof(*fb->Images));
    fb->DepthBuffers = calloc(fb->Length, sizeof(*fb->DepthBuffers));
    fb->FrameBuffers = calloc(fb->Length, sizeof(*fb->FrameBuffers));
    if (!fb->Images || !fb->DepthBuffers || !fb->FrameBuffers) {
        return false;
    }

    for (uint32_t i = 0; i < fb->Length; ++i) {
        fb->Images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
    }
    Q2XR_CHECK_XR(xrEnumerateSwapchainImages(
        fb->Handle, fb->Length, &fb->Length, (XrSwapchainImageBaseHeader *)fb->Images));

    for (uint32_t i = 0; i < fb->Length; ++i) {
        GLuint color = fb->Images[i].image;
        glBindTexture(GL_TEXTURE_2D, color);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);

        glGenRenderbuffers(1, &fb->DepthBuffers[i]);
        glBindRenderbuffer(GL_RENDERBUFFER, fb->DepthBuffers[i]);
        rawGles.RenderbufferStorageMultisampleEXT(GL_RENDERBUFFER, NUM_MULTI_SAMPLES, GL_DEPTH_COMPONENT24, width, height);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);

        glGenFramebuffers(1, &fb->FrameBuffers[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, fb->FrameBuffers[i]);
        rawGles.FramebufferTexture2DMultisampleEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0, NUM_MULTI_SAMPLES);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fb->DepthBuffers[i]);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            ALOGE("Incomplete OpenXR eye framebuffer: 0x%x", status);
            return false;
        }
    }

    return true;
}

static void q2xrFramebuffer_Destroy(q2xrFramebuffer *fb)
{
    if (fb->FrameBuffers) {
        glDeleteFramebuffers(fb->Length, fb->FrameBuffers);
    }
    if (fb->DepthBuffers) {
        glDeleteRenderbuffers(fb->Length, fb->DepthBuffers);
    }
    if (fb->Handle != XR_NULL_HANDLE) {
        xrDestroySwapchain(fb->Handle);
    }
    free(fb->Images);
    free(fb->DepthBuffers);
    free(fb->FrameBuffers);
    memset(fb, 0, sizeof(*fb));
}

static bool q2xrFramebuffer_Acquire(q2xrFramebuffer *fb)
{
    XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrResult result = xrAcquireSwapchainImage(fb->Handle, &acquireInfo, &fb->Index);
    if (XR_FAILED(result)) {
        q2xr_CheckXr(result, "xrAcquireSwapchainImage", __LINE__);
        return false;
    }

    XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = Q2XR_SWAPCHAIN_TIMEOUT;
    result = xrWaitSwapchainImage(fb->Handle, &waitInfo);
    if (XR_FAILED(result)) {
        q2xr_CheckXr(result, "xrWaitSwapchainImage", __LINE__);
        return false;
    }

    return true;
}

static void q2xrFramebuffer_Release(q2xrFramebuffer *fb)
{
    XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    Q2XR_CHECK_XR(xrReleaseSwapchainImage(fb->Handle, &releaseInfo));
}

static void q2xr_Path(const char *path, XrPath *xrPath)
{
    Q2XR_CHECK_XR(xrStringToPath(gApp.Instance, path, xrPath));
}

static XrActionSuggestedBinding q2xr_Binding(XrAction action, XrPath binding)
{
    XrActionSuggestedBinding result;
    result.action = action;
    result.binding = binding;
    return result;
}

static void q2xr_CreateAction(XrActionType type, const char *name, const char *label, XrAction *action)
{
    XrActionCreateInfo aci;
    memset(&aci, 0, sizeof(aci));
    aci.type = XR_TYPE_ACTION_CREATE_INFO;
    aci.actionType = type;
    aci.countSubactionPaths = NUM_EYES;
    aci.subactionPaths = handPath;
    Q_strlcpy(aci.actionName, name, sizeof(aci.actionName));
    Q_strlcpy(aci.localizedActionName, label, sizeof(aci.localizedActionName));
    Q2XR_CHECK_XR(xrCreateAction(actionSet, &aci, action));
}

static XrActionSuggestedBinding q2xr_BindingFromString(XrAction action, const char *bindingString)
{
    XrPath bindingPath = XR_NULL_PATH;
    q2xr_Path(bindingString, &bindingPath);
    return q2xr_Binding(action, bindingPath);
}

static void q2xr_SuggestTouchBindings(void)
{
    XrPath profile = XR_NULL_PATH;

    if (hmdType == XR_DEVICE_TYPE_PICO) {
        q2xr_Path("/interaction_profiles/pico/neo3_controller", &profile);
    } else {
        q2xr_Path("/interaction_profiles/oculus/touch_controller", &profile);
    }

    XrActionSuggestedBinding bindings[32];
    uint32_t n = 0;

    bindings[n++] = q2xr_BindingFromString(gripPoseAction, "/user/hand/left/input/aim/pose");
    bindings[n++] = q2xr_BindingFromString(gripPoseAction, "/user/hand/right/input/aim/pose");
    bindings[n++] = q2xr_BindingFromString(aimPoseAction, "/user/hand/left/input/aim/pose");
    bindings[n++] = q2xr_BindingFromString(aimPoseAction, "/user/hand/right/input/aim/pose");
    bindings[n++] = q2xr_BindingFromString(hapticAction, "/user/hand/left/output/haptic");
    bindings[n++] = q2xr_BindingFromString(hapticAction, "/user/hand/right/output/haptic");

    if (hmdType == XR_DEVICE_TYPE_PICO) {
        bindings[n++] = q2xr_BindingFromString(menuAction, "/user/hand/left/input/back/click");
        bindings[n++] = q2xr_BindingFromString(menuAction, "/user/hand/right/input/back/click");
        bindings[n++] = q2xr_BindingFromString(triggerAction, "/user/hand/left/input/trigger/click");
        bindings[n++] = q2xr_BindingFromString(triggerAction, "/user/hand/right/input/trigger/click");
    } else {
        bindings[n++] = q2xr_BindingFromString(menuAction, "/user/hand/left/input/menu/click");
        bindings[n++] = q2xr_BindingFromString(triggerAction, "/user/hand/left/input/trigger");
        bindings[n++] = q2xr_BindingFromString(triggerAction, "/user/hand/right/input/trigger");
    }

    bindings[n++] = q2xr_BindingFromString(squeezeAction, "/user/hand/left/input/squeeze/value");
    bindings[n++] = q2xr_BindingFromString(squeezeAction, "/user/hand/right/input/squeeze/value");
    bindings[n++] = q2xr_BindingFromString(thumbstickAction, "/user/hand/left/input/thumbstick");
    bindings[n++] = q2xr_BindingFromString(thumbstickAction, "/user/hand/right/input/thumbstick");
    bindings[n++] = q2xr_BindingFromString(thumbstickClickAction, "/user/hand/left/input/thumbstick/click");
    bindings[n++] = q2xr_BindingFromString(thumbstickClickAction, "/user/hand/right/input/thumbstick/click");

    if (hmdType != XR_DEVICE_TYPE_PICO) {
        bindings[n++] = q2xr_BindingFromString(thumbstickTouchAction, "/user/hand/left/input/thumbstick/touch");
        bindings[n++] = q2xr_BindingFromString(thumbstickTouchAction, "/user/hand/right/input/thumbstick/touch");
    }

    bindings[n++] = q2xr_BindingFromString(xAction, "/user/hand/left/input/x/click");
    bindings[n++] = q2xr_BindingFromString(yAction, "/user/hand/left/input/y/click");
    bindings[n++] = q2xr_BindingFromString(aAction, "/user/hand/right/input/a/click");
    bindings[n++] = q2xr_BindingFromString(bAction, "/user/hand/right/input/b/click");

    XrInteractionProfileSuggestedBinding suggested;
    memset(&suggested, 0, sizeof(suggested));
    suggested.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
    suggested.interactionProfile = profile;
    suggested.countSuggestedBindings = n;
    suggested.suggestedBindings = bindings;
    Q2XR_CHECK_XR(xrSuggestInteractionProfileBindings(gApp.Instance, &suggested));
}
static bool q2xr_CreateActions(void)
{
    q2xr_Path("/user/hand/left", &handPath[0]);
    q2xr_Path("/user/hand/right", &handPath[1]);

    XrActionSetCreateInfo asci;
    memset(&asci, 0, sizeof(asci));
    asci.type = XR_TYPE_ACTION_SET_CREATE_INFO;
    Q_strlcpy(asci.actionSetName, "gameplay", sizeof(asci.actionSetName));
    Q_strlcpy(asci.localizedActionSetName, "Gameplay", sizeof(asci.localizedActionSetName));
    Q2XR_CHECK_XR(xrCreateActionSet(gApp.Instance, &asci, &actionSet));

    q2xr_CreateAction(XR_ACTION_TYPE_POSE_INPUT, "grip_pose", "Grip Pose", &gripPoseAction);
    q2xr_CreateAction(XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Aim Pose", &aimPoseAction);
    q2xr_CreateAction(XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Haptic", &hapticAction);
    q2xr_CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "trigger", "Trigger", &triggerAction);
    q2xr_CreateAction(XR_ACTION_TYPE_FLOAT_INPUT, "squeeze", "Squeeze", &squeezeAction);
    q2xr_CreateAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick", "Thumbstick", &thumbstickAction);
    q2xr_CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "thumbstick_click", "Thumbstick Click", &thumbstickClickAction);
    q2xr_CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "thumbstick_touch", "Thumbstick Touch", &thumbstickTouchAction);
    q2xr_CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "a_click", "A", &aAction);
    q2xr_CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "b_click", "B", &bAction);
    q2xr_CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "x_click", "X", &xAction);
    q2xr_CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "y_click", "Y", &yAction);
    q2xr_CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "menu_click", "Menu", &menuAction);

    q2xr_SuggestTouchBindings();

    for (int hand = 0; hand < NUM_EYES; ++hand) {
        XrActionSpaceCreateInfo asciSpace;
        memset(&asciSpace, 0, sizeof(asciSpace));
        asciSpace.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
        asciSpace.poseInActionSpace = q2xr_IdentityPose();
        asciSpace.subactionPath = handPath[hand];
        asciSpace.action = gripPoseAction;
        Q2XR_CHECK_XR(xrCreateActionSpace(gApp.Session, &asciSpace, &gripSpace[hand]));
        asciSpace.action = aimPoseAction;
        Q2XR_CHECK_XR(xrCreateActionSpace(gApp.Session, &asciSpace, &aimSpace[hand]));
    }

    XrSessionActionSetsAttachInfo attachInfo;
    memset(&attachInfo, 0, sizeof(attachInfo));
    attachInfo.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &actionSet;
    Q2XR_CHECK_XR(xrAttachSessionActionSets(gApp.Session, &attachInfo));
    return true;
}

static XrActionStateBoolean q2xr_GetBoolean(XrAction action, int hand)
{
    XrActionStateGetInfo getInfo = {XR_TYPE_ACTION_STATE_GET_INFO};
    getInfo.action = action;
    getInfo.subactionPath = handPath[hand];
    XrActionStateBoolean state = {XR_TYPE_ACTION_STATE_BOOLEAN};
    Q2XR_CHECK_XR(xrGetActionStateBoolean(gApp.Session, &getInfo, &state));
    return state;
}

static XrActionStateFloat q2xr_GetFloat(XrAction action, int hand)
{
    XrActionStateGetInfo getInfo = {XR_TYPE_ACTION_STATE_GET_INFO};
    getInfo.action = action;
    getInfo.subactionPath = handPath[hand];
    XrActionStateFloat state = {XR_TYPE_ACTION_STATE_FLOAT};
    Q2XR_CHECK_XR(xrGetActionStateFloat(gApp.Session, &getInfo, &state));
    return state;
}

static XrActionStateVector2f q2xr_GetVector2(XrAction action, int hand)
{
    XrActionStateGetInfo getInfo = {XR_TYPE_ACTION_STATE_GET_INFO};
    getInfo.action = action;
    getInfo.subactionPath = handPath[hand];
    XrActionStateVector2f state = {XR_TYPE_ACTION_STATE_VECTOR2F};
    Q2XR_CHECK_XR(xrGetActionStateVector2f(gApp.Session, &getInfo, &state));
    return state;
}

void TBXR_UpdateControllers(void)
{
    if (gApp.Session == XR_NULL_HANDLE || !gApp.Focused || actionSet == XR_NULL_HANDLE) {
        return;
    }

    XrActiveActionSet activeSet;
    memset(&activeSet, 0, sizeof(activeSet));
    activeSet.actionSet = actionSet;
    XrActionsSyncInfo syncInfo;
    memset(&syncInfo, 0, sizeof(syncInfo));
    syncInfo.type = XR_TYPE_ACTIONS_SYNC_INFO;
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeSet;
    XrResult syncResult = xrSyncActions(gApp.Session, &syncInfo);
    if (XR_FAILED(syncResult)) {
        q2xr_CheckXr(syncResult, "xrSyncActions", __LINE__);
        return;
    }

    ovrInputStateTrackedRemote *states[NUM_EYES] = {&leftTrackedRemoteState_new, &rightTrackedRemoteState_new};
    ovrTracking *tracking[NUM_EYES] = {&leftRemoteTracking_new, &rightRemoteTracking_new};

    for (int hand = 0; hand < NUM_EYES; ++hand) {
        memset(states[hand], 0, sizeof(*states[hand]));
        XrActionStateBoolean trigger = q2xr_GetBoolean(triggerAction, hand);
        XrActionStateFloat squeeze = q2xr_GetFloat(squeezeAction, hand);
        XrActionStateVector2f stick = q2xr_GetVector2(thumbstickAction, hand);
        states[hand]->IndexTrigger = trigger.currentState ? 1.0f : 0.0f;
        states[hand]->GripTrigger = squeeze.currentState;
        float joyX = stick.currentState.x;
        float joyY = stick.currentState.y;

        float joyLen = sqrtf((joyX * joyX) + (joyY * joyY));

        if (joyLen > 1.0f) {
            joyX /= joyLen;
            joyY /= joyLen;
        }

        states[hand]->Joystick.x = joyX;
        states[hand]->Joystick.y = joyY;

        if (trigger.currentState) states[hand]->Buttons |= ovrButton_Trigger;
        if (squeeze.currentState > 0.5f) states[hand]->Buttons |= ovrButton_GripTrigger;
        if (q2xr_GetBoolean(thumbstickClickAction, hand).currentState) states[hand]->Buttons |= ovrButton_Joystick | (hand == 0 ? ovrButton_LThumb : ovrButton_RThumb);
        if (q2xr_GetBoolean(thumbstickTouchAction, hand).currentState) states[hand]->Touches |= ovrTouch_ThumbRest;
        if (q2xr_GetBoolean(menuAction, hand).currentState) states[hand]->Buttons |= ovrButton_Enter;
        if (hand == 0) {
            if (q2xr_GetBoolean(xAction, hand).currentState) states[hand]->Buttons |= ovrButton_X;
            if (q2xr_GetBoolean(yAction, hand).currentState) states[hand]->Buttons |= ovrButton_Y;
        } else {
            if (q2xr_GetBoolean(aAction, hand).currentState) states[hand]->Buttons |= ovrButton_A;
            if (q2xr_GetBoolean(bAction, hand).currentState) states[hand]->Buttons |= ovrButton_B;
        }

        XrSpaceLocation aimLocation = {XR_TYPE_SPACE_LOCATION};
        XrSpaceVelocity velocity = {XR_TYPE_SPACE_VELOCITY};
        aimLocation.next = &velocity;
        Q2XR_CHECK_XR(xrLocateSpace(aimSpace[hand], gApp.StageSpace, gApp.FrameState.predictedDisplayTime, &aimLocation));
        tracking[hand]->Active = (aimLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
        tracking[hand]->HeadPose.Pose.Position = aimLocation.pose.position;
        tracking[hand]->HeadPose.Pose.Orientation = aimLocation.pose.orientation;
        tracking[hand]->Velocity = velocity;
    }
}

float vibration_channel_duration[2] = {0.0f, 0.0f};
float vibration_channel_intensity[2] = {0.0f, 0.0f};

void Android_Vibrate(float duration, int channel, float intensity)
{
    if (channel < 0 || channel >= 2) {
        return;
    }
    if (vibration_channel_duration[channel] > 0.0f) {
        return;
    }
    if (vibration_channel_duration[channel] == -1.0f && duration != 0.0f) {
        return;
    }
    vibration_channel_duration[channel] = duration;
    vibration_channel_intensity[channel] = intensity;
}

static void q2xr_ProcessHaptics(float frameTimeMs)
{
    if (gApp.Session == XR_NULL_HANDLE || hapticAction == XR_NULL_HANDLE) {
        return;
    }

    for (int hand = 0; hand < 2; ++hand) {
        if (vibration_channel_duration[hand] > 0.0f || vibration_channel_duration[hand] == -1.0f) {
            XrHapticVibration vibration = {XR_TYPE_HAPTIC_VIBRATION};
            vibration.amplitude = vibration_channel_intensity[hand];
            vibration.duration = XR_MIN_HAPTIC_DURATION;
            vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
            XrHapticActionInfo info = {XR_TYPE_HAPTIC_ACTION_INFO};
            info.action = hapticAction;
            info.subactionPath = handPath[hand];
            xrApplyHapticFeedback(gApp.Session, &info, (const XrHapticBaseHeader *)&vibration);

            if (vibration_channel_duration[hand] != -1.0f) {
                vibration_channel_duration[hand] -= frameTimeMs;
                if (vibration_channel_duration[hand] < 0.0f) {
                    vibration_channel_duration[hand] = 0.0f;
                    vibration_channel_intensity[hand] = 0.0f;
                }
            }
        }
    }
}

#ifndef XR_PICO_CONTROLLER_INTERACTION_EXTENSION_NAME
#define XR_PICO_CONTROLLER_INTERACTION_EXTENSION_NAME "XR_BD_controller_interaction"
#endif

static bool q2xr_ExtensionSupported(const XrExtensionProperties *props, uint32_t count, const char *name)
{
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(props[i].extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

static bool q2xr_InitOpenXR(q2xrAppThread *thread)
{
    PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = NULL;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&xrInitializeLoaderKHR);
    if (xrInitializeLoaderKHR) {
        XrLoaderInitInfoAndroidKHR loaderInitInfo = {XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        loaderInitInfo.applicationVM = thread->JavaVm;
        loaderInitInfo.applicationContext = thread->ActivityObject;
        xrInitializeLoaderKHR((XrLoaderInitInfoBaseHeaderKHR *)&loaderInitInfo);
    }

    /* Enumerate the runtime's available extensions so we only request what it
     * actually supports. The Pico controller extension (XR_BD_controller_interaction)
     * is absent on the Meta runtime, and requesting an unsupported extension makes
     * xrCreateInstance fail outright (XR_ERROR_EXTENSION_NOT_PRESENT), which would
     * leave the app hanging on the loading screen. */
    uint32_t availableCount = 0;
    xrEnumerateInstanceExtensionProperties(NULL, 0, &availableCount, NULL);

    XrExtensionProperties *available = NULL;
    if (availableCount > 0) {
        available = (XrExtensionProperties *)malloc(availableCount * sizeof(XrExtensionProperties));
    }
    if (available) {
        for (uint32_t i = 0; i < availableCount; i++) {
            memset(&available[i], 0, sizeof(available[i]));
            available[i].type = XR_TYPE_EXTENSION_PROPERTIES;
        }
        if (XR_FAILED(xrEnumerateInstanceExtensionProperties(NULL, availableCount, &availableCount, available))) {
            availableCount = 0;
        }
    } else {
        availableCount = 0;
    }

    const char *extensions[8];
    uint32_t extensionCount = 0;
    /* Required. */
    extensions[extensionCount++] = XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME;
    extensions[extensionCount++] = XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME;
    /* Optional: enable Pico controller bindings only on a runtime that has them. */
    if (q2xr_ExtensionSupported(available, availableCount, XR_PICO_CONTROLLER_INTERACTION_EXTENSION_NAME)) {
        extensions[extensionCount++] = XR_PICO_CONTROLLER_INTERACTION_EXTENSION_NAME;
        ALOGV("Q2XR enabling %s", XR_PICO_CONTROLLER_INTERACTION_EXTENSION_NAME);
    }
    /* Optional: lets us lock the display to 72Hz where the runtime supports it. */
    if (q2xr_ExtensionSupported(available, availableCount, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) {
        extensions[extensionCount++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
        ALOGV("Q2XR enabling %s", XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    }

    free(available);

    XrApplicationInfo appInfo;
    memset(&appInfo, 0, sizeof(appInfo));
    Q_strlcpy(appInfo.applicationName, "Quake2Quest", sizeof(appInfo.applicationName));
    appInfo.applicationVersion = 20;
    Q_strlcpy(appInfo.engineName, "Yamagi Quake II", sizeof(appInfo.engineName));
    appInfo.engineVersion = 1;
    appInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);

    XrInstanceCreateInfoAndroidKHR androidInfo = {XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    androidInfo.applicationVM = thread->JavaVm;
    androidInfo.applicationActivity = thread->ActivityObject;

    XrInstanceCreateInfo createInfo;
    memset(&createInfo, 0, sizeof(createInfo));
    createInfo.type = XR_TYPE_INSTANCE_CREATE_INFO;
    createInfo.next = &androidInfo;
    createInfo.applicationInfo = appInfo;
    createInfo.enabledExtensionCount = extensionCount;
    createInfo.enabledExtensionNames = extensions;

    XrResult result = xrCreateInstance(&createInfo, &gApp.Instance);
    if (XR_FAILED(result)) {
        q2xr_CheckXr(result, "xrCreateInstance", __LINE__);
        return false;
    }
    ALOGV("Q2XR xrCreateInstance succeeded");

    XrInstanceProperties props = {XR_TYPE_INSTANCE_PROPERTIES};
    Q2XR_CHECK_XR(xrGetInstanceProperties(gApp.Instance, &props));
    ALOGV("OpenXR runtime: %s", props.runtimeName);
    if (strstr(props.runtimeName, "Pico") || strstr(props.runtimeName, "PICO")) {
        hmdType = XR_DEVICE_TYPE_PICO;
    } else {
        hmdType = XR_DEVICE_TYPE_META;
    }

    XrSystemGetInfo systemInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    result = xrGetSystem(gApp.Instance, &systemInfo, &gApp.SystemId);
    if (XR_FAILED(result)) {
        q2xr_CheckXr(result, "xrGetSystem", __LINE__);
        return false;
    }
    ALOGV("Q2XR xrGetSystem succeeded systemId=%llu", (unsigned long long)gApp.SystemId);

    PFN_xrGetOpenGLESGraphicsRequirementsKHR getGlesRequirements = NULL;
    Q2XR_CHECK_XR(xrGetInstanceProcAddr(gApp.Instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&getGlesRequirements));
    if (getGlesRequirements) {
        XrGraphicsRequirementsOpenGLESKHR requirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
        Q2XR_CHECK_XR(getGlesRequirements(gApp.Instance, gApp.SystemId, &requirements));
    }

    uint32_t viewCount = 0;
    Q2XR_CHECK_XR(xrEnumerateViewConfigurationViews(gApp.Instance, gApp.SystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, NULL));
    if (viewCount != NUM_EYES) {
        ALOGE("Expected stereo OpenXR views, got %u", viewCount);
        return false;
    }
    for (int i = 0; i < NUM_EYES; ++i) {
        gApp.ViewConfig[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
        gApp.Views[i].type = XR_TYPE_VIEW;
    }
    Q2XR_CHECK_XR(xrEnumerateViewConfigurationViews(gApp.Instance, gApp.SystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, NUM_EYES, &viewCount, gApp.ViewConfig));

    gApp.Width = (int)(gApp.ViewConfig[0].recommendedImageRectWidth * SS_MULTIPLIER);
    gApp.Height = (int)(gApp.ViewConfig[0].recommendedImageRectHeight * SS_MULTIPLIER);
    gApp.RefreshRate = 72; /* fixed 72Hz - higher rates can stutter on this engine */
    ALOGV("Q2XR view size recommended=%ux%u using=%dx%d",
          gApp.ViewConfig[0].recommendedImageRectWidth,
          gApp.ViewConfig[0].recommendedImageRectHeight,
          gApp.Width,
          gApp.Height);

    XrGraphicsBindingOpenGLESAndroidKHR graphicsBinding = {XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    graphicsBinding.display = gApp.Egl.Display;
    graphicsBinding.config = gApp.Egl.Config;
    graphicsBinding.context = gApp.Egl.Context;

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &graphicsBinding;
    sessionInfo.systemId = gApp.SystemId;
    result = xrCreateSession(gApp.Instance, &sessionInfo, &gApp.Session);
    if (XR_FAILED(result)) {
        q2xr_CheckXr(result, "xrCreateSession", __LINE__);
        return false;
    }
    ALOGV("Q2XR xrCreateSession succeeded");

    /* Lock the display to 72Hz. The engine's frame timing assumes this (gApp.RefreshRate),
     * and higher refresh rates can stutter on this engine, so it is not user-selectable.
     * Best-effort: the proc only resolves if XR_FB_display_refresh_rate was enabled above;
     * otherwise we inherit the runtime default (72Hz on Quest). */
    {
        PFN_xrRequestDisplayRefreshRateFB pfnRequestDisplayRefreshRate = NULL;
        if (XR_SUCCEEDED(xrGetInstanceProcAddr(gApp.Instance, "xrRequestDisplayRefreshRateFB",
                (PFN_xrVoidFunction *)&pfnRequestDisplayRefreshRate)) && pfnRequestDisplayRefreshRate) {
            XrResult rr = pfnRequestDisplayRefreshRate(gApp.Session, 72.0f);
            ALOGV("Q2XR request 72Hz display refresh: %d", rr);
        }
    }

    XrReferenceSpaceCreateInfo spaceInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.poseInReferenceSpace = q2xr_IdentityPose();
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    Q2XR_CHECK_XR(xrCreateReferenceSpace(gApp.Session, &spaceInfo, &gApp.LocalSpace));
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    result = xrCreateReferenceSpace(gApp.Session, &spaceInfo, &gApp.StageSpace);
    if (XR_FAILED(result)) {
        gApp.StageSpace = gApp.LocalSpace;
    }
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    Q2XR_CHECK_XR(xrCreateReferenceSpace(gApp.Session, &spaceInfo, &gApp.ViewSpace));

    for (int eye = 0; eye < NUM_EYES; ++eye) {
        ALOGV("Q2XR creating framebuffer for eye %d", eye);
        if (!q2xrFramebuffer_Create(&gApp.Eye[eye], gApp.Width, gApp.Height)) {
            return false;
        }
    }

    q2xr_CreateActions();
    ALOGV("Q2XR action setup completed");
    return true;
}

static void q2xr_DestroyOpenXR(void)
{
    for (int hand = 0; hand < NUM_EYES; ++hand) {
        if (gripSpace[hand]) xrDestroySpace(gripSpace[hand]);
        if (aimSpace[hand]) xrDestroySpace(aimSpace[hand]);
        gripSpace[hand] = XR_NULL_HANDLE;
        aimSpace[hand] = XR_NULL_HANDLE;
    }
    if (actionSet) {
        xrDestroyActionSet(actionSet);
        actionSet = XR_NULL_HANDLE;
    }
    for (int eye = 0; eye < NUM_EYES; ++eye) {
        q2xrFramebuffer_Destroy(&gApp.Eye[eye]);
    }
    if (gApp.ViewSpace && gApp.ViewSpace != gApp.LocalSpace) xrDestroySpace(gApp.ViewSpace);
    if (gApp.StageSpace && gApp.StageSpace != gApp.LocalSpace) xrDestroySpace(gApp.StageSpace);
    if (gApp.LocalSpace) xrDestroySpace(gApp.LocalSpace);
    if (gApp.Session) xrDestroySession(gApp.Session);
    if (gApp.Instance) xrDestroyInstance(gApp.Instance);
    memset(&gApp, 0, sizeof(gApp));
}

static void q2xr_ProcessEvents(void)
{
    XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(gApp.Instance, &event) == XR_SUCCESS) {
        switch (event.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                XrEventDataSessionStateChanged *changed = (XrEventDataSessionStateChanged *)&event;
                if (changed->state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo beginInfo = {XR_TYPE_SESSION_BEGIN_INFO};
                    beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    Q2XR_CHECK_XR(xrBeginSession(gApp.Session, &beginInfo));
                    gApp.SessionRunning = true;
                } else if (changed->state == XR_SESSION_STATE_STOPPING) {
                    Q2XR_CHECK_XR(xrEndSession(gApp.Session));
                    gApp.SessionRunning = false;
                } else if (changed->state == XR_SESSION_STATE_FOCUSED) {
                    gApp.Focused = true;
                } else if (changed->state == XR_SESSION_STATE_VISIBLE) {
                    gApp.Focused = false;
                } else if (changed->state == XR_SESSION_STATE_EXITING || changed->state == XR_SESSION_STATE_LOSS_PENDING) {
                    gApp.SessionRunning = false;
                }
                break;
            }
            default:
                break;
        }
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
        event.next = NULL;
    }
}

static void q2xr_UpdateHeadPose(void)
{
    XrSpaceLocation location = {XR_TYPE_SPACE_LOCATION};
    if (XR_SUCCEEDED(xrLocateSpace(gApp.ViewSpace, gApp.StageSpace, gApp.FrameState.predictedDisplayTime, &location))) {
        q2xrHeadPoseStage = location.pose;
        vec3_t orientation;
        QuatToYawPitchRoll(location.pose.orientation, 0.0f, orientation);
        VectorCopy(orientation, hmdorientation);
        setHMDPosition(-location.pose.position.x, location.pose.position.y, -location.pose.position.z, orientation[YAW]);
        setWorldPosition(-location.pose.position.x, location.pose.position.y, -location.pose.position.z);
    }
}

static void q2xr_RenderFrame(void)
{
    int time;
    do {
        global_time = Sys_Milliseconds();
        time = (int)(global_time - oldtime);
    } while (time < 1);

    q2xr_ProcessEvents();
    if (!gApp.SessionRunning) {
        oldtime = global_time;
        return;
    }
    if (q2xrFrameLogCount < 6) {
        ALOGV("Q2XR frame %d begin time=%d", q2xrFrameLogCount, time);
    }

    XrFrameWaitInfo waitInfo = {XR_TYPE_FRAME_WAIT_INFO};
    memset(&gApp.FrameState, 0, sizeof(gApp.FrameState));
    gApp.FrameState.type = XR_TYPE_FRAME_STATE;
    Q2XR_CHECK_XR(xrWaitFrame(gApp.Session, &waitInfo, &gApp.FrameState));

    XrFrameBeginInfo beginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
    Q2XR_CHECK_XR(xrBeginFrame(gApp.Session, &beginInfo));
    if (q2xrFrameLogCount < 6) {
        ALOGV("Q2XR frame %d xrBeginFrame shouldRender=%d", q2xrFrameLogCount, gApp.FrameState.shouldRender);
    }

    q2xr_UpdateHeadPose();
    q2xr_ProcessHaptics((float)time);

    if (vr_control_scheme != NULL) {
        acquireTrackedRemotesData();
        switch ((int)vr_control_scheme->value) {
            case RIGHT_HANDED_DEFAULT:
                HandleInput_Default(&rightTrackedRemoteState_new, &rightTrackedRemoteState_old, &rightRemoteTracking_new,
                                    &leftTrackedRemoteState_new, &leftTrackedRemoteState_old, &leftRemoteTracking_new,
                                    ovrButton_A, ovrButton_B, ovrButton_X, ovrButton_Y);
                break;
            case LEFT_HANDED_DEFAULT:
            case LEFT_HANDED_SWITCH_STICKS:
                HandleInput_Default(&leftTrackedRemoteState_new, &leftTrackedRemoteState_old, &leftRemoteTracking_new,
                                    &rightTrackedRemoteState_new, &rightTrackedRemoteState_old, &rightRemoteTracking_new,
                                    ovrButton_X, ovrButton_Y, ovrButton_A, ovrButton_B);
                break;
        }
        rightTrackedRemoteState_old = rightTrackedRemoteState_new;
        leftTrackedRemoteState_old = leftTrackedRemoteState_new;
    }

    Qcommon_BeginFrame(time * 1000);
    if (q2xrFrameLogCount < 6) {
        ALOGV("Q2XR frame %d Qcommon_BeginFrame complete", q2xrFrameLogCount);
    }

    XrViewLocateInfo viewLocateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
    viewLocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    viewLocateInfo.displayTime = gApp.FrameState.predictedDisplayTime;
    viewLocateInfo.space = gApp.StageSpace;
    XrViewState viewState = {XR_TYPE_VIEW_STATE};
    uint32_t viewCount = 0;
    Q2XR_CHECK_XR(xrLocateViews(gApp.Session, &viewLocateInfo, &viewState, NUM_EYES, &viewCount, gApp.Views));
    for (int eye = 0; eye < NUM_EYES; ++eye) {
        Cvar_SetValue(va("gl1_openxr_fov_left_%d", eye), tanf(gApp.Views[eye].fov.angleLeft));
        Cvar_SetValue(va("gl1_openxr_fov_right_%d", eye), tanf(gApp.Views[eye].fov.angleRight));
        Cvar_SetValue(va("gl1_openxr_fov_up_%d", eye), tanf(gApp.Views[eye].fov.angleUp));
        Cvar_SetValue(va("gl1_openxr_fov_down_%d", eye), tanf(gApp.Views[eye].fov.angleDown));
    }

    bool endedQuakeFrame = false;
    const bool screenLayer = useScreenLayer();
    if (screenLayer && !q2xrWasUsingScreenLayer) {
        float screenDistance = Cvar_VariableValue("vr_screen_depth");
        const float yaw = hmdorientation[YAW];
        if (screenDistance <= 0.0f) {
            screenDistance = 3.5f;
        }
        q2xrScreenLayerPose.orientation = q2xr_QuatFromYaw(yaw);
        q2xrScreenLayerPose.position.x = q2xrHeadPoseStage.position.x - sinf(radians(yaw)) * screenDistance;
        q2xrScreenLayerPose.position.y = q2xrHeadPoseStage.position.y;
        q2xrScreenLayerPose.position.z = q2xrHeadPoseStage.position.z - cosf(radians(yaw)) * screenDistance;
    }
    q2xrWasUsingScreenLayer = screenLayer;

    XrCompositionLayerProjection projectionLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView projectionViews[NUM_EYES];
    XrCompositionLayerQuad quadLayer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    memset(projectionViews, 0, sizeof(projectionViews));

    int eyeCount = screenLayer ? 1 : NUM_EYES;
    for (int eye = 0; eye < eyeCount; ++eye) {
        q2xrFramebuffer *fb = &gApp.Eye[eye];
        if (!q2xrFramebuffer_Acquire(fb)) {
            continue;
        }
        if (q2xrFrameLogCount < 6) {
            ALOGV("Q2XR frame %d eye %d acquired image %u framebuffer=%u", q2xrFrameLogCount, eye, fb->Index, fb->FrameBuffers[fb->Index]);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, fb->FrameBuffers[fb->Index]);
        glViewport(0, 0, fb->Width, fb->Height);
        glScissor(0, 0, fb->Width, fb->Height);
        glEnable(GL_SCISSOR_TEST);
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_FRAMEBUFFER_SRGB);

        Qcommon_Frame(screenLayer ? 0 : eye);
        if (q2xrFrameLogCount < 6) {
            ALOGV("Q2XR frame %d eye %d Qcommon_Frame complete", q2xrFrameLogCount, eye);
        }

        if (!endedQuakeFrame && eye == eyeCount - 1) {
            Qcommon_EndFrame(time * 1000);
            endedQuakeFrame = true;
            if (q2xrFrameLogCount < 6) {
                ALOGV("Q2XR frame %d Qcommon_EndFrame complete", q2xrFrameLogCount);
            }
        }

        const GLenum depthAttachment[1] = {GL_DEPTH_ATTACHMENT};
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, depthAttachment);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        q2xrFramebuffer_Release(fb);

        if (!screenLayer) {
            projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            projectionViews[eye].pose = gApp.Views[eye].pose;
            projectionViews[eye].fov = gApp.Views[eye].fov;
            projectionViews[eye].subImage.swapchain = fb->Handle;
            projectionViews[eye].subImage.imageRect.offset.x = 0;
            projectionViews[eye].subImage.imageRect.offset.y = 0;
            projectionViews[eye].subImage.imageRect.extent.width = fb->Width;
            projectionViews[eye].subImage.imageRect.extent.height = fb->Height;
        } else {
            const float screenAspect = (float)fb->Width / (float)fb->Height;
            quadLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
            quadLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                   XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT;
            quadLayer.space = gApp.StageSpace;
            quadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quadLayer.subImage.swapchain = fb->Handle;
            quadLayer.subImage.imageRect.offset.x = 0;
            quadLayer.subImage.imageRect.offset.y = 0;
            quadLayer.subImage.imageRect.extent.width = fb->Width;
            quadLayer.subImage.imageRect.extent.height = fb->Height;
            quadLayer.pose = q2xrScreenLayerPose;
            quadLayer.size.width = 3.0f;
            quadLayer.size.height = quadLayer.size.width / screenAspect;
        }
    }

    if (!endedQuakeFrame) {
        Qcommon_EndFrame(time * 1000);
        if (q2xrFrameLogCount < 6) {
            ALOGV("Q2XR frame %d Qcommon_EndFrame complete without eye target", q2xrFrameLogCount);
        }
    }
    oldtime = global_time;

    const XrCompositionLayerBaseHeader *layers[2];
    uint32_t layerCount = 0;
    if (!screenLayer) {
        projectionLayer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
        projectionLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                     XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT;
        projectionLayer.space = gApp.StageSpace;
        projectionLayer.viewCount = NUM_EYES;
        projectionLayer.views = projectionViews;
        layers[layerCount++] = (const XrCompositionLayerBaseHeader *)&projectionLayer;
    } else if (quadLayer.subImage.swapchain != XR_NULL_HANDLE) {
        layers[layerCount++] = (const XrCompositionLayerBaseHeader *)&quadLayer;
    }

    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = gApp.FrameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = gApp.FrameState.shouldRender ? layerCount : 0;
    endInfo.layers = gApp.FrameState.shouldRender ? layers : NULL;
    Q2XR_CHECK_XR(xrEndFrame(gApp.Session, &endInfo));
    if (q2xrFrameLogCount < 6) {
        ALOGV("Q2XR frame %d xrEndFrame complete", q2xrFrameLogCount);
    }
    q2xrFrameLogCount++;
}

bool useScreenLayer()
{
    return ((cls.state != ca_connected && cls.state != ca_active) ||
            cls.key_dest != key_game ||
            cl.attractloop ||              /* startup/attract demo: show on the flat cinematic screen, not 6DoF */
            cl.cinematictime != 0);
}

void Q2VR_exit(int exitCode)
{
    (void)exitCode;
}

static void UnEscapeQuotes(char *arg)
{
    char *last = NULL;
    while (*arg) {
        if (*arg == '"' && last && *last == '\\') {
            char *c_curr = arg;
            char *c_last = last;
            while (*c_curr) {
                *c_last = *c_curr;
                c_last = c_curr;
                c_curr++;
            }
            *c_last = '\0';
        }
        last = arg;
        arg++;
    }
}

static int ParseCommandLine(char *cmdline, char **outArgv)
{
    char *bufp;
    char *lastp = NULL;
    int count = 0;
    int last_count = 0;

    for (bufp = cmdline; *bufp;) {
        while (isspace(*bufp)) {
            ++bufp;
        }
        if (*bufp == '"') {
            ++bufp;
            if (*bufp) {
                if (outArgv) outArgv[count] = bufp;
                ++count;
            }
            while (*bufp && (*bufp != '"' || (lastp && *lastp == '\\'))) {
                lastp = bufp;
                ++bufp;
            }
        } else {
            if (*bufp) {
                if (outArgv) outArgv[count] = bufp;
                ++count;
            }
            while (*bufp && !isspace(*bufp)) {
                ++bufp;
            }
        }
        if (*bufp) {
            if (outArgv) *bufp = '\0';
            ++bufp;
        }
        if (outArgv && last_count != count) {
            UnEscapeQuotes(outArgv[last_count]);
        }
        last_count = count;
    }
    if (outArgv) {
        outArgv[count] = NULL;
    }
    return count;
}

void initialize_gl4es();
void Qcommon_Init(int argc, char **argv);
void FS_AddDirToSearchPath(char *dir, qboolean create);

void Quest_GetScreenRes(int *width, int *height)
{
    if (useScreenLayer()) {
        *width = (int)cylinderSize[0];
        *height = (int)cylinderSize[1];
    } else {
        *width = gApp.Width ? gApp.Width : 1024;
        *height = gApp.Height ? gApp.Height : 1024;
    }
}

int Quest_GetRefresh()
{
    return gApp.RefreshRate ? gApp.RefreshRate : 90;
}

float getFOV()
{
    return 90.0f;
}

void Quest_MessageBox(const char *title, const char *text)
{
    ALOGE("%s %s", title, text);
}

void VR_Init()
{
    playerYaw = 0.0f;
    showingScreenLayer = false;
    remote_movementSideways = 0.0f;
    remote_movementForward = 0.0f;
    remote_movementUp = 0.0f;
    positional_movementSideways = 0.0f;
    positional_movementForward = 0.0f;
    snapTurn = 0.0f;
    ducked = DUCK_NOTDUCKED;

    srand(time(NULL));

    vr_snapturn_angle = Cvar_Get("vr_snapturn_angle", "45", CVAR_ARCHIVE);
    vr_smoothturn = Cvar_Get("vr_smoothturn", "0", CVAR_ARCHIVE);
    vr_walkdirection = Cvar_Get("vr_walkdirection", "1", CVAR_ARCHIVE); /* 1 = gaze/HMD direction (default), 0 = off-hand controller */
    vr_weapon_pitchadjust = Cvar_Get("vr_weapon_pitchadjust", "-20.0", CVAR_ARCHIVE);
    vr_control_scheme = Cvar_Get("vr_control_scheme", "0", CVAR_ARCHIVE);
    vr_height_adjust = Cvar_Get("vr_height_adjust", "0.0", CVAR_ARCHIVE);
    vr_weaponscale = Cvar_Get("vr_weaponscale", "0.56", CVAR_ARCHIVE);
    vr_weapon_stabilised = Cvar_Get("vr_weapon_stabilised", "0.0", CVAR_LATCH);
    vr_comfort_mask = Cvar_Get("vr_comfort_mask", "0.0", CVAR_ARCHIVE);
    vr_turn_deadzone = Cvar_Get("vr_turn_deadzone", "0.2", CVAR_ARCHIVE);
    vr_framerate = Cvar_Get("vr_framerate", "0", CVAR_ARCHIVE);
    vr_use_wheels = Cvar_Get("vr_use_wheels", "1", CVAR_ARCHIVE);
    vr_jump_sound = Cvar_Get("vr_jump_sound", "1", CVAR_ARCHIVE);
    vr_worldscale = Cvar_Get("vr_worldscale", "26.2467", CVAR_ARCHIVE);
	vr_lasersight = Cvar_Get("vr_lasersight", "2", CVAR_ARCHIVE); /* 2 = simple laser dot */
    Cvar_Get("vr_hud_depth", "0.5", CVAR_ARCHIVE);
    Cvar_Get("vr_hud_ipd", "0.064", CVAR_ARCHIVE);
    Cvar_Get("vr_screen_depth", "3.5", CVAR_ARCHIVE);

    refresh_names = malloc(2 * sizeof(char *));
    refresh_values = malloc(sizeof(float));
    refresh_names[0] = "90Hz";
    refresh_names[1] = NULL;
    refresh_values[0] = 90.0f;
}

void QuatToYawPitchRoll(ovrQuatf q, float pitchAdjust, vec3_t out)
{
    XrQuaternionf adjusted = q;
    if (pitchAdjust != 0.0f) {
        XrQuaternionf pitchQuat = {sinf(radians(pitchAdjust) * 0.5f), 0.0f, 0.0f, cosf(radians(pitchAdjust) * 0.5f)};
        adjusted.x = q.w * pitchQuat.x + q.x * pitchQuat.w + q.y * pitchQuat.z - q.z * pitchQuat.y;
        adjusted.y = q.w * pitchQuat.y - q.x * pitchQuat.z + q.y * pitchQuat.w + q.z * pitchQuat.x;
        adjusted.z = q.w * pitchQuat.z + q.x * pitchQuat.y - q.y * pitchQuat.x + q.z * pitchQuat.w;
        adjusted.w = q.w * pitchQuat.w - q.x * pitchQuat.x - q.y * pitchQuat.y - q.z * pitchQuat.z;
    }

    XrVector3f forwardInVr = q2xr_QuatRotateVector(adjusted, (XrVector3f){0.0f, 0.0f, -1.0f});
    XrVector3f rightInVr = q2xr_QuatRotateVector(adjusted, (XrVector3f){1.0f, 0.0f, 0.0f});
    XrVector3f upInVr = q2xr_QuatRotateVector(adjusted, (XrVector3f){0.0f, 1.0f, 0.0f});

    vec3_t forward = {-forwardInVr.z, -forwardInVr.x, forwardInVr.y};
    vec3_t right = {-rightInVr.z, -rightInVr.x, rightInVr.y};
    vec3_t up = {-upInVr.z, -upInVr.x, upInVr.y};
    VectorNormalize(forward);
    VectorNormalize(right);
    VectorNormalize(up);

    const float sp = -forward[2];
    const float cp_x_cy = forward[0];
    const float cp_x_sy = forward[1];
    const float cp_x_sr = -right[2];
    const float cp_x_cr = up[2];

    float yaw = atan2f(cp_x_sy, cp_x_cy);
    float roll = atan2f(cp_x_sr, cp_x_cr);
    float cy = cosf(yaw);
    float sy = sinf(yaw);
    float cr = cosf(roll);
    float sr = sinf(roll);
    float cp;

    if (fabsf(cy) > EQUAL_EPSILON) {
        cp = cp_x_cy / cy;
    } else if (fabsf(sy) > EQUAL_EPSILON) {
        cp = cp_x_sy / sy;
    } else if (fabsf(sr) > EQUAL_EPSILON) {
        cp = cp_x_sr / sr;
    } else if (fabsf(cr) > EQUAL_EPSILON) {
        cp = cp_x_cr / cr;
    } else {
        cp = cosf(asinf(sp));
    }

    out[PITCH] = degrees(atan2f(sp, cp));
    out[YAW] = degrees(yaw);
    out[ROLL] = degrees(roll);

    while (out[PITCH] >= 90.0f) out[PITCH] -= 180.0f;
    while (out[PITCH] < -90.0f) out[PITCH] += 180.0f;
    while (out[YAW] >= 180.0f) out[YAW] -= 360.0f;
    while (out[YAW] < -180.0f) out[YAW] += 360.0f;
    while (out[ROLL] >= 180.0f) out[ROLL] -= 360.0f;
    while (out[ROLL] < -180.0f) out[ROLL] += 360.0f;
}

void setWorldPosition(float x, float y, float z)
{
    vec3_t oldPosition;
    VectorSet(oldPosition, worldPosition[0], worldPosition[1], worldPosition[2]);
    VectorSet(worldPosition, x, y, z);
    VectorSet(positionDeltaThisFrame, worldPosition[0] - oldPosition[0], worldPosition[1] - oldPosition[1], worldPosition[2] - oldPosition[2]);
}

void setHMDPosition(float x, float y, float z, float yaw)
{
    VectorSet(hmdPosition, -x, y, -z);
    if (!player_moving) {
        playerYaw = yaw;
    }
}

void getVROrigins(vec3_t _weaponoffset, vec3_t _weaponangles, vec3_t _hmdPosition)
{
    VectorCopy(weaponoffset, _weaponoffset);
    VectorCopy(weaponangles, _weaponangles);
    VectorCopy(hmdPosition, _hmdPosition);
}

void VR_GetMove(float *forward, float *side, float *up, float *yaw, float *pitch, float *roll)
{
    // Safety net: only let room-scale/head positional movement drive the player while
    // on the ground. When airborne, physically leaning into geometry could otherwise
    // shove the collision box into a wall and wedge the player mid-jump. Thumbstick
    // (remote_*) air-control is unaffected.
    qboolean onGround = (cl.frame.playerstate.pmove.pm_flags & PMF_ON_GROUND) != 0;
    float posForward = onGround ? positional_movementForward : 0.0f;
    float posSide = onGround ? positional_movementSideways : 0.0f;

    *forward = remote_movementForward + posForward;
    *side = remote_movementSideways + posSide;
    *up = remote_movementUp;
    *yaw = hmdorientation[YAW] + snapTurn;
    *pitch = hmdorientation[PITCH];
    *roll = hmdorientation[ROLL];
}

static void *AppThreadFunction(void *parm)
{
    q2xrAppThread *thread = (q2xrAppThread *)parm;
    JNIEnv *env = NULL;
    (*thread->JavaVm)->AttachCurrentThread(thread->JavaVm, &env, NULL);
    prctl(PR_SET_NAME, (long)"Q2XR::Main", 0, 0, 0);

    q2xrEgl_Clear(&gApp.Egl);
    ALOGV("Q2XR app thread starting");
    if (!q2xrEgl_Create(&gApp.Egl)) {
        ALOGE("EGL initialization failed");
        return NULL;
    }
    initialize_gl4es();
    ALOGV("Q2XR initialize_gl4es complete");
    if (!eglMakeCurrent(gApp.Egl.Display, gApp.Egl.TinySurface, gApp.Egl.TinySurface, gApp.Egl.Context)) {
        ALOGE("eglMakeCurrent failed after GL4ES initialization");
        return NULL;
    }
    if (!q2xr_InitOpenXR(thread)) {
        ALOGE("OpenXR initialization failed");
        return NULL;
    }
    ALOGV("Q2XR OpenXR initialization complete");

    chdir("/sdcard");

    const char *baseCmd = thread->CommandLine ? thread->CommandLine : "quake2";
    size_t cmdLen = strlen(baseCmd) + 160;
    char *cmd = malloc(cmdLen);
    if (cmd == NULL) {
        ALOGE("Unable to allocate command line");
        return NULL;
    }
    snprintf(cmd, cmdLen,
             "%s +set r_mode -1 +set r_customwidth %d +set r_customheight %d +set gl1_stereo %d",
             baseCmd,
             gApp.Width,
             gApp.Height,
             Q2XR_STEREO_OPENXR);
    argc = ParseCommandLine(cmd, NULL);
    argv = calloc(argc + 1, sizeof(char *));
    ParseCommandLine(cmd, argv);
    if (argc == 0) {
        argc = 1;
        argv[0] = "quake2";
    }

    Qcommon_Init(argc, argv);
    Cvar_SetValue("cl_showfps", 0);
    ALOGV("Q2XR Qcommon_Init complete argc=%d", argc);
    FS_AddDirToSearchPath("/sdcard/Quake2Quest", true);
    quake2_initialised = true;
    ALOGV("Q2XR entering render loop");

    pthread_mutex_lock(&thread->Mutex);
    while (!thread->Destroyed) {
        bool shouldRender = thread->Resumed;
        pthread_mutex_unlock(&thread->Mutex);

        if (shouldRender) {
            q2xr_RenderFrame();
        } else {
            usleep(10000);
            q2xr_ProcessEvents();
        }

        pthread_mutex_lock(&thread->Mutex);
    }
    pthread_mutex_unlock(&thread->Mutex);

    q2xr_DestroyOpenXR();
    q2xrEgl_Destroy(&gApp.Egl);
    (*thread->JavaVm)->DetachCurrentThread(thread->JavaVm);
    return NULL;
}

static void q2xrAppThread_Create(q2xrAppThread *thread, JNIEnv *env, jobject activityObject, jclass activityClass, const char *commandLine)
{
    memset(thread, 0, sizeof(*thread));
    (*env)->GetJavaVM(env, &thread->JavaVm);
    thread->ActivityObject = (*env)->NewGlobalRef(env, activityObject);
    thread->ActivityClass = (*env)->NewGlobalRef(env, activityClass);
    thread->CommandLine = commandLine ? strdup(commandLine) : strdup("quake2");
    pthread_mutex_init(&thread->Mutex, NULL);
    pthread_cond_init(&thread->Cond, NULL);
    pthread_create(&thread->Thread, NULL, AppThreadFunction, thread);
}

static void q2xrAppThread_Destroy(q2xrAppThread *thread, JNIEnv *env)
{
    pthread_mutex_lock(&thread->Mutex);
    thread->Destroyed = true;
    pthread_mutex_unlock(&thread->Mutex);
    pthread_join(thread->Thread, NULL);
    if (thread->NativeWindow) {
        ANativeWindow_release(thread->NativeWindow);
    }
    (*env)->DeleteGlobalRef(env, thread->ActivityObject);
    (*env)->DeleteGlobalRef(env, thread->ActivityClass);
    free(thread->CommandLine);
    pthread_cond_destroy(&thread->Cond);
    pthread_mutex_destroy(&thread->Mutex);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)vm;
    (void)reserved;
    return JNI_VERSION_1_4;
}

JNIEXPORT jlong JNICALL Java_com_drbeef_quake2quest_GLES3JNILib_onCreate(JNIEnv *env, jclass activityClass, jobject activity, jstring commandLineParams)
{
    const char *commandLine = (*env)->GetStringUTFChars(env, commandLineParams, 0);
    q2xrAppThread *thread = malloc(sizeof(q2xrAppThread));
    q2xrAppThread_Create(thread, env, activity, activityClass, commandLine);
    (*env)->ReleaseStringUTFChars(env, commandLineParams, commandLine);
    return (jlong)((size_t)thread);
}

JNIEXPORT void JNICALL Java_com_drbeef_quake2quest_GLES3JNILib_onStart(JNIEnv *env, jclass clazz, jlong handle)
{
    (void)env; (void)clazz; (void)handle;
}

JNIEXPORT void JNICALL Java_com_drbeef_quake2quest_GLES3JNILib_onResume(JNIEnv *env, jclass clazz, jlong handle)
{
    (void)env; (void)clazz;
    q2xrAppThread *thread = (q2xrAppThread *)((size_t)handle);
    pthread_mutex_lock(&thread->Mutex);
    thread->Resumed = true;
    pthread_mutex_unlock(&thread->Mutex);
}

JNIEXPORT void JNICALL Java_com_drbeef_quake2quest_GLES3JNILib_onPause(JNIEnv *env, jclass clazz, jlong handle)
{
    (void)env; (void)clazz;
    q2xrAppThread *thread = (q2xrAppThread *)((size_t)handle);
    pthread_mutex_lock(&thread->Mutex);
    thread->Resumed = false;
    pthread_mutex_unlock(&thread->Mutex);
}

JNIEXPORT void JNICALL Java_com_drbeef_quake2quest_GLES3JNILib_onStop(JNIEnv *env, jclass clazz, jlong handle)
{
    (void)env; (void)clazz; (void)handle;
}

JNIEXPORT void JNICALL Java_com_drbeef_quake2quest_GLES3JNILib_onDestroy(JNIEnv *env, jclass clazz, jlong handle)
{
    (void)clazz;
    if (handle != 0) {
        q2xrAppThread *thread = (q2xrAppThread *)((size_t)handle);
        q2xrAppThread_Destroy(thread, env);
        free(thread);
    }
}

JNIEXPORT void JNICALL Java_com_drbeef_quake2quest_GLES3JNILib_onSurfaceCreated(JNIEnv *env, jclass clazz, jlong handle, jobject surface)
{
    (void)clazz;
    q2xrAppThread *thread = (q2xrAppThread *)((size_t)handle);
    if (!thread) return;
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    pthread_mutex_lock(&thread->Mutex);
    if (thread->NativeWindow) {
        ANativeWindow_release(thread->NativeWindow);
    }
    thread->NativeWindow = window;
    pthread_mutex_unlock(&thread->Mutex);
}

JNIEXPORT void JNICALL Java_com_drbeef_quake2quest_GLES3JNILib_onSurfaceChanged(JNIEnv *env, jclass clazz, jlong handle, jobject surface)
{
    Java_com_drbeef_quake2quest_GLES3JNILib_onSurfaceCreated(env, clazz, handle, surface);
}

JNIEXPORT void JNICALL Java_com_drbeef_quake2quest_GLES3JNILib_onSurfaceDestroyed(JNIEnv *env, jclass clazz, jlong handle)
{
    (void)env; (void)clazz;
    q2xrAppThread *thread = (q2xrAppThread *)((size_t)handle);
    if (!thread) return;
    pthread_mutex_lock(&thread->Mutex);
    if (thread->NativeWindow) {
        ANativeWindow_release(thread->NativeWindow);
        thread->NativeWindow = NULL;
    }
    pthread_mutex_unlock(&thread->Mutex);
}

