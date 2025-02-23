/****************************************************************************
 *
 *   Copyright (c) 2020 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file usr_hrt.c
 *
 * Userspace High-resolution timer callouts and timekeeping.
 *
 * This can be used with nuttx userspace
 *
 */

#ifndef MODULE_NAME
#define MODULE_NAME "usr_hrt"
#endif

#include <px4_platform_common/px4_config.h>
#include <px4_platform_common/defines.h>
#include <px4_platform_common/log.h>
#include <px4_platform_common/posix.h>
#include <px4_platform_common/shutdown.h>

#include <sys/ioctl.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sched.h>
#include <errno.h>

#include <assert.h>
#include <debug.h>

#include <board_config.h>
#include <drivers/drv_hrt.h>
#include <sys/boardctl.h>

static px4_task_t g_usr_hrt_task = -1;
static px4_hrt_handle_t g_hrt_client_handle;
static px4_sem_t g_worker_lock;

#ifdef PX4_USERSPACE_HRT
static uintptr_t g_abstime_base;
#endif

/**
 * Wrapper for atexit()
 */
static void hrt_stop(void)
{
	if (g_usr_hrt_task >= 0) {
		px4_task_delete(g_usr_hrt_task);
	}

	px4_sem_destroy(&g_worker_lock);
	boardctl(HRT_UNREGISTER, (uintptr_t)&g_hrt_client_handle);
}

/**
 * Event dispatcher thread
 */
static int event_thread(int argc, char *argv[])
{
	struct hrt_boardctl ioc_parm {
		.handle = g_hrt_client_handle,
		.entry = nullptr,
		.time = 0,
		.interval = 0,
		.callout = nullptr,
		.arg = nullptr
	};

	while (1) {
		/* Wait for hrt tick */
		boardctl(HRT_WAITEVENT, (uintptr_t)&ioc_parm);

		/* HRT event received, dispatch */
		if (ioc_parm.callout) {
			ioc_parm.callout(ioc_parm.arg);
		}
	}

	return 0;
}

static void start_worker(void)
{
	if (g_usr_hrt_task >= 0) {
		// Worker is already (for sure) running, get out
		return;
	}

	// Ensure only a single thread gets to create the worker, the rest will wait
	px4_sem_wait(&g_worker_lock);

	if (g_usr_hrt_task < 0) {
		g_usr_hrt_task = px4_task_spawn_cmd("usr_hrt", SCHED_DEFAULT, SCHED_PRIORITY_MAX,
						    PX4_STACK_ADJUSTED(1024), event_thread, NULL);
	}

	px4_sem_post(&g_worker_lock);
}

/**
 * Fetch a never-wrapping absolute time value in microseconds from
 * some arbitrary epoch shortly after system start.
 */
hrt_abstime
hrt_absolute_time(void)
{
#ifndef PX4_USERSPACE_HRT
	hrt_abstime abstime = 0;
	boardctl(HRT_ABSOLUTE_TIME, (uintptr_t)&abstime);
	return abstime;
#else

	if (g_abstime_base)	{
		return getreg64(g_abstime_base);

	} else {
		PX4_ERR("g_abstime_base is NULL\n");
		return 0;
	}

#endif
}

/**
 * Request stop.
 */
bool hrt_request_stop()
{
	hrt_stop();
	return true;
}

/**
 * Initialise the high-resolution timing module.
 */
void
hrt_init(void)
{
	boardctl(HRT_REGISTER, (uintptr_t)&g_hrt_client_handle);
#ifdef PX4_USERSPACE_HRT
	boardctl(HRT_ABSTIME_BASE, (uintptr_t)&g_abstime_base);
#endif

	if (g_hrt_client_handle) {
		atexit(hrt_stop);
		px4_sem_init(&g_worker_lock, 0, 1);
	}
}

/**
 * Call callout(arg) after interval has elapsed.
 */
void
hrt_call_after(struct hrt_call *entry, hrt_abstime delay, hrt_callout callout, void *arg)
{
	struct hrt_boardctl ioc_parm {
		.handle = g_hrt_client_handle,
		.entry = entry,
		.time = delay,
		.interval = 0,
		.callout = callout,
		.arg = arg
	};

	start_worker();
	boardctl(HRT_CALL_AFTER, (uintptr_t)&ioc_parm);
}

/**
 * Call callout(arg) at calltime.
 */
void
hrt_call_at(struct hrt_call *entry, hrt_abstime calltime, hrt_callout callout, void *arg)
{
	hrt_boardctl_t ioc_parm{
		.handle = g_hrt_client_handle,
		.entry = entry,
		.time = calltime,
		.interval = 0,
		.callout = callout,
		.arg = arg
	};

	start_worker();
	boardctl(HRT_CALL_AT, (uintptr_t)&ioc_parm);
}

/**
 * Call callout(arg) every period.
 */
void
hrt_call_every(struct hrt_call *entry, hrt_abstime delay, hrt_abstime interval, hrt_callout callout, void *arg)
{
	hrt_boardctl_t ioc_parm {
		.handle = g_hrt_client_handle,
		.entry = entry,
		.time = delay,
		.interval = interval,
		.callout = callout,
		.arg = arg,
	};

	start_worker();
	boardctl(HRT_CALL_EVERY, (uintptr_t)&ioc_parm);
}

/**
 * Remove the entry from the callout list.
 */
void
hrt_cancel(struct hrt_call *entry)
{
	hrt_boardctl_t ioc_parm {
		.handle = g_hrt_client_handle,
		.entry = entry,
	};

	start_worker();
	boardctl(HRT_CANCEL, (uintptr_t)&ioc_parm);
}

void
hrt_call_init(struct hrt_call *entry)
{
	memset(entry, 0, sizeof(*entry));
}

/**
 * If this returns true, the call has been invoked and removed from the callout list.
 *
 * Always returns false for repeating callouts.
 */
bool
hrt_called(struct hrt_call *entry)
{
	return (entry->deadline == 0);
}

latency_info_t
get_latency(uint16_t bucket_idx, uint16_t counter_idx)
{
	latency_boardctl_t latency_ioc;
	latency_ioc.bucket_idx = bucket_idx;
	latency_ioc.counter_idx = counter_idx;
	latency_ioc.latency = {0, 0};
	boardctl(HRT_GET_LATENCY, (uintptr_t)&latency_ioc);
	return latency_ioc.latency;
}

void reset_latency_counters()
{
	boardctl(HRT_RESET_LATENCY, NULL);
}
