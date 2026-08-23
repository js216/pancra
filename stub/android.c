// SPDX-License-Identifier: GPL-3.0
// stub_android.c --- Link-time stub for libandroid.so
// Copyright 2026 Jakob Kastelic

/* Link-time stub for the device's /system/lib64/libandroid.so. Only the
 * symbol NAMES matter: the app is linked against this so `-Wl,--no-undefined`
 * can prove nothing is missing, and the dynamic linker binds the real
 * implementations on the phone. No line of this file ever runs there.
 *
 * TYPED THROUGH ndk.h, not through a second hand-written mirror. Declaring
 * its own `int ANativeWindow_lock(void *, void *, void *)` and friends --
 * the same symbols the app calls, spelled differently, in a header no
 * translation unit includes beside ndk.h -- leaves the compiler unable to see
 * the two disagreeing, which is exactly the failure the whole
 * ndkabi.h check exists to prevent, in our own tree. There is one declaration
 * set now, and it is the one production calls through.
 *
 * MOCK EVENT ACCESSORS ARE NOT HERE: returning one finger at the origin so
 * the host can link input.c is a TEST fixture, and it lives in
 * test/app/androidmock.c. A stub that ships in the build should not be
 * answering questions. */
/* EXPORTED: these are link-time stand-ins for the device's own
 * libraries, and a hidden symbol is not one the linker can stand in for. */
#pragma GCC visibility push(default)

#include "ndk.h"
#include <stdint.h>

int32_t ANativeWindow_setBuffersGeometry(struct ANativeWindow *w, int32_t width,
                                         int32_t height, int32_t format)
{
   (void)w;
   (void)width;
   (void)height;
   (void)format;
   return 0;
}

int32_t ANativeWindow_lock(struct ANativeWindow *w,
                           struct ANativeWindow_Buffer *out,
                           struct ARect *dirty)
{
   (void)w;
   (void)out;
   (void)dirty;
   return 0;
}

int32_t ANativeWindow_unlockAndPost(struct ANativeWindow *w)
{
   (void)w;
   return 0;
}

struct ALooper *ALooper_forThread(void)
{
   return 0;
}

int ALooper_addFd(struct ALooper *l, int fd, int ident, int events,
                  int (*cb)(int fd, int events, void *data), void *data)
{
   (void)l;
   (void)fd;
   (void)ident;
   (void)events;
   (void)cb;
   (void)data;
   return 1;
}

int ALooper_removeFd(struct ALooper *l, int fd)
{
   (void)l;
   (void)fd;
   return 1;
}

void AInputQueue_attachLooper(struct AInputQueue *q, struct ALooper *l,
                              int ident,
                              int (*cb)(int fd, int events, void *data),
                              void *data)
{
   (void)q;
   (void)l;
   (void)ident;
   (void)cb;
   (void)data;
}

void AInputQueue_detachLooper(struct AInputQueue *q)
{
   (void)q;
}

int32_t AInputQueue_getEvent(struct AInputQueue *q, struct AInputEvent **ev)
{
   (void)q;
   (void)ev;
   return 0;
}

int32_t AInputQueue_preDispatchEvent(struct AInputQueue *q,
                                     struct AInputEvent *ev)
{
   (void)q;
   (void)ev;
   return 0;
}

void AInputQueue_finishEvent(struct AInputQueue *q, struct AInputEvent *ev,
                             int handled)
{
   (void)q;
   (void)ev;
   (void)handled;
}

int32_t AInputEvent_getType(const struct AInputEvent *ev)
{
   (void)ev;
   return 0;
}

int32_t AMotionEvent_getAction(const struct AInputEvent *ev)
{
   (void)ev;
   return 0;
}

float AMotionEvent_getX(const struct AInputEvent *ev, unsigned long idx)
{
   (void)ev;
   (void)idx;
   return 0;
}

float AMotionEvent_getY(const struct AInputEvent *ev, unsigned long idx)
{
   (void)ev;
   (void)idx;
   return 0;
}

unsigned long AMotionEvent_getPointerCount(const struct AInputEvent *ev)
{
   (void)ev;
   return 0;
}

int32_t AMotionEvent_getPointerId(const struct AInputEvent *ev,
                                  unsigned long idx)
{
   (void)ev;
   (void)idx;
   return 0;
}

unsigned long AMotionEvent_getHistorySize(const struct AInputEvent *ev)
{
   (void)ev;
   return 0;
}

float AMotionEvent_getHistoricalX(const struct AInputEvent *ev,
                                  unsigned long idx, unsigned long h)
{
   (void)ev;
   (void)idx;
   (void)h;
   return 0;
}

float AMotionEvent_getHistoricalY(const struct AInputEvent *ev,
                                  unsigned long idx, unsigned long h)
{
   (void)ev;
   (void)idx;
   (void)h;
   return 0;
}

int32_t AKeyEvent_getAction(const struct AInputEvent *ev)
{
   (void)ev;
   return 0;
}

int32_t AKeyEvent_getKeyCode(const struct AInputEvent *ev)
{
   (void)ev;
   return 0;
}

void ANativeActivity_setWindowFlags(struct ANativeActivity *a, uint32_t add,
                                    uint32_t remove)
{
   (void)a;
   (void)add;
   (void)remove;
}

#pragma GCC visibility pop
