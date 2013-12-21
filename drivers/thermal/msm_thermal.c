/* Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
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

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/msm_tsens.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/msm_tsens.h>
#include <linux/msm_thermal.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <mach/cpufreq.h>

static struct cpus {
	bool throttling;
	int thermal_steps[5];
	uint32_t limited_max_freq;
	unsigned int max_freq;
	struct cpufreq_policy policy;
} cpu_stats = {
	.throttling = false,
	.thermal_steps = {729600, 1190400, 1497600, 1728000},
	.limited_max_freq = UINT_MAX,
};

unsigned int temp_threshold = 70;
module_param(temp_threshold, int, 0755);

static struct msm_thermal_data msm_thermal_info;

static struct workqueue_struct *wq;
static struct delayed_work check_temp_work;

unsigned short get_threshold(void)
{
	return temp_threshold;
}

static int  msm_thermal_cpufreq_callback(struct notifier_block *nfb,
		unsigned long event, void *data)
{
	struct cpufreq_policy *policy = data;

	if (event != CPUFREQ_ADJUST)
		return 0;

	if (policy->max != cpu_stats.limited_max_freq)
		cpufreq_verify_within_limits(policy, policy->min,
				cpu_stats.limited_max_freq);

	return 0;
}

static struct notifier_block msm_thermal_cpufreq_notifier = {
	.notifier_call = msm_thermal_cpufreq_callback,
};

static void limit_cpu_freqs(uint32_t max_freq)
{
    int cpu;

	if (cpu_stats.limited_max_freq == max_freq)
		return;

	cpu_stats.limited_max_freq = max_freq;
    
	/* Update new limits */
	get_online_cpus();
	for_each_online_cpu(cpu)
	{
		cpufreq_update_policy(cpu);
		pr_info("%s: Setting cpu%d max frequency to %d\n",
				KBUILD_MODNAME, cpu, cpu_stats.limited_max_freq);
	}
	put_online_cpus();
}

void update_max_freq_buffer(void)
{
	if (!cpu_stats.throttling)
	{
		cpufreq_get_policy(&cpu_stats.policy, 0);
		cpu_stats.max_freq = cpu_stats.policy.max;
	}
}

static void check_temp(struct work_struct *work)
{
	struct tsens_device tsens_dev;
	long temp = 0;
    
	tsens_dev.sensor_num = msm_thermal_info.sensor_id;
	tsens_get_temp(&tsens_dev, &temp);

	/* most of the time the device is not hot so reschedule early */
	if (likely(temp < temp_threshold))
	{
		if (unlikely(cpu_stats.throttling))
		{
			limit_cpu_freqs(cpu_stats.max_freq);
			cpu_stats.throttling = false;
		}

		goto reschedule;
	}

	/* 
	 * Get the max freq before we throttle so that we can re-apply it after
	 * the device cools down
	 */
	update_max_freq_buffer();

	if (temp >= (temp_threshold + 20))
		limit_cpu_freqs(cpu_stats.thermal_steps[0]);

	else if (temp >= (temp_threshold + 15))
		limit_cpu_freqs(cpu_stats.thermal_steps[1]);
    
	else if (temp >= (temp_threshold + 10))
		limit_cpu_freqs(cpu_stats.thermal_steps[2]);

	else if (temp >= temp_threshold)
		limit_cpu_freqs(cpu_stats.thermal_steps[3]);

	cpu_stats.throttling = true;

reschedule:
	queue_delayed_work(wq, &check_temp_work, HZ);
}

int __devinit msm_thermal_init(struct msm_thermal_data *pdata)
{
	int ret = 0;
    
	BUG_ON(!pdata);
	BUG_ON(pdata->sensor_id >= TSENS_MAX_SENSORS);
	memcpy(&msm_thermal_info, pdata, sizeof(struct msm_thermal_data));
    
	wq = alloc_workqueue("msm_thermal_workqueue", WQ_UNBOUND, 0);
    
    if (!wq)
        return -ENOMEM;

	cpufreq_register_notifier(&msm_thermal_cpufreq_notifier,
			CPUFREQ_POLICY_NOTIFIER);
    
	INIT_DELAYED_WORK(&check_temp_work, check_temp);
	queue_delayed_work(wq, &check_temp_work, HZ*30);
    
	return ret;
}

static int __devinit msm_thermal_dev_probe(struct platform_device *pdev)
{
	int ret = 0;
	char *key = NULL;
	struct device_node *node = pdev->dev.of_node;
	struct msm_thermal_data data;
    
	memset(&data, 0, sizeof(struct msm_thermal_data));
	key = "qcom,sensor-id";
	ret = of_property_read_u32(node, key, &data.sensor_id);
	if (ret)
		goto fail;
	WARN_ON(data.sensor_id >= TSENS_MAX_SENSORS);
    
fail:
	if (ret)
		pr_err("%s: Failed reading node=%s, key=%s\n",
		       __func__, node->full_name, key);
	else
		ret = msm_thermal_init(&data);
    
	return ret;
}

static struct of_device_id msm_thermal_match_table[] = {
	{.compatible = "qcom,msm-thermal"},
	{},
};

static struct platform_driver msm_thermal_device_driver = {
	.probe = msm_thermal_dev_probe,
	.driver = {
		.name = "msm-thermal",
		.owner = THIS_MODULE,
		.of_match_table = msm_thermal_match_table,
	},
};

int __init msm_thermal_device_init(void)
{
	return platform_driver_register(&msm_thermal_device_driver);
}
