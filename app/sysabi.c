// SPDX-License-Identifier: GPL-3.0
// sysabi.c --- the two timer operations (see sysabi.h)
// Copyright 2026 Jakob Kastelic

/* The whole implementation. Everything interesting about this boundary is in
 * the header and in app/sysabifacts.h; what is here is the arithmetic that
 * was being repeated at each call site, done once. */
#include "sysabi.h"

int sys_timer_open(void)
{
   /* MONOTONIC, and it is the entire reason this wrapper exists rather than
    * a bare call. A repaint timer on CLOCK_REALTIME stops firing for however
    * long the phone's clock jumps forward, and fires in a burst when it jumps
    * back -- and the clock DOES jump: the first NTP sync after a boot with no
    * network can move it by hours. */
   return timerfd_create(SYS_CLOCK_MONOTONIC, SYS_TFD_NONBLOCK);
}

int sys_timer_arm_ms(int fd, long first_ms, long repeat_ms)
{
   if (fd < 0)
      return 0;
   /* ZERO IS DISARM, to the kernel: an it_value of {0,0} stops the timer
    * whatever it_interval says. That is the caller's to mean, not this
    * function's to second-guess, so it is passed through. */
   struct itimerspec its;
   its.it_value.tv_sec     = first_ms / 1000;
   its.it_value.tv_nsec    = (first_ms % 1000) * 1000000L;
   its.it_interval.tv_sec  = repeat_ms / 1000;
   its.it_interval.tv_nsec = (repeat_ms % 1000) * 1000000L;
   return timerfd_settime(fd, 0, &its, 0) == 0;
}
