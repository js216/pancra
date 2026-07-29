// SPDX-License-Identifier: GPL-3.0
// ndk.h --- Minimal Android NDK declarations (we don't ship the NDK headers)
// Copyright 2026 Jakob Kastelic

/* The app is built freestanding without the Android NDK, so the handful of NDK
 * types/functions/constants it uses are mirrored here by hand (ABI-compatible
 * with <android/native_activity.h>, <android/looper.h>, <android/input.h>,
 * <android/native_window.h>). All the NDK constants live here, not scattered
 * through the sources. The host offline UI harness provides mock definitions of
 * the ANativeWindow_* functions (see test/). */
#ifndef PANCRA_NDK_H
#define PANCRA_NDK_H

#include <jni.h> /* JavaVM, JNIEnv, jobject in ANativeActivity */
#include <stddef.h>
#include <stdint.h>

/* ---- native window / framebuffer ---- */
struct ANativeWindow; /* opaque platform handle */

struct ANativeWindow_Buffer {
   int32_t width, height, stride, format;
   void *bits;
   uint32_t reserved[6];
};

struct ARect {
   int32_t left, top, right, bottom;
};

#define WINDOW_FORMAT_RGBA_8888 1

int32_t ANativeWindow_setBuffersGeometry(struct ANativeWindow *w, int32_t width,
                                         int32_t height, int32_t format);
int32_t ANativeWindow_lock(struct ANativeWindow *w,
                           struct ANativeWindow_Buffer *out,
                           struct ARect *dirty);
int32_t ANativeWindow_unlockAndPost(struct ANativeWindow *w);

/* ---- looper (event loop) ---- */
struct ALooper;
struct ALooper *ALooper_forThread(void);
int ALooper_addFd(struct ALooper *l, int fd, int ident, int events,
                  int (*cb)(int fd, int events, void *data), void *data);
int ALooper_removeFd(struct ALooper *l, int fd);
#define ALOOPER_EVENT_INPUT 1

/* ---- input queue / motion events ---- */
struct AInputQueue;
struct AInputEvent;
void AInputQueue_attachLooper(struct AInputQueue *q, struct ALooper *l,
                              int ident,
                              int (*cb)(int fd, int events, void *data),
                              void *data);
void AInputQueue_detachLooper(struct AInputQueue *q);
int32_t AInputQueue_getEvent(struct AInputQueue *q, struct AInputEvent **ev);
int32_t AInputQueue_preDispatchEvent(struct AInputQueue *q,
                                     struct AInputEvent *ev);
void AInputQueue_finishEvent(struct AInputQueue *q, struct AInputEvent *ev,
                             int handled);
int32_t AInputEvent_getType(const struct AInputEvent *ev);
int32_t AMotionEvent_getAction(const struct AInputEvent *ev);
float AMotionEvent_getX(const struct AInputEvent *ev, unsigned long idx);
float AMotionEvent_getY(const struct AInputEvent *ev, unsigned long idx);
unsigned long AMotionEvent_getHistorySize(const struct AInputEvent *ev);
float AMotionEvent_getHistoricalX(const struct AInputEvent *ev,
                                  unsigned long idx, unsigned long h);
float AMotionEvent_getHistoricalY(const struct AInputEvent *ev,
                                  unsigned long idx, unsigned long h);
int32_t AKeyEvent_getAction(const struct AInputEvent *ev);
int32_t AKeyEvent_getKeyCode(const struct AInputEvent *ev);
#define AINPUT_EVENT_TYPE_KEY       1
#define AINPUT_EVENT_TYPE_MOTION    2
#define AKEY_EVENT_ACTION_DOWN      0
#define AKEY_EVENT_ACTION_UP        1
#define AKEYCODE_BACK               4
#define AMOTION_EVENT_ACTION_DOWN   0
#define AMOTION_EVENT_ACTION_UP     1
#define AMOTION_EVENT_ACTION_MOVE   2
#define AMOTION_EVENT_ACTION_CANCEL 3
#define AMOTION_EVENT_ACTION_MASK   0xff

/* ---- native activity ---- */
struct ANativeActivity {
   struct ANativeActivityCallbacks *callbacks;
   JavaVM *vm;
   JNIEnv *env;
   jobject clazz; /* actually the NativeActivity instance, not its class */
   const char *internalDataPath, *externalDataPath;
   int32_t sdkVersion;
   void *instance, *assetManager;
   const char *obbPath;
};

void ANativeActivity_setWindowFlags(struct ANativeActivity *a, uint32_t add,
                                    uint32_t remove);
#define AWINDOW_FLAG_KEEP_SCREEN_ON 0x00000080

struct ANativeActivityCallbacks {
   void (*onStart)(struct ANativeActivity *);
   void (*onResume)(struct ANativeActivity *);
   void *(*onSaveInstanceState)(struct ANativeActivity *, size_t *);
   void (*onPause)(struct ANativeActivity *);
   void (*onStop)(struct ANativeActivity *);
   void (*onDestroy)(struct ANativeActivity *);
   void (*onWindowFocusChanged)(struct ANativeActivity *, int);
   void (*onNativeWindowCreated)(struct ANativeActivity *,
                                 struct ANativeWindow *);
   void (*onNativeWindowResized)(struct ANativeActivity *,
                                 struct ANativeWindow *);
   void (*onNativeWindowRedrawNeeded)(struct ANativeActivity *,
                                      struct ANativeWindow *);
   void (*onNativeWindowDestroyed)(struct ANativeActivity *,
                                   struct ANativeWindow *);
   void (*onInputQueueCreated)(struct ANativeActivity *, struct AInputQueue *);
   void (*onInputQueueDestroyed)(struct ANativeActivity *,
                                 struct AInputQueue *);
   void (*onContentRectChanged)(struct ANativeActivity *, const struct ARect *);
   void (*onConfigurationChanged)(struct ANativeActivity *);
   void (*onLowMemory)(struct ANativeActivity *);
};

#endif
