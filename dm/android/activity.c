// The Android app: a NativeActivity (no Java code) around the host. The host's main runs on a thread of its own, as on the command line; the activity hands it the window
// (dm_vk_set_window, the Vulkan backend's swapchain) and the input: a game controller and the touch screen, mapped onto the Switch buttons (dm_pad_set / dm_input_set).
// Files (adb pushes them; see build_apk.sh): <external files>/mods (the rewritten modules), <external files>/vksh (the shaders), the RomFS at DM_ROMFS (default /sdcard/nx/dmc/romfs,
// which needs the MANAGE_EXTERNAL_STORAGE app op), the save and temp folders under the internal files folder. <external files>/env.txt sets further variables (KEY=VALUE lines);
// the log goes to <external files>/dm.log
#include <android/native_activity.h>
#include <android/input.h>
#include <android/looper.h>
#include <android/window.h>
#include <android/log.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../host/dm_input.h"

// The host's lifecycle entry points (hle_oe.c / hle_nvn.c), the same round trip the Mac and iOS front ends run on focus loss and minimize
void dm_focus_set(int st);
void dm_audio_suspend(int on);
void guest_clock_freeze_for(int reason, int on);
void dm_gfx_suspend(int on);

int main(int argc, char **argv);   // host/main.c
void dm_vk_set_window(ANativeWindow *w);   // nvn_vk.c
__attribute__((visibility("default"))) ANativeWindow *dm_android_window;

static char g_ext[512], g_int[512];
static AInputQueue *g_queue; static ALooper *g_looper; static pthread_mutex_t g_in_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_started;

static void env_file(void) {
    char p[600]; snprintf(p, sizeof p, "%s/env.txt", g_ext);
    FILE *f = fopen(p, "r"); if (!f) return;
    char ln[1024];
    while (fgets(ln, sizeof ln, f)) {
        ln[strcspn(ln, "\r\n")] = 0; for (size_t k = strlen(ln); k && (ln[k - 1] == ' ' || ln[k - 1] == '\t'); ) ln[--k] = 0;   // trailing blanks
        char *eq = strchr(ln, '='); if (!eq || ln[0] == '#') continue;
        *eq = 0; setenv(ln, eq + 1, 1);
    }
    fclose(f);
    __android_log_print(ANDROID_LOG_INFO, "dm", "env.txt %s: DM_SCALE=%s", p, getenv("DM_SCALE") ? getenv("DM_SCALE") : "(unset)");
}
static void *host_thread(void *arg) {
    (void)arg;
    char p[600];
    snprintf(p, sizeof p, "%s/dm.log", g_ext); freopen(p, "w", stderr); setvbuf(stderr, NULL, _IOLBF, 0);
    snprintf(p, sizeof p, "%s/save", g_int); mkdir(p, 0700); setenv("DM_SAVE", p, 0);
    snprintf(p, sizeof p, "%s/temp", g_int); mkdir(p, 0700); setenv("DM_TEMP", p, 0);
    snprintf(p, sizeof p, "%s/vksh", g_ext); setenv("DM_VK_SHADERS", p, 0);
    setenv("DM_ROMFS", "/sdcard/nx/dmc/romfs", 0); setenv("DM_FS", "1", 0); setenv("DM_AUDIO", "1", 0);
    // The Switch's handheld mode (720p): docked renders the scene at 1600x900, which the S10e's GPU cannot hold at 60 fps in DMC's heaviest scenes once Samsung's
    // thermal policy caps it at 377 MHz; handheld does. DM_OPMODE=1 in env.txt for docked
    setenv("DM_OPMODE", "0", 0);
    chdir(g_int);
    static char mods[600]; snprintf(mods, sizeof mods, "%s/mods", g_ext);
    char *argv[] = {"dm", mods, NULL};
    __android_log_print(ANDROID_LOG_INFO, "dm", "host starting (modules %s, log %s/dm.log)", mods, g_ext);
    int r = main(2, argv);
    __android_log_print(ANDROID_LOG_INFO, "dm", "host returned %d", r);
    exit(r);
    return NULL;
}

// ---------- Input ----------
// Face buttons: by letter by default (Android's A, an Xbox controller's A, is the Switch's A), so the game's prompts ("A Button") name the button to press, and its button
// pictures are redrawn as Xbox ones (texpatch, below). DM_PAD_LAYOUT=switch maps by position instead (the bottom button is the Switch's bottom button B) and keeps the
// Switch pictures
static uint64_t g_keys, g_hat, g_touch_btn; static float g_ax[8]; static int32_t g_touch_lx, g_touch_ly;
static uint64_t key_btn(int32_t k) {
    static int label = -1; if (label < 0) { const char *e = getenv("DM_PAD_LAYOUT"); label = !(e && !strcasecmp(e, "switch")); }
    switch (k) {
        case AKEYCODE_BUTTON_A: return label ? DM_BTN_A : DM_BTN_B; case AKEYCODE_BUTTON_B: return label ? DM_BTN_B : DM_BTN_A;
        case AKEYCODE_BUTTON_X: return label ? DM_BTN_X : DM_BTN_Y; case AKEYCODE_BUTTON_Y: return label ? DM_BTN_Y : DM_BTN_X;
        case AKEYCODE_BUTTON_L1: return DM_BTN_L; case AKEYCODE_BUTTON_R1: return DM_BTN_R; case AKEYCODE_BUTTON_L2: return DM_BTN_ZL; case AKEYCODE_BUTTON_R2: return DM_BTN_ZR;
        case AKEYCODE_BUTTON_START: return DM_BTN_PLUS; case AKEYCODE_BUTTON_SELECT: case AKEYCODE_BACK: return DM_BTN_MINUS;
        case AKEYCODE_PLUS: return DM_BTN_PLUS;   // a keyboard's + presses the Switch's + (START); the handheld's Start button is bound to Home by its firmware
        case AKEYCODE_BUTTON_THUMBL: return DM_BTN_LS; case AKEYCODE_BUTTON_THUMBR: return DM_BTN_RS;
        case AKEYCODE_DPAD_LEFT: return DM_BTN_LEFT; case AKEYCODE_DPAD_RIGHT: return DM_BTN_RIGHT; case AKEYCODE_DPAD_UP: return DM_BTN_UP; case AKEYCODE_DPAD_DOWN: return DM_BTN_DOWN;
    }
    return 0;
}
static int32_t stick(float v) { if (v > -0.12f && v < 0.12f) return 0; return (int32_t)(v * DM_STICK_MAX); }
static void pad_publish(void) {
    uint64_t b = g_keys | g_hat;
    if (g_ax[4] > 0.5f) b |= DM_BTN_ZL;
    if (g_ax[5] > 0.5f) b |= DM_BTN_ZR;
    dm_pad_set(b, stick(g_ax[0]), -stick(g_ax[1]), stick(g_ax[2]), -stick(g_ax[3]));
}
// Touch: the left half is a floating stick (the first touch point is its centre), the right half four face buttons around its centre, the top corners L / R and the top middle Plus
#define NTOUCH 10
static struct { int id, kind; float x0, y0; } g_tp[NTOUCH];
static void touch_publish(void) { dm_input_set(g_touch_btn, g_touch_lx, g_touch_ly, 0, 0); }
static uint64_t touch_zone(float x, float y, float w, float h) {
    if (y < h * 0.18f) return x < w * 0.25f ? DM_BTN_L : x > w * 0.75f ? DM_BTN_R : x > w * 0.4f && x < w * 0.6f ? DM_BTN_PLUS : 0;
    float cx = w * 0.82f, cy = h * 0.62f, dx = x - cx, dy = y - cy;
    if (dx * dx > dy * dy) return dx > 0 ? DM_BTN_A : DM_BTN_Y;
    return dy > 0 ? DM_BTN_B : DM_BTN_X;
}
static void touch_event(const AInputEvent *e) {
    int32_t a = AMotionEvent_getAction(e), act = a & AMOTION_EVENT_ACTION_MASK, idx = (a & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    ANativeWindow *win = dm_android_window; if (!win) return;
    float w = (float)ANativeWindow_getWidth(win), h = (float)ANativeWindow_getHeight(win);
    if (act == AMOTION_EVENT_ACTION_DOWN || act == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        int id = AMotionEvent_getPointerId(e, (size_t)idx); float x = AMotionEvent_getX(e, (size_t)idx), y = AMotionEvent_getY(e, (size_t)idx);
        for (int i = 0; i < NTOUCH; i++) if (!g_tp[i].kind) { g_tp[i].id = id; g_tp[i].x0 = x; g_tp[i].y0 = y; g_tp[i].kind = x < w * 0.5f && y > h * 0.18f ? 1 : 2; break; }
    } else if (act == AMOTION_EVENT_ACTION_UP || act == AMOTION_EVENT_ACTION_POINTER_UP || act == AMOTION_EVENT_ACTION_CANCEL) {
        int id = act == AMOTION_EVENT_ACTION_CANCEL ? -1 : AMotionEvent_getPointerId(e, (size_t)idx);
        for (int i = 0; i < NTOUCH; i++) if (g_tp[i].kind && (id < 0 || g_tp[i].id == id)) g_tp[i].kind = 0;
    }
    g_touch_btn = 0; g_touch_lx = g_touch_ly = 0;
    size_t n = AMotionEvent_getPointerCount(e);
    for (size_t p = 0; p < n; p++) {
        int id = AMotionEvent_getPointerId(e, p); float x = AMotionEvent_getX(e, p), y = AMotionEvent_getY(e, p);
        for (int i = 0; i < NTOUCH; i++) if (g_tp[i].kind && g_tp[i].id == id) {
            if (g_tp[i].kind == 1) { float r = h * 0.12f, dx = (x - g_tp[i].x0) / r, dy = (y - g_tp[i].y0) / r;
                if (dx > 1) dx = 1; if (dx < -1) dx = -1; if (dy > 1) dy = 1; if (dy < -1) dy = -1; g_touch_lx = stick(dx); g_touch_ly = -stick(dy); }
            else g_touch_btn |= touch_zone(x, y, w, h);
        }
    }
    touch_publish();
}
static int input_cb(int fd, int events, void *data) {
    (void)fd; (void)events; (void)data;
    AInputEvent *e;
    pthread_mutex_lock(&g_in_mu);
    while (g_queue && AInputQueue_getEvent(g_queue, &e) >= 0) {
        if (AInputQueue_preDispatchEvent(g_queue, e)) continue;
        int handled = 0, src = AInputEvent_getSource(e);
        if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_KEY) {
            uint64_t b = key_btn(AKeyEvent_getKeyCode(e));
            if (b) { if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN) g_keys |= b; else if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_UP) g_keys &= ~b; pad_publish(); handled = 1; }
        } else if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_MOTION) {
            if ((src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
                g_ax[0] = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0); g_ax[1] = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0);
                g_ax[2] = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Z, 0); g_ax[3] = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RZ, 0);
                g_ax[4] = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_LTRIGGER, 0) + AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_BRAKE, 0);
                g_ax[5] = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RTRIGGER, 0) + AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_GAS, 0);
                float hx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0), hy = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
                g_hat = (hx < -0.5f ? DM_BTN_LEFT : 0) | (hx > 0.5f ? DM_BTN_RIGHT : 0) | (hy < -0.5f ? DM_BTN_UP : 0) | (hy > 0.5f ? DM_BTN_DOWN : 0);
                pad_publish(); handled = 1;
            } else if (src & AINPUT_SOURCE_TOUCHSCREEN) { touch_event(e); handled = 1; }
        }
        AInputQueue_finishEvent(g_queue, e, handled);
    }
    pthread_mutex_unlock(&g_in_mu);
    return 1;
}
static void *input_thread(void *arg) {
    (void)arg;
    g_looper = ALooper_prepare(0);
    for (;;) ALooper_pollOnce(-1, NULL, NULL, NULL);
    return NULL;
}

// ---------- Activity callbacks ----------
// Background / foreground: without this the game kept rendering and playing music behind the launcher until the app was swiped away
static void on_pause(ANativeActivity *a) { (void)a; dm_focus_set(2); dm_focus_set(3); dm_audio_suspend(1); guest_clock_freeze_for(1, 1); dm_gfx_suspend(1); }
static void on_resume(ANativeActivity *a) { (void)a; dm_gfx_suspend(0); guest_clock_freeze_for(1, 0); dm_audio_suspend(0); dm_focus_set(2); dm_focus_set(1); }
static void on_window_created(ANativeActivity *a, ANativeWindow *w) { (void)a; dm_vk_set_window(w); }
static void on_window_destroyed(ANativeActivity *a, ANativeWindow *w) { (void)a; (void)w; dm_vk_set_window(NULL); }
static void on_queue_created(ANativeActivity *a, AInputQueue *q) {
    (void)a;
    while (!g_looper) usleep(1000);
    pthread_mutex_lock(&g_in_mu); g_queue = q; AInputQueue_attachLooper(q, g_looper, 1, input_cb, NULL); pthread_mutex_unlock(&g_in_mu);
}
static void on_queue_destroyed(ANativeActivity *a, AInputQueue *q) {
    (void)a; pthread_mutex_lock(&g_in_mu); AInputQueue_detachLooper(q); g_queue = NULL; pthread_mutex_unlock(&g_in_mu);
}
static void on_destroy(ANativeActivity *a) { (void)a; __android_log_print(ANDROID_LOG_INFO, "dm", "activity destroyed: exiting"); _exit(0); }

__attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity *a, void *saved, size_t n) {
    (void)saved; (void)n;
    a->callbacks->onNativeWindowCreated = on_window_created;
    a->callbacks->onNativeWindowDestroyed = on_window_destroyed;
    a->callbacks->onPause = on_pause;
    a->callbacks->onResume = on_resume;
    a->callbacks->onInputQueueCreated = on_queue_created;
    a->callbacks->onInputQueueDestroyed = on_queue_destroyed;
    a->callbacks->onDestroy = on_destroy;
    ANativeActivity_setWindowFlags(a, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);
    if (g_started++) return;   // a re-created activity (rotation, return from background) keeps the running host
    snprintf(g_ext, sizeof g_ext, "%s", a->externalDataPath ? a->externalDataPath : a->internalDataPath);
    snprintf(g_int, sizeof g_int, "%s", a->internalDataPath);
    mkdir(g_ext, 0700);
    env_file();   // before either thread starts: the input thread reads DM_PAD_LAYOUT, the host the rest (its defaults above do not overwrite these)
    { const char *l = getenv("DM_PAD_LAYOUT"); char p[600]; snprintf(p, sizeof p, "%s/texpatch", g_ext); if (!(l && !strcasecmp(l, "switch"))) setenv("DM_TEXPATCH", p, 0); }   // Xbox button pictures
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 16 << 20);
    pthread_create(&th, NULL, input_thread, NULL); pthread_detach(th);
    pthread_create(&th, &at, host_thread, NULL); pthread_detach(th);
}
