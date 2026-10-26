// Host front end on Android. Bring-up form: no window, the guest runs on the calling thread (as the Mac host does without a window).
#include "../host/dm.h"
#include "../host/front.h"

void front_run(void (*guest)(void)) { guest(); }
void *front_metal_layer(void) { return NULL; }
int front_screen_max_fps(void) { return 60; }
void front_screen_match(int fps) { (void)fps; }
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    (void)title; (void)sub; (void)guide; (void)initial; (void)max_chars; (void)out; (void)out_cap;
    return -1;
}
void front_save_failed(int err) { LOG("[RED] save not written (errno %d)", err); }
