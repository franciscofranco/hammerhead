/*
 * Copyright (c) 2013-2014, Francisco Franco. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/notifier.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/jiffies.h>
#include <linux/input.h>
#include <linux/time.h>
#include <linux/slab.h>

#define MIN_TIME_INTERVAL_US (50 * USEC_PER_MSEC)

struct touchboost_inputopen {
	struct input_handle *handle;
	struct work_struct inputopen_work;
} touchboost_inputopen;

/*
 * Use this variable in your governor of choice to calculate when the cpufreq
 * core is allowed to ramp the cpu down after an input event. That logic is done
 * by you, this var only outputs the last time in us an event was captured
 */
static u64 last_input_time = 0;

inline u64 get_input_time(void)
{
	return last_input_time;
}

static void boost_input_event(struct input_handle *handle,
                unsigned int type, unsigned int code, int value)
{
	u64 now;

	if ((type == EV_ABS)) {
		now = ktime_to_us(ktime_get());

		if (now - last_input_time < MIN_TIME_INTERVAL_US)
			return;

		last_input_time = ktime_to_us(ktime_get());
	}
}

static int boost_input_connect(struct input_handler *handler,
                struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (handle == NULL)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = handler->name;

	error = input_register_handle(handle);
	if (error)
		goto err;

	error = input_open_device(handle);
        if (error) {
                input_unregister_handle(handle);
		goto err;
	}

	return 0;

err:
	kfree(handle);
	return error;
}

static void boost_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id boost_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		/* assumption: MT_.._X & MT_.._Y are in the same long */
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
				BIT_MASK(ABS_MT_POSITION_X) |
				BIT_MASK(ABS_MT_POSITION_Y) },
	},
	{ },
};

static struct input_handler boost_input_handler = {
	.event          = boost_input_event,
	.connect        = boost_input_connect,
	.disconnect     = boost_input_disconnect,
	.name           = "input-boost",
	.id_table       = boost_ids,
};

static int __init init(void)
{
	if (input_register_handler(&boost_input_handler))
		pr_info("Unable to register the input handler\n");

	return 0;
}
late_initcall(init);
