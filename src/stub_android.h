// SPDX-License-Identifier: GPL-3.0
// stub_android.h --- libandroid.so stub declarations
// Copyright 2026 Jakob Kastelic

/* Declarations for the symbols defined in stub_android.c. These are the ABI the
 * stub libandroid.so exports; the phone's real libandroid binds them at
 * runtime. */
#ifndef STUB_ANDROID_H
#define STUB_ANDROID_H

int ANativeWindow_setBuffersGeometry(void *w, int a, int b, int c);
int ANativeWindow_lock(void *w, void *buf, void *rect);
int ANativeWindow_unlockAndPost(void *w);
void *ALooper_forThread(void);
void AInputQueue_attachLooper(void *q, void *l, int i, void *cb, void *d);
void AInputQueue_detachLooper(void *q);
int AInputQueue_getEvent(void *q, void **ev);
int AInputQueue_preDispatchEvent(void *q, void *ev);
void AInputQueue_finishEvent(void *q, void *ev, int handled);
int AInputEvent_getType(const void *ev);
int AMotionEvent_getAction(const void *ev);
int AKeyEvent_getAction(const void *ev);
int AKeyEvent_getKeyCode(const void *ev);
float AMotionEvent_getX(const void *ev, unsigned long i);
float AMotionEvent_getY(const void *ev, unsigned long i);
unsigned long AMotionEvent_getHistorySize(const void *ev);
float AMotionEvent_getHistoricalX(const void *ev, unsigned long i,
                                  unsigned long h);
float AMotionEvent_getHistoricalY(const void *ev, unsigned long i,
                                  unsigned long h);
int ALooper_addFd(void *l, int fd, int ident, int events, void *cb, void *data);
void ANativeActivity_setWindowFlags(void *a, unsigned add, unsigned rm);

#endif
