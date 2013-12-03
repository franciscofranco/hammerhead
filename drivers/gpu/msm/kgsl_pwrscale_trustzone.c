/* Copyright (c) 2010-2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/spinlock.h>
#include <mach/socinfo.h>
#include <mach/scm.h>
#include <linux/module.h>
#include <linux/jiffies.h>

#include "kgsl.h"
#include "kgsl_pwrscale.h"
#include "kgsl_device.h"

#define TZ_GOVERNOR_PERFORMANCE 0
#define TZ_GOVERNOR_ONDEMAND    1
#define TZ_GOVERNOR_INTERACTIVE	2

struct tz_priv {
	int governor;
	struct kgsl_power_stats bin;
	unsigned int idle_dcvs;
};
spinlock_t tz_lock;

/* FLOOR is 5msec to capture up to 3 re-draws
 * per frame for 60fps content.
 */
#define FLOOR			5000
/* CEILING is 50msec, larger than any standard
 * frame length, but less than the idle timer.
 */
#define CEILING			50000
#define TZ_RESET_ID		0x3
#define TZ_UPDATE_ID		0x4
#define TZ_INIT_ID		0x6

unsigned long window_time = 0;
unsigned long sample_time_ms = 100;
unsigned int up_threshold = 60;
unsigned int down_threshold = 25;
unsigned int up_differential = 10;
bool debug = 0;
unsigned long gpu_pref_counter;

module_param(sample_time_ms, long, 0664);
module_param(up_threshold, int, 0664);
module_param(down_threshold, int, 0664);
module_param(debug, bool, 0664);

static struct clk_scaling_stats {
	unsigned long total_time_ms;
	unsigned long busy_time_ms;
	unsigned long threshold;
} gpu_stats = {
	.total_time_ms = 0,
	.busy_time_ms = 0,
	.threshold = 0,
};

static ssize_t tz_governor_show(struct kgsl_device *device,
				struct kgsl_pwrscale *pwrscale,
				char *buf)
{
	struct tz_priv *priv = pwrscale->priv;
	int ret;

	if (priv->governor == TZ_GOVERNOR_ONDEMAND)
		ret = snprintf(buf, 10, "ondemand\n");
	else if (priv->governor == TZ_GOVERNOR_INTERACTIVE)
		ret = snprintf(buf, 13, "interactive\n");
	else
		ret = snprintf(buf, 13, "performance\n");

	return ret;
}

static ssize_t tz_governor_store(struct kgsl_device *device,
				struct kgsl_pwrscale *pwrscale,
				 const char *buf, size_t count)
{
	char str[20];
	struct tz_priv *priv = pwrscale->priv;
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;
	int ret;

	ret = sscanf(buf, "%20s", str);
	if (ret != 1)
		return -EINVAL;

	mutex_lock(&device->mutex);

	if (!strncmp(str, "ondemand", 8))
		priv->governor = TZ_GOVERNOR_ONDEMAND;
	else if (!strncmp(str, "interactive", 11))
		priv->governor = TZ_GOVERNOR_INTERACTIVE;
	else if (!strncmp(str, "performance", 11))
		priv->governor = TZ_GOVERNOR_PERFORMANCE;

	if (priv->governor == TZ_GOVERNOR_PERFORMANCE) {
		kgsl_pwrctrl_pwrlevel_change(device, pwr->max_pwrlevel);
		pwr->default_pwrlevel = pwr->max_pwrlevel;
	} else {
		pwr->default_pwrlevel = pwr->init_pwrlevel;
	}

	mutex_unlock(&device->mutex);
	return count;
}

PWRSCALE_POLICY_ATTR(governor, 0644, tz_governor_show, tz_governor_store);

static struct attribute *tz_attrs[] = {
	&policy_attr_governor.attr,
	NULL
};

static struct attribute_group tz_attr_group = {
	.attrs = tz_attrs,
};

static void tz_wake(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale)
{
	return;
}

static void tz_idle(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale)
{
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;
	struct tz_priv *priv = pwrscale->priv;
	struct kgsl_power_stats stats;

	/* In "performance" mode the clock speed always stays
	   the same */
	if (priv->governor == TZ_GOVERNOR_PERFORMANCE)
		return;

	device->ftbl->power_stats(device, &stats);
	priv->bin.total_time += stats.total_time;
	priv->bin.busy_time += stats.busy_time;

	if (time_is_after_jiffies(window_time + msecs_to_jiffies(sample_time_ms)))
		return;

	gpu_stats.total_time_ms = jiffies_to_msecs((long)jiffies - (long)window_time);
	do_div(priv->bin.busy_time, USEC_PER_MSEC);
	gpu_stats.busy_time_ms = priv->bin.busy_time;

	/* if busy time is 0 and we're already on the lowest pwrlevel, bail early */
	if (!gpu_stats.busy_time_ms && pwr->active_pwrlevel == pwr->min_pwrlevel)
		return;

	if (debug)
	{ 
		pr_info("GPU current load: %ld\n", gpu_stats.busy_time_ms);
		pr_info("GPU total time load: %ld\n", gpu_stats.total_time_ms);
		pr_info("GPU frequency: %d\n", 
								pwr->pwrlevels[pwr->active_pwrlevel].gpu_freq);
	}

	/*
	 * The below table is from Mako's Adreno 320 - needs update to match
	 * the Adreno 330 just for the sake of the comment itself
	 * 
	 * Scale the up_threshold value based on the active_pwrlevel. We have
	 * 4 different levels:
	 * 3 = 128MHz
	 * 2 = 200MHz
	 * 1 = 320MHz
	 * 0 = 400MHz
	 *
	 * Making the up_threshold value lower if the active level is 2 or 3 will
	 * possibly improve smoothness while scrolling or open applications with
	 * a lot of images and what not. With a Full HD panel like Flo/Deb I could
	 * notice a few frame drops while this algorithm didn't scale past 128MHz
	 * on simple operations. This is fixed with up_threshold being scaled
	 */

	if (pwr->active_pwrlevel == pwr->min_pwrlevel)
		gpu_stats.threshold = up_threshold / pwr->active_pwrlevel;
	else if (pwr->active_pwrlevel > 0)
		gpu_stats.threshold = up_threshold - up_differential;
	else
		gpu_stats.threshold = up_threshold;

	if ((gpu_stats.busy_time_ms * 100) > (gpu_stats.total_time_ms * gpu_stats.threshold))
	{
		if (gpu_pref_counter < 100)
			++gpu_pref_counter;

		if (pwr->active_pwrlevel > pwr->max_pwrlevel)
			kgsl_pwrctrl_pwrlevel_change(device,
					     pwr->active_pwrlevel - 1);
	}
	else if ((gpu_stats.busy_time_ms * 100) < (gpu_stats.total_time_ms * down_threshold))
	{
		if (gpu_pref_counter > 0)
			--gpu_pref_counter;

		if (pwr->active_pwrlevel < pwr->max_pwrlevel)
			kgsl_pwrctrl_pwrlevel_change(device,
					     pwr->active_pwrlevel + 1);
	}

	priv->bin.total_time = 0;
	priv->bin.busy_time = 0;
	window_time = jiffies;
}

static void tz_busy(struct kgsl_device *device,
	struct kgsl_pwrscale *pwrscale)
{
	device->on_time = ktime_to_us(ktime_get());
}

static void tz_sleep(struct kgsl_device *device,
	struct kgsl_pwrscale *pwrscale)
{
	struct tz_priv *priv = pwrscale->priv;
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;

	/*
	 * We don't want the GPU to go to sleep if the busy_time_ms calculated on
	 * idle routine is not below down_threshold. This is just a measure of
	 * precaution
	 */
	if ((gpu_stats.busy_time_ms * 100) < 
			(gpu_stats.total_time_ms * down_threshold))
	{
		kgsl_pwrctrl_pwrlevel_change(device, pwr->min_pwrlevel);
	}

	if (debug)
	{
		pr_info("GPU frequency: %d\n", 
								pwr->pwrlevels[pwr->active_pwrlevel].gpu_freq);
	}

	gpu_pref_counter = 0;
	priv->bin.total_time = 0;
	priv->bin.busy_time = 0;
	window_time = jiffies;
}

#ifdef CONFIG_MSM_SCM
static int tz_init(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale)
{
	int i = 0, j = 1, ret = 0;
	struct tz_priv *priv;
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;
	unsigned int tz_pwrlevels[KGSL_MAX_PWRLEVELS + 1];

	priv = pwrscale->priv = kzalloc(sizeof(struct tz_priv), GFP_KERNEL);
	if (pwrscale->priv == NULL)
		return -ENOMEM;
	priv->idle_dcvs = 0;
	priv->governor = TZ_GOVERNOR_INTERACTIVE;
	spin_lock_init(&tz_lock);
	kgsl_pwrscale_policy_add_files(device, pwrscale, &tz_attr_group);
	for (i = 0; i < pwr->num_pwrlevels - 1; i++) {
		if (i == 0)
			tz_pwrlevels[j] = pwr->pwrlevels[i].gpu_freq;
		else if (pwr->pwrlevels[i].gpu_freq !=
				pwr->pwrlevels[i - 1].gpu_freq) {
			j++;
			tz_pwrlevels[j] = pwr->pwrlevels[i].gpu_freq;
		}
	}
	tz_pwrlevels[0] = j;
	ret = scm_call(SCM_SVC_DCVS, TZ_INIT_ID, tz_pwrlevels,
				sizeof(tz_pwrlevels), NULL, 0);
	if (ret)
		priv->idle_dcvs = 1;
	return 0;
}
#else
static int tz_init(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale)
{
	return -EINVAL;
}
#endif /* CONFIG_MSM_SCM */

static void tz_close(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale)
{
	kgsl_pwrscale_policy_remove_files(device, pwrscale, &tz_attr_group);
	kfree(pwrscale->priv);
	pwrscale->priv = NULL;
}

struct kgsl_pwrscale_policy kgsl_pwrscale_policy_tz = {
	.name = "trustzone",
	.init = tz_init,
	.busy = tz_busy,
	.idle = tz_idle,
	.sleep = tz_sleep,
	.wake = tz_wake,
	.close = tz_close
};
EXPORT_SYMBOL(kgsl_pwrscale_policy_tz);
