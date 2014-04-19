/*
 * Copyright (c) 2013, Francisco Franco <franciscofranco.1990@gmail.com>.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Simple no bullshit hot[un]plug driver for SMP
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/platform_device.h>
#include <linux/timer.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/lcd_notify.h>

#define MAKO_HOTPLUG "mako_hotplug"

#define DEFAULT_LOAD_THRESHOLD 60
#define DEFAULT_HIGH_LOAD_COUNTER 10
#define DEFAULT_MAX_LOAD_COUNTER 20
#define DEFAULT_CPUFREQ_UNPLUG_LIMIT 1000000
#define DEFAULT_MIN_TIME_CPU_ONLINE 1
#define DEFAULT_TIMER 1

#define MIN_CPU_UP_US 1000 * USEC_PER_MSEC;
#define NUM_POSSIBLE_CPUS num_possible_cpus()

extern bool boosted;

static struct cpu_stats
{
	unsigned int counter[2];
	u64 timestamp[2];
	struct notifier_block notif;
} stats = {
	.counter = {0},
};

struct hotplug_tunables
{
	/*
	 * system load threshold to decide when online or offline cores
	 * from 0 to 100
	 */
	unsigned int load_threshold;

	/*
	 * counter to filter online/offline calls. The load needs to be above
	 * load_threshold X high_load_counter times for the cores to go online
	 * otherwise they stay offline
	 */
	unsigned int high_load_counter;

	/*
	 * max number of samples counters allowed to be counted. The higher the
	 * value the longer it will take the driver to offline cores after a period
	 * of high and continuous load
	 */
	unsigned int max_load_counter;

	/*
	 * if the current CPU freq is above this limit don't offline the cores
	 * for a couple of extra samples
	 */
	unsigned int cpufreq_unplug_limit;

	/*
	 * minimum time in seconds that a core stays online to avoid too many
	 * online/offline calls
	 */
	unsigned int min_time_cpu_online;

	/*
	 * sample timer in seconds. The default value of 1 equals to 10 samples
	 * every second. The higher the value the less samples per second it runs
	 */
	unsigned int timer;
} tunables;

static struct workqueue_struct *wq;
static struct delayed_work decide_hotplug;
static struct work_struct suspend, resume;

static void cpu_revive(unsigned int cpu)
{
	cpu_up(cpu);
	stats.timestamp[cpu - 2] = ktime_to_us(ktime_get());
}

static void cpu_smash(unsigned int cpu)
{
	struct hotplug_tunables *t = &tunables;
	u64 extra_time = MIN_CPU_UP_US;

	/*
	 * Let's not unplug this cpu unless its been online for longer than
	 * 1sec to avoid consecutive ups and downs if the load is varying
	 * closer to the threshold point.
	 */
	if (unlikely(t->min_time_cpu_online > 1))
		extra_time = t->min_time_cpu_online * MIN_CPU_UP_US;

	if (ktime_to_us(ktime_get()) < stats.timestamp[cpu - 2] + extra_time)
		return;

	cpu_down(cpu);
	stats.counter[cpu - 2] = 0;
}

static void __ref decide_hotplug_func(struct work_struct *work)
{
	unsigned int cpu, cpu_nr;
	unsigned int cur_load;
	unsigned int freq_buf;
	unsigned int nr_online_cpus = num_online_cpus();
	struct cpufreq_policy policy;
	struct hotplug_tunables *t = &tunables;

	/*
	 * reschedule early when the system has woken up from the FREEZER but the
	 * display is not on
	 */
	if (unlikely(nr_online_cpus == 1))
		goto reschedule;

	/*
	 * reschedule early when the user doesn't want more than 2 cores online
	 */
	if (unlikely(t->load_threshold == 100 && nr_online_cpus == 2))
		goto reschedule;

	/*
	 * reschedule early when users to run with all cores online
	 */
	if (unlikely(!t->load_threshold && nr_online_cpus == NUM_POSSIBLE_CPUS))
		goto reschedule;

	for (cpu = 0, cpu_nr = 2; cpu < 2; cpu++, cpu_nr++)
	{
		/*
		 * just in case there's a race between screen on and this thread and
		 * cpu1 is still waking up
		 */
		if (cpu && cpu_is_offline(cpu))
			goto reschedule;

		cur_load = cpufreq_quick_get_util(cpu);

		if (cur_load >= t->load_threshold)
		{
			if (likely(stats.counter[cpu] < t->max_load_counter))
				stats.counter[cpu] += 2;

			if (cpu_is_offline(cpu_nr)
					&& stats.counter[cpu] >= t->high_load_counter)
				cpu_revive(cpu_nr);
			}

		else
		{
			if (stats.counter[cpu])
				--stats.counter[cpu];

			if (cpu_online(cpu_nr) && stats.counter[cpu] < t->high_load_counter)
			{
				/*
				 * offline the cpu only if its freq is lower than
				 * CPUFREQ_UNPLUG_LIMIT. Else fill the counter so that this cpu
				 * stays online at least 5 more samples (time depends on the
				 * sample timer period)
				 */
				cpufreq_get_policy(&policy, cpu_nr);

				freq_buf = policy.min;

				if (policy.min > t->cpufreq_unplug_limit)
					freq_buf = t->cpufreq_unplug_limit;

				if (policy.cur > freq_buf && !boosted)
					stats.counter[cpu] = t->high_load_counter + 5;
				else
					cpu_smash(cpu_nr);
			}
		}
	}

reschedule:
	queue_delayed_work_on(0, wq, &decide_hotplug,
		msecs_to_jiffies(t->timer * HZ));
}

static void mako_hotplug_suspend(struct work_struct *work)
{
	int cpu;

	stats.counter[0] = 0;
	stats.counter[1] = 0;

	for_each_online_cpu(cpu)
	{
		if (!cpu)
			continue;

		cpu_down(cpu);
	}

	pr_info("%s: suspend\n", MAKO_HOTPLUG);
}

static void __ref mako_hotplug_resume(struct work_struct *work)
{
	int cpu = 1;

	if (cpu_is_offline(cpu))
		cpu_up(cpu);

	pr_info("%s: resume\n", MAKO_HOTPLUG);
}

static int lcd_notifier_callback(struct notifier_block *this,
	unsigned long event, void *data)
{
	if (event == LCD_EVENT_ON_START)
		queue_work_on(0, wq, &resume);
	else if (event == LCD_EVENT_OFF_START)
		queue_work_on(0, wq, &suspend);

	return NOTIFY_OK;
}

/*
 * Sysfs get/set entries start
 */

static ssize_t load_threshold_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct hotplug_tunables *t = &tunables;

	return sprintf(buf, "%u\n", t->load_threshold);
}

static ssize_t load_threshold_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct hotplug_tunables *t = &tunables;

	unsigned int new_val;

	sscanf(buf, "%u", &new_val);

	if (new_val != t->load_threshold && new_val >= 0 && new_val <= 100)
	{
		t->load_threshold = new_val;
	}

	return size;
}

static ssize_t high_load_counter_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct hotplug_tunables *t = &tunables;

	return sprintf(buf, "%u\n", t->high_load_counter);
}

static ssize_t high_load_counter_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct hotplug_tunables *t = &tunables;

	unsigned int new_val;

	sscanf(buf, "%u", &new_val);

	if (new_val != t->high_load_counter && new_val >= 0 && new_val <= 50)
	{
		t->high_load_counter = new_val;
	}

	return size;
}

static ssize_t max_load_counter_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct hotplug_tunables *t = &tunables;

	return sprintf(buf, "%u\n", t->max_load_counter);
}

static ssize_t max_load_counter_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct hotplug_tunables *t = &tunables;

	unsigned int new_val;

	sscanf(buf, "%u", &new_val);

	if (new_val != t->max_load_counter && new_val >= 0 && new_val <= 50)
	{
		t->max_load_counter = new_val;
	}

	return size;
}

static ssize_t cpufreq_unplug_limit_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct hotplug_tunables *t = &tunables;

	return sprintf(buf, "%u\n", t->cpufreq_unplug_limit);
}

static ssize_t cpufreq_unplug_limit_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct hotplug_tunables *t = &tunables;

	unsigned int new_val;

	sscanf(buf, "%u", &new_val);

	if (new_val != t->cpufreq_unplug_limit &&
		new_val >= 0 && new_val <= UINT_MAX)
	{
		t->cpufreq_unplug_limit = new_val;
	}

	return size;
}

static ssize_t min_time_cpu_online_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct hotplug_tunables *t = &tunables;

	return sprintf(buf, "%u\n", t->min_time_cpu_online);
}

static ssize_t min_time_cpu_online_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct hotplug_tunables *t = &tunables;

	unsigned int new_val;

	sscanf(buf, "%u", &new_val);

	if (new_val != t->min_time_cpu_online && new_val >= 0 && new_val <= 100)
	{
		t->min_time_cpu_online = new_val;
	}

	return size;
}

static ssize_t timer_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct hotplug_tunables *t = &tunables;

	return sprintf(buf, "%u\n", t->timer);
}

static ssize_t timer_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t size)
{
	struct hotplug_tunables *t = &tunables;

	unsigned int new_val;

	sscanf(buf, "%u", &new_val);

	if (new_val != t->timer && new_val >= 0 && new_val <= 100)
	{
		t->timer = new_val;
	}

	return size;
}

static DEVICE_ATTR(load_threshold, 0664, load_threshold_show, load_threshold_store);
static DEVICE_ATTR(high_load_counter, 0664, high_load_counter_show,
					high_load_counter_store);
static DEVICE_ATTR(max_load_counter, 0664, max_load_counter_show,
					max_load_counter_store);
static DEVICE_ATTR(cpufreq_unplug_limit, 0664, cpufreq_unplug_limit_show,
					cpufreq_unplug_limit_store);
static DEVICE_ATTR(min_time_cpu_online, 0664, min_time_cpu_online_show,
					min_time_cpu_online_store);
static DEVICE_ATTR(timer, 0664, timer_show, timer_store);

static struct attribute *mako_hotplug_control_attributes[] =
{
	&dev_attr_load_threshold.attr,
	&dev_attr_high_load_counter.attr,
	&dev_attr_max_load_counter.attr,
	&dev_attr_cpufreq_unplug_limit.attr,
	&dev_attr_min_time_cpu_online.attr,
	&dev_attr_timer.attr,
	NULL
};

static struct attribute_group mako_hotplug_control_group =
{
	.attrs  = mako_hotplug_control_attributes,
};

static struct miscdevice mako_hotplug_control_device =
{
	.minor = MISC_DYNAMIC_MINOR,
	.name = "mako_hotplug_control",
};

/*
 * Sysfs get/set entries end
 */

static int __devinit mako_hotplug_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct hotplug_tunables *t = &tunables;

	wq = alloc_workqueue("mako_hotplug_workqueue", WQ_FREEZABLE, 1);

	if (!wq)
	{
		ret = -ENOMEM;
		goto err;
	}

	t->load_threshold = DEFAULT_LOAD_THRESHOLD;
	t->high_load_counter = DEFAULT_HIGH_LOAD_COUNTER;
	t->max_load_counter = DEFAULT_MAX_LOAD_COUNTER;
	t->cpufreq_unplug_limit = DEFAULT_CPUFREQ_UNPLUG_LIMIT;
	t->min_time_cpu_online = DEFAULT_MIN_TIME_CPU_ONLINE;
	t->timer = DEFAULT_TIMER;

	stats.timestamp[0] = ktime_to_us(ktime_get());
	stats.timestamp[1] = ktime_to_us(ktime_get());

	stats.notif.notifier_call = lcd_notifier_callback;

	if (lcd_register_client(&stats.notif))
	{
		ret = -EINVAL;
		goto err;
	}

	ret = misc_register(&mako_hotplug_control_device);

	if (ret)
	{
		ret = -EINVAL;
		goto err;
	}

	ret = sysfs_create_group(&mako_hotplug_control_device.this_device->kobj,
			&mako_hotplug_control_group);

	if (ret)
	{
		ret = -EINVAL;
		goto err;
	}

	INIT_WORK(&resume, mako_hotplug_resume);
	INIT_WORK(&suspend, mako_hotplug_suspend);
	INIT_DELAYED_WORK(&decide_hotplug, decide_hotplug_func);

	queue_delayed_work_on(0, wq, &decide_hotplug, HZ * 20);

err:
	return ret;
}

static struct platform_device mako_hotplug_device = {
	.name = MAKO_HOTPLUG,
	.id = -1,
};

static int mako_hotplug_remove(struct platform_device *pdev)
{
	destroy_workqueue(wq);

	return 0;
}

static struct platform_driver mako_hotplug_driver = {
	.probe = mako_hotplug_probe,
	.remove = mako_hotplug_remove,
	.driver = {
		.name = MAKO_HOTPLUG,
		.owner = THIS_MODULE,
	},
};

static int __init mako_hotplug_init(void)
{
	int ret;

	ret = platform_driver_register(&mako_hotplug_driver);

	if (ret)
	{
		return ret;
	}

	ret = platform_device_register(&mako_hotplug_device);

	if (ret)
	{
		return ret;
	}

	pr_info("%s: init\n", MAKO_HOTPLUG);

	return ret;
}

static void __exit mako_hotplug_exit(void)
{
	platform_device_unregister(&mako_hotplug_device);
	platform_driver_unregister(&mako_hotplug_driver);
}

late_initcall(mako_hotplug_init);
module_exit(mako_hotplug_exit);
