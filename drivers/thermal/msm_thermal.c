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

#define THROTTLE_FREQUENCY 1267200
#define PANIC_THROTTLE_FREQUENCY 729600

static struct cpus {
	bool throttling;
	int thermal_steps[5];
} cpu_stats = {
	.throttling = false,
	.thermal_steps = {729600, 1190400, 1497600, 1728000, 1958400},
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

static int update_cpu_max_freq(int cpu, uint32_t max_freq)
{
	int ret = 0;

	ret = msm_cpufreq_set_freq_limits(cpu, MSM_CPUFREQ_NO_LIMIT, max_freq);
	if (ret)
		return ret;

	if (cpu_online(cpu)) {
		struct cpufreq_policy policy;
		
		ret = cpufreq_get_policy(&policy, cpu);

		if (ret)
			return ret;

		if (max_freq == MSM_CPUFREQ_NO_LIMIT)
			max_freq = policy.max;

		ret = cpufreq_driver_target(&policy, max_freq,
			CPUFREQ_RELATION_H);
	}

	pr_info("%s: Setting cpu%d max frequency to %d\n",
				KBUILD_MODNAME, cpu, max_freq);

	return ret;
}

static void limit_cpu_freqs(uint32_t max_freq)
{
	int ret;
    int cpu;
    
	/* Update new limits */
	for_each_possible_cpu(cpu) {
		ret = update_cpu_max_freq(cpu, max_freq);
		if (ret)
			pr_debug(
			"%s: Unable to limit cpu%d max freq to %d\n",
					KBUILD_MODNAME, cpu, max_freq);
	}
}

static void check_temp(struct work_struct *work)
{
	struct tsens_device tsens_dev;
	long temp = 0;
    
	tsens_dev.sensor_num = msm_thermal_info.sensor_id;
	tsens_get_temp(&tsens_dev, &temp);

	if (temp >= (temp_threshold + 25))
	{
		cpu_stats.throttling = true;
		limit_cpu_freqs(cpu_stats.thermal_steps[0]);
	}

	else if (temp >= (temp_threshold + 20))
	{
		cpu_stats.throttling = true;
		limit_cpu_freqs(cpu_stats.thermal_steps[1]);
	}
    
	else if (temp >= (temp_threshold + 15))
	{
		cpu_stats.throttling = true;
		limit_cpu_freqs(cpu_stats.thermal_steps[2]);
	}

	else if (temp >= (temp_threshold + 10))
	{
		cpu_stats.throttling = true;
		limit_cpu_freqs(cpu_stats.thermal_steps[3]);
	}

	else if (temp >= (temp_threshold + 5))
	{
		cpu_stats.throttling = true;
		limit_cpu_freqs(cpu_stats.thermal_steps[4]);
	}

	else if (temp <= temp_threshold)
	{
		if (cpu_stats.throttling)
		{
			limit_cpu_freqs(MSM_CPUFREQ_NO_LIMIT);
			cpu_stats.throttling = false;
		}
	}

	queue_delayed_work(wq, &check_temp_work, HZ);
}

int __devinit msm_thermal_init(struct msm_thermal_data *pdata)
{
	int ret = 0;
    
	BUG_ON(!pdata);
	BUG_ON(pdata->sensor_id >= TSENS_MAX_SENSORS);
	memcpy(&msm_thermal_info, pdata, sizeof(struct msm_thermal_data));
    
	wq = alloc_workqueue("msm_thermal_workqueue", WQ_FREEZABLE | WQ_UNBOUND, 0);
    
    if (!wq)
        return -ENOMEM;
    
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
