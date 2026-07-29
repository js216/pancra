// SPDX-License-Identifier: GPL-3.0
// stub_android.c --- Link-time stub for libandroid.so
// Copyright 2026 Jakob Kastelic

/* Link-time stub for the device's /system/lib64/libandroid.so.
 * Only the symbol names matter; the real implementations are bound
 * by the dynamic linker on the phone.
 */
#include "stub_android.h"

int ANativeWindow_setBuffersGeometry(void *w, int a, int b, int c)
{
   (void)w;
   (void)a;
   (void)b;
   (void)c;
   return 0;
}

int ANativeWindow_lock(void *w, void *buf, void *rect)
{
   (void)w;
   (void)buf;
   (void)rect;
   return 0;
}

int ANativeWindow_unlockAndPost(void *w)
{
   (void)w;
   return 0;
}

void *ALooper_forThread(void)
{
   return 0;
}

void AInputQueue_attachLooper(void *q, void *l, int i, void *cb, void *d)
{
   (void)q;
   (void)l;
   (void)i;
   (void)cb;
   (void)d;
}

void AInputQueue_detachLooper(void *q)
{
   (void)q;
}

int AInputQueue_getEvent(void *q, void **ev)
{
   (void)q;
   (void)ev;
   return 0;
}

int AInputQueue_preDispatchEvent(void *q, void *ev)
{
   (void)q;
   (void)ev;
   return 0;
}

void AInputQueue_finishEvent(void *q, void *ev, int handled)
{
   (void)q;
   (void)ev;
   (void)handled;
}

int AInputEvent_getType(const void *ev)
{
   (void)ev;
   return 0;
}

int AMotionEvent_getAction(const void *ev)
{
   (void)ev;
   return 0;
}

int AKeyEvent_getAction(const void *ev)
{
   (void)ev;
   return 0;
}

int AKeyEvent_getKeyCode(const void *ev)
{
   (void)ev;
   return 0;
}

float AMotionEvent_getX(const void *ev, unsigned long i)
{
   (void)ev;
   (void)i;
   return 0;
}

float AMotionEvent_getY(const void *ev, unsigned long i)
{
   (void)ev;
   (void)i;
   return 0;
}

unsigned long AMotionEvent_getHistorySize(const void *ev)
{
   (void)ev;
   return 0;
}

float AMotionEvent_getHistoricalX(const void *ev, unsigned long i,
                                  unsigned long h)
{
   (void)ev;
   (void)i;
   (void)h;
   return 0;
}

float AMotionEvent_getHistoricalY(const void *ev, unsigned long i,
                                  unsigned long h)
{
   (void)ev;
   (void)i;
   (void)h;
   return 0;
}

int ALooper_addFd(void *l, int fd, int ident, int events, void *cb, void *data)
{
   (void)l;
   (void)fd;
   (void)ident;
   (void)events;
   (void)cb;
   (void)data;
   return 1;
}

int ALooper_removeFd(void *l, int fd)
{
   (void)l;
   (void)fd;
   return 1;
}

void ANativeActivity_setWindowFlags(void *a, unsigned add, unsigned rm)
{
   (void)a;
   (void)add;
   (void)rm;
}
