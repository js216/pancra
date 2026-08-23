// SPDX-License-Identifier: GPL-3.0
// ndkabi.h --- the ABI facts app/ndk.h claims, in a form the NDK can check
// Copyright 2026 Jakob Kastelic

/* WHY A HAND-WRITTEN BOUNDARY NEEDS A CHECKED ONE BESIDE IT.
 *
 * app/ndk.h mirrors a piece of the Android NDK by hand -- four structs, a
 * callback table of sixteen function pointers, twenty-odd prototypes and as
 * many numeric constants -- because the app is built freestanding and does not
 * ship the NDK. That mirror is an ABI CLAIM about somebody else's binary, and
 * a wrong claim does not fail to compile. It fails on the phone: a member
 * inserted into ANativeActivityCallbacks upstream moves every callback after
 * it, so onInputQueueCreated is installed in the slot the platform reads as
 * onContentRectChanged and the app takes a rectangle for a queue pointer. A
 * constant that drifts turns a MOVE into a CANCEL. A size_t argument written
 * as `int` truncates the pointer index on the day a second finger arrives.
 * Every one of those compiles cleanly, links cleanly, and is discovered by
 * whoever is wearing the sensor.
 *
 * HOW THIS FILE CLOSES THAT. It states the facts -- sizes, member offsets,
 * constant values, and the exact type of every function -- WITHOUT naming
 * either declaration set, so the same assertions can be compiled twice:
 *
 *   - against app/ndk.h, in every app translation unit, on every build;
 *   - against the OFFICIAL headers of a pinned NDK, by `make ndkcheck`
 *     (test/app/ndkofficial.c, tools/ndk pinned in tools/DEPENDENCIES).
 *
 * Both compilations must succeed. That is what makes "ABI-compatible with
 * <android/native_activity.h>" a thing the build knows rather than a sentence
 * in a comment: our mirror and Google's headers are held to one list of
 * numbers, and a difference between them is a compile error naming the member.
 *
 * WHAT IT CANNOT SEE, said plainly: the pinned NDK is not the phone. A device
 * whose libandroid.so disagrees with the NDK headers that describe it is
 * beyond anything a compiler can check, and so is a platform that changes the
 * ABI without changing the headers. What this removes is the drift between US
 * and the published contract, which is the part that is ours to get wrong.
 *
 * THE NUMBERS ARE THE NDK'S, not a preference: they were extracted from
 * r27c's own headers (see tools/DEPENDENCIES for the pin) rather than typed
 * from the struct definitions, because typing them from the definitions would
 * reproduce whatever mistake the definitions contain. */
#ifndef PANCRA_NDKABI_H
#define PANCRA_NDKABI_H

/* THE DECLARATIONS COME FIRST, and this file does not choose which. Including
 * it standalone would silently check nothing, so it refuses: app/ndk.h defines
 * this before including it, and test/app/ndkofficial.c defines it after
 * including <android/...>. */
#ifndef NDKABI_DECLS
#error "include the declarations first: app/ndk.h, or the official NDK headers"
#endif

#include <stddef.h>
#include <stdint.h>

/* ---- the framebuffer a frame is drawn into ----
 *
 * uidraw.c writes pixels at `bits` and steps by `stride`. Every number here
 * is one the renderer does arithmetic with. */
_Static_assert(sizeof(struct ANativeWindow_Buffer) == 48,
               "ANativeWindow_Buffer is 48 bytes on LP64");
_Static_assert(offsetof(struct ANativeWindow_Buffer, width) == 0, "width");
_Static_assert(offsetof(struct ANativeWindow_Buffer, height) == 4, "height");
_Static_assert(offsetof(struct ANativeWindow_Buffer, stride) == 8, "stride");
_Static_assert(offsetof(struct ANativeWindow_Buffer, format) == 12, "format");
_Static_assert(offsetof(struct ANativeWindow_Buffer, bits) == 16, "bits");
_Static_assert(offsetof(struct ANativeWindow_Buffer, reserved) == 24,
               "reserved");
_Static_assert(sizeof(((struct ANativeWindow_Buffer *)0)->width) == 4,
               "the geometry fields are int32_t, not int");

_Static_assert(sizeof(struct ARect) == 16, "ARect is four int32_t");
_Static_assert(offsetof(struct ARect, left) == 0, "left");
_Static_assert(offsetof(struct ARect, top) == 4, "top");
_Static_assert(offsetof(struct ARect, right) == 8, "right");
_Static_assert(offsetof(struct ARect, bottom) == 12, "bottom");

/* ---- the activity the platform hands us ----
 *
 * main.c reads internalDataPath (where every file this app owns lives), vm,
 * env and clazz (the whole Java bridge), and writes callbacks. An offset
 * wrong here is a wrong POINTER, read as a path or called as a method. */
_Static_assert(sizeof(struct ANativeActivity) == 80, "ANativeActivity");
_Static_assert(offsetof(struct ANativeActivity, callbacks) == 0, "callbacks");
_Static_assert(offsetof(struct ANativeActivity, vm) == 8, "vm");
_Static_assert(offsetof(struct ANativeActivity, env) == 16, "env");
_Static_assert(offsetof(struct ANativeActivity, clazz) == 24, "clazz");
_Static_assert(offsetof(struct ANativeActivity, internalDataPath) == 32,
               "internalDataPath");
_Static_assert(offsetof(struct ANativeActivity, externalDataPath) == 40,
               "externalDataPath");
_Static_assert(offsetof(struct ANativeActivity, sdkVersion) == 48,
               "sdkVersion");
_Static_assert(offsetof(struct ANativeActivity, instance) == 56, "instance");
_Static_assert(offsetof(struct ANativeActivity, assetManager) == 64,
               "assetManager");
_Static_assert(offsetof(struct ANativeActivity, obbPath) == 72, "obbPath");

/* THE CALLBACK TABLE, MEMBER BY MEMBER. The size alone would not catch the
 * failure that matters: a member REPLACED by another of the same width leaves
 * the table 128 bytes and hands the platform's calls to the wrong function.
 * Sixteen offsets, so a member inserted, removed or reordered anywhere names
 * itself. */
_Static_assert(sizeof(struct ANativeActivityCallbacks) == 128,
               "sixteen callbacks on LP64");
#define NDKABI_CB(m, off)                                                      \
   _Static_assert(offsetof(struct ANativeActivityCallbacks, m) == (off), #m)
NDKABI_CB(onStart, 0);
NDKABI_CB(onResume, 8);
NDKABI_CB(onSaveInstanceState, 16);
NDKABI_CB(onPause, 24);
NDKABI_CB(onStop, 32);
NDKABI_CB(onDestroy, 40);
NDKABI_CB(onWindowFocusChanged, 48);
NDKABI_CB(onNativeWindowCreated, 56);
NDKABI_CB(onNativeWindowResized, 64);
NDKABI_CB(onNativeWindowRedrawNeeded, 72);
NDKABI_CB(onNativeWindowDestroyed, 80);
NDKABI_CB(onInputQueueCreated, 88);
NDKABI_CB(onInputQueueDestroyed, 96);
NDKABI_CB(onContentRectChanged, 104);
NDKABI_CB(onConfigurationChanged, 112);
NDKABI_CB(onLowMemory, 120);
#undef NDKABI_CB

/* ---- the constants, which are values and not names ----
 *
 * A name that survives a value change is the quietest failure on this
 * boundary: the code still compiles, still reads well, and means something
 * else. */
_Static_assert(WINDOW_FORMAT_RGBA_8888 == 1, "the pixel format we ask for");
_Static_assert(ALOOPER_EVENT_INPUT == 1, "the looper event we register for");
_Static_assert(AINPUT_EVENT_TYPE_KEY == 1, "key events");
_Static_assert(AINPUT_EVENT_TYPE_MOTION == 2, "motion events");
_Static_assert(AKEY_EVENT_ACTION_DOWN == 0, "key down");
_Static_assert(AKEY_EVENT_ACTION_UP == 1, "key up");
_Static_assert(AKEYCODE_BACK == 4, "the BACK key, which navigates the app");
_Static_assert(AMOTION_EVENT_ACTION_DOWN == 0, "touch down");
_Static_assert(AMOTION_EVENT_ACTION_UP == 1, "touch up");
_Static_assert(AMOTION_EVENT_ACTION_MOVE == 2, "touch move");
_Static_assert(AMOTION_EVENT_ACTION_CANCEL == 3, "touch cancel");
_Static_assert(AMOTION_EVENT_ACTION_MASK == 0xff, "the action byte");
_Static_assert(AMOTION_EVENT_ACTION_POINTER_DOWN == 5, "second finger down");
_Static_assert(AMOTION_EVENT_ACTION_POINTER_UP == 6, "second finger up");
_Static_assert(AMOTION_EVENT_ACTION_POINTER_INDEX_MASK == 0xff00,
               "which finger the action is about");
_Static_assert(AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT == 8,
               "...and where in the word it sits");
_Static_assert(AWINDOW_FLAG_KEEP_SCREEN_ON == 0x00000080,
               "the flag that keeps a glucose display visible");

/* ---- the prototypes ----
 *
 * Offsets and constants leave the calls themselves unchecked, and a call is
 * where the truncation happens: AMotionEvent_getX takes a size_t index, and a
 * mirror that says `int` passes 32 bits where the callee reads 64.
 *
 * Each assignment below is a type check the compiler performs against
 * whichever declaration set is in scope; an incompatible prototype is an
 * error naming the function. `static inline` so it costs nothing and warns
 * about nothing when it is never called -- it exists to be COMPILED. */
static inline void ndkabi_prototypes(void)
{
   int32_t (*geom)(struct ANativeWindow *, int32_t, int32_t, int32_t) =
       ANativeWindow_setBuffersGeometry;
   int32_t (*lock)(struct ANativeWindow *, struct ANativeWindow_Buffer *,
                   struct ARect *)         = ANativeWindow_lock;
   int32_t (*post)(struct ANativeWindow *) = ANativeWindow_unlockAndPost;
   struct ALooper *(*forthread)(void)      = ALooper_forThread;
   int (*addfd)(struct ALooper *, int, int, int, int (*)(int, int, void *),
                void *)                    = ALooper_addFd;
   int (*rmfd)(struct ALooper *, int)      = ALooper_removeFd;
   void (*attach)(struct AInputQueue *, struct ALooper *, int,
                  int (*)(int, int, void *), void *) = AInputQueue_attachLooper;
   void (*detach)(struct AInputQueue *)              = AInputQueue_detachLooper;
   int32_t (*getev)(struct AInputQueue *, struct AInputEvent **) =
       AInputQueue_getEvent;
   int32_t (*predisp)(struct AInputQueue *, struct AInputEvent *) =
       AInputQueue_preDispatchEvent;
   void (*finish)(struct AInputQueue *, struct AInputEvent *, int) =
       AInputQueue_finishEvent;
   int32_t (*evtype)(const struct AInputEvent *)   = AInputEvent_getType;
   int32_t (*maction)(const struct AInputEvent *)  = AMotionEvent_getAction;
   float (*mx)(const struct AInputEvent *, size_t) = AMotionEvent_getX;
   float (*my)(const struct AInputEvent *, size_t) = AMotionEvent_getY;
   size_t (*mcount)(const struct AInputEvent *) = AMotionEvent_getPointerCount;
   int32_t (*mid)(const struct AInputEvent *, size_t) =
       AMotionEvent_getPointerId;
   size_t (*mhist)(const struct AInputEvent *) = AMotionEvent_getHistorySize;
   float (*mhx)(const struct AInputEvent *, size_t, size_t) =
       AMotionEvent_getHistoricalX;
   float (*mhy)(const struct AInputEvent *, size_t, size_t) =
       AMotionEvent_getHistoricalY;
   int32_t (*kaction)(const struct AInputEvent *) = AKeyEvent_getAction;
   int32_t (*kcode)(const struct AInputEvent *)   = AKeyEvent_getKeyCode;
   void (*wflags)(struct ANativeActivity *, uint32_t, uint32_t) =
       ANativeActivity_setWindowFlags;
   (void)geom;
   (void)lock;
   (void)post;
   (void)forthread;
   (void)addfd;
   (void)rmfd;
   (void)attach;
   (void)detach;
   (void)getev;
   (void)predisp;
   (void)finish;
   (void)evtype;
   (void)maction;
   (void)mx;
   (void)my;
   (void)mcount;
   (void)mid;
   (void)mhist;
   (void)mhx;
   (void)mhy;
   (void)kaction;
   (void)kcode;
   (void)wflags;
}

#endif
