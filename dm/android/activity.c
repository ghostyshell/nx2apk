// The Android app: a NativeActivity (no Java code) around the host. The host's main runs on a thread of its own, as on the command line; the activity hands it the window
// (dm_vk_set_window, the Vulkan backend's swapchain) and the input: a game controller and the touch screen, mapped onto the Switch buttons (dm_pad_set / dm_input_set).
// Files (adb pushes them; see build_apk.sh): <external files>/mods (the rewritten modules), <external files>/vksh (the shaders), the RomFS at DM_ROMFS (default /sdcard/nx/dmc/romfs,
// which needs the MANAGE_EXTERNAL_STORAGE app op), the save and temp folders under the internal files folder. <external files>/env.txt sets further variables (KEY=VALUE lines);
// the log goes to <external files>/dm.log
#include <android/native_activity.h>
#include <android/asset_manager.h>
#include <android/configuration.h>
#include <android/input.h>
#include <android/looper.h>
#include <android/window.h>
#include <android/log.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
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

// ---------- Bundled data (optional) ----------
// A build made with BUNDLE=1 packs the rewritten modules and shaders as APK assets (build_apk.sh), so a
// personal reinstall is one APK plus the RomFS: on first run (or when the bundle's stamp changes) the assets
// are extracted into internal storage and used instead of the folders under the external files dir. No
// bundle -> the paths are exactly as before. Personal installs only: the bundle carries the game's own
// rewritten code, it must never be what a public download serves (see README).
static int g_bundle;

static void rm_files(const char *dir) {
    DIR *dp = opendir(dir); if (!dp) return;
    struct dirent *e; char p[1024];
    while ((e = readdir(dp))) { if (e->d_name[0] == '.') continue; snprintf(p, sizeof p, "%s/%s", dir, e->d_name); unlink(p); }
    closedir(dp);
}
static int bundle_one(AAssetManager *am, const char *adir, const char *dest, int verbose) {
    AAssetDir *d = AAssetManager_openDir(am, adir);
    if (!d) { if (verbose) __android_log_print(ANDROID_LOG_INFO, "dm", "bundle: openDir %s failed", adir); return 0; }
    const char *fn; int n = 0, seen = 0;
    while ((fn = AAssetDir_getNextFileName(d))) {
        seen++;
        char apath[256]; snprintf(apath, sizeof apath, "%s/%s", adir, fn);   // open() resolves from the assets root, the enumerator returns bare names
        AAsset *a = AAssetManager_open(am, apath, AASSET_MODE_STREAMING);
        if (!a) { if (verbose) __android_log_print(ANDROID_LOG_INFO, "dm", "bundle: open %s failed", apath); continue; }
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dest, fn);
        FILE *f = fopen(p, "w");
        if (!f) { if (verbose) __android_log_print(ANDROID_LOG_INFO, "dm", "bundle: write %s failed: %s", p, strerror(errno)); AAsset_close(a); continue; }
        char buf[65536]; int r; while ((r = AAsset_read(a, buf, sizeof buf)) > 0) fwrite(buf, 1, r, f);
        fclose(f); AAsset_close(a); n++;
    }
    AAssetDir_close(d);
    if (verbose) __android_log_print(ANDROID_LOG_INFO, "dm", "bundle: %s -> %d seen, %d copied", adir, seen, n);
    return n;
}
static int bundle_extract(ANativeActivity *a) {
    AAssetManager *am = a->assetManager;
    AAsset *s = AAssetManager_open(am, "bundle/stamp", AASSET_MODE_BUFFER);
    if (!s) { __android_log_print(ANDROID_LOG_INFO, "dm", "bundle: no stamp asset (am %p)", (void *)am); return 0; }
    char stamp[128]; off_t n = AAsset_getLength64(s); if (n > (off_t)sizeof stamp - 1) n = sizeof stamp - 1;
    memcpy(stamp, AAsset_getBuffer(s), n); stamp[n] = 0; AAsset_close(s);
    char old[600]; snprintf(old, sizeof old, "%s/bundle.stamp", g_int);
    FILE *f = fopen(old, "r"); char cur[128] = {0};
    if (f) { size_t r = fread(cur, 1, sizeof cur - 1, f); cur[r] = 0; fclose(f); }
    if (!strcmp(cur, stamp)) return 1;   // this exact bundle is already on disk
    int total = 0;
    const char *names[] = {"mods", "vksh", "texpatch"};
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) {
        char adir[64], dest[600];
        snprintf(adir, sizeof adir, "bundle/%s", names[i]);
        snprintf(dest, sizeof dest, "%s/%s", g_int, names[i]);
        rm_files(dest); mkdir(dest, 0700);
        total += bundle_one(am, adir, dest, total == 0);   // verbose on the first dir only
    }
    if (!total) return 0;
    f = fopen(old, "w"); if (f) { fputs(stamp, f); fclose(f); }
    __android_log_print(ANDROID_LOG_INFO, "dm", "extracted bundle: %d files into %s", total, g_int);
    return 1;
}

// The screen's shape, so the game renders at the device's own aspect with no env.txt: DM_DISPLAY = the surface's landscape aspect at 720 lines
// (the Switch's handheld mode: docked's 1600x900 scene does not hold 60 fps on phones once the GPU is thermally capped), 1080 with DM_OPMODE=1.
// The surface, not the screen: the game's picture is presented to the window, so its shape is the one that must match (the window is narrower than the
// panel on phones with a cutout). The window arrives right after onCreate; until it does, the screen's dp shape is the stand-in. env.txt wins.
static AAssetManager *g_am;   // set in onCreate: the dp fallback of display_default
static void display_default(AAssetManager *am) {
    if (getenv("DM_DISPLAY")) return;
    int w = 0, h = 0;
    for (int i = 0; i < 1600; i++) {   // the surface comes up a few seconds into onCreate on some phones
        ANativeWindow *win = dm_android_window;
        if (win) { w = ANativeWindow_getWidth(win); h = ANativeWindow_getHeight(win); }
        if (w > 0 && h > 0) break;
        usleep(5000);
    }
    if (w <= 0 || h <= 0) {   // no window yet: the screen's dp shape, whose ratio is the pixel ratio
        AConfiguration *c = am ? AConfiguration_new() : NULL; if (!c) return;
        AConfiguration_fromAssetManager(c, am);
        w = AConfiguration_getScreenWidthDp(c); h = AConfiguration_getScreenHeightDp(c);
        AConfiguration_delete(c);
        if (w <= 0 || h <= 0) return;
    }
    if (w < h) { int t = w; w = h; h = t; }   // the game runs in landscape
    const char *op = getenv("DM_OPMODE");
    int lines = op && !strcmp(op, "1") ? 1080 : 720;
    char buf[32]; snprintf(buf, sizeof buf, "%dx%d", (int)(((long)w * lines / h + 1) & ~1), lines);
    setenv("DM_DISPLAY", buf, 1);
    __android_log_print(ANDROID_LOG_INFO, "dm", "display %dx%d → DM_DISPLAY=%s", w, h, buf);
}
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
    snprintf(p, sizeof p, "%s/vksh", g_bundle ? g_int : g_ext); setenv("DM_VK_SHADERS", p, 0);
    setenv("DM_ROMFS", "/sdcard/nx/dmc/romfs", 0); setenv("DM_FS", "1", 0); setenv("DM_AUDIO", "1", 0);
    setenv("DM_PAD_DMC", "1", 0);   // this app is Devil May Cry: ship the standard Xbox face-button layout (env.txt can clear it)
    // The Switch's handheld mode (720p): docked renders the scene at 1600x900, which the S10e's GPU cannot hold at 60 fps in DMC's heaviest scenes once Samsung's
    // thermal policy caps it at 377 MHz; handheld does. DM_OPMODE=1 in env.txt for docked
    setenv("DM_OPMODE", "0", 0);
    display_default(g_am);   // on this thread, not onCreate's: the window callback arrives on the UI thread, which must stay free
    chdir(g_int);
    static char mods[600]; snprintf(mods, sizeof mods, "%s/mods", g_bundle ? g_int : g_ext);
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
    static int dmc = -1; if (dmc < 0) { const char *e = getenv("DM_PAD_DMC"); dmc = e && !strcmp(e, "1"); }
    if (label && dmc) {
        // Devil May Cry's own Type A scheme is action(jump) / sword / shoot+examine on B / X / A: translate the
        // standard Xbox layout onto it — Xbox A jumps, Y swings, B examines, X shoots (the game examines and
        // shoots with one button, so B and X both reach it; Switch Y has no role). texpatch swaps the art to match.
        switch (k) {
            case AKEYCODE_BUTTON_A: return DM_BTN_B;
            case AKEYCODE_BUTTON_B: case AKEYCODE_BUTTON_X: return DM_BTN_A;
            case AKEYCODE_BUTTON_Y: return DM_BTN_X;
        }
    }
    switch (k) {
        case AKEYCODE_BUTTON_A: return label ? DM_BTN_A : DM_BTN_B; case AKEYCODE_BUTTON_B: return label ? DM_BTN_B : DM_BTN_A;
        case AKEYCODE_BUTTON_X: return label ? DM_BTN_X : DM_BTN_Y; case AKEYCODE_BUTTON_Y: return label ? DM_BTN_Y : DM_BTN_X;
        case AKEYCODE_BUTTON_L1: return DM_BTN_L; case AKEYCODE_BUTTON_R1: return DM_BTN_R; case AKEYCODE_BUTTON_L2: return DM_BTN_ZL; case AKEYCODE_BUTTON_R2: return DM_BTN_ZR;
        case AKEYCODE_BUTTON_START: return DM_BTN_PLUS; case AKEYCODE_BUTTON_SELECT: case AKEYCODE_BACK: return DM_BTN_MINUS;
        case AKEYCODE_PLUS: return DM_BTN_PLUS; case AKEYCODE_MINUS: return DM_BTN_MINUS;   // a keyboard's + / - press the Switch's + / - (start / select-options); controllers whose firmware binds Start to Home are covered by their + / - and View / Select instead
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
            int32_t k = AKeyEvent_getKeyCode(e);
            uint64_t b = key_btn(k);
            if (b) { if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN) g_keys |= b; else if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_UP) g_keys &= ~b; pad_publish(); handled = 1; }
            else if (k == AKEYCODE_HOME || k == AKEYCODE_BUTTON_MODE) handled = 1;   // the controller's Home button: no game button (the host has none) and it must not leave the game for the launcher
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
    g_am = a->assetManager;   // the host thread derives DM_DISPLAY once it has the window
    g_bundle = bundle_extract(a);   // before either thread starts: the host thread picks the mods/shaders source from this
    { const char *l = getenv("DM_PAD_LAYOUT"); char p[600]; snprintf(p, sizeof p, "%s/texpatch", g_bundle ? g_int : g_ext); if (!(l && !strcasecmp(l, "switch"))) setenv("DM_TEXPATCH", p, 0); }   // Xbox button pictures
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 16 << 20);
    pthread_create(&th, NULL, input_thread, NULL); pthread_detach(th);
    pthread_create(&th, &at, host_thread, NULL); pthread_detach(th);
}
