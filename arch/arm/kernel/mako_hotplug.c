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

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/hotplug.h>
#include <linux/input.h>
#include <linux/lcd_notify.h>

#include <mach/cpufreq.h>

#define DEFAULT_FIRST_LEVEL 60
#define DEFAULT_SUSPEND_FREQ 729600
#define DEFAULT_CORES_ON_TOUCH 2
#define HIGH_LOAD_COUNTER 20
#define TIMER HZ

/*
 * 1000ms = 1 second
 */
#define MIN_TIME_CPU_ONLINE_MS 1000

static struct cpu_stats
{
    unsigned int default_first_level;
    unsigned int suspend_frequency;
    unsigned int cores_on_touch;
    unsigned int counter[2];
	unsigned long timestamp[2];
	bool ready_to_online[2];
	struct notifier_block notif;
	bool screen_off;
} stats = {
	.default_first_level = DEFAULT_FIRST_LEVEL,
    .suspend_frequency = DEFAULT_SUSPEND_FREQ,
    .cores_on_touch = DEFAULT_CORES_ON_TOUCH,
    .counter = {0},
	.timestamp = {0},
	.ready_to_online = {false},
	.screen_off = false,
};

static struct workqueue_struct *wq;
static struct delayed_work decide_hotplug;

static inline void calc_cpu_hotplug(unsigned int counter0,
									unsigned int counter1)
{
	//int cpu;
	int i, k;

	stats.ready_to_online[0] = counter0 >= 10;
	stats.ready_to_online[1] = counter1 >= 10;

	//commented for now
	/*if (unlikely(gpu_pref_counter >= 60))
	{
		if (num_online_cpus() < num_possible_cpus())
		{
			for_each_possible_cpu(cpu)
			{
				if (cpu && cpu_is_offline(cpu))
					cpu_up(cpu);
			}
		}

		return;
	}*/

	for (i = 0, k = 2; i < 2; i++, k++)
	{
		if (stats.ready_to_online[i])
		{
			if (cpu_is_offline(k))
			{
				cpu_up(k);
				stats.timestamp[i] = ktime_to_ms(ktime_get());
			}
		}
		else if (cpu_online(k))
		{
			/*
			 * Let's not unplug this cpu unless its been online for longer than
			 * 1sec to avoid consecutive ups and downs if the load is varying
			 * closer to the threshold point.
			 */
			if (ktime_to_ms(ktime_get()) + MIN_TIME_CPU_ONLINE_MS
					> stats.timestamp[i])
				cpu_down(k);
		}
	}
}

static void __ref decide_hotplug_func(struct work_struct *work)
{
    int cpu;
	//int i;

	//commented for now
	/*	
	if (_ts->ts_data.curr_data[0].state == ABS_PRESS)
	{
		for (i = num_online_cpus(); i < stats.cores_on_touch; i++)
		{
			if (cpu_is_offline(i))
			{
				cpu_up(i);
				stats.timestamp[i-2] = ktime_to_ms(ktime_get());
			}
		}
		goto re_queue;
	}*/

	if (unlikely(stats.screen_off))
		return;

    for_each_online_cpu(cpu) 
    {
        if (report_load_at_max_freq(cpu) >= stats.default_first_level)
        {
            if (likely(stats.counter[cpu] < HIGH_LOAD_COUNTER))    
                stats.counter[cpu] += 2;
        }

        else
        {
            if (stats.counter[cpu])
                --stats.counter[cpu];
        }

		if (cpu)
			break;
    }

	calc_cpu_hotplug(stats.counter[0], stats.counter[1]);

//re_queue:	
    queue_delayed_work(wq, &decide_hotplug, msecs_to_jiffies(TIMER));
}

void mako_hotplug_early_suspend(void)
{	 
    int cpu;

    /* cancel the hotplug work when the screen is off and flush the WQ */
    cancel_delayed_work(&decide_hotplug);
	flush_workqueue(wq);

    pr_info("Suspend stopping Hotplug work...\n");

	/* reset the counters so that we start clean next time the display is on */
    stats.counter[0] = 0;
    stats.counter[1] = 0;

    /* cap max frequency to 729MHz by default */
	for_each_possible_cpu(cpu)
    	msm_cpufreq_set_freq_limits(cpu, MSM_CPUFREQ_NO_LIMIT, 
            stats.suspend_frequency);

	disable_nonboot_cpus();

    pr_info("Cpulimit: Suspend - limit cpus max frequency to: %dMHz\n", 
			stats.suspend_frequency/1000);
}

void mako_hotplug_late_resume(void)
{  
    int cpu;

	/* restore max frequency */
	for_each_possible_cpu(cpu)
    	msm_cpufreq_set_freq_limits(cpu, MSM_CPUFREQ_NO_LIMIT, 
				MSM_CPUFREQ_NO_LIMIT);

	enable_nonboot_cpus();

    pr_info("Cpulimit: Resume - restore cpus max frequency.\n");
    
    pr_info("Resume starting Hotplug work...\n");
    queue_delayed_work(wq, &decide_hotplug, HZ);
}

static int lcd_notifier_callback(struct notifier_block *this,
				unsigned long event, void *data)
{

	switch (event) {
	case LCD_EVENT_ON_START:
		pr_info("LCD is on.\n");
		mako_hotplug_late_resume();
		stats.screen_off = false;
		break;
	case LCD_EVENT_ON_END:
		break;
	case LCD_EVENT_OFF_START:
		break;
	case LCD_EVENT_OFF_END:
		pr_info("LCD is off.\n");
		mako_hotplug_early_suspend();
		stats.screen_off = true;
		break;
	default:
		break;
	}

	return 0;
}

/* sysfs functions for external driver */
void update_first_level(unsigned int level)
{
    stats.default_first_level = level;
}

void update_suspend_frequency(unsigned int freq)
{
    stats.suspend_frequency = freq;
}

void update_cores_on_touch(unsigned int num)
{
    stats.cores_on_touch = num;
}

unsigned int get_first_level()
{
    return stats.default_first_level;
}

unsigned int get_suspend_frequency()
{
    return stats.suspend_frequency;
}

unsigned int get_cores_on_touch()
{
    return stats.cores_on_touch;
}
/* end sysfs functions from external driver */

int __init mako_hotplug_init(void)
{
	pr_info("Mako Hotplug driver started.\n");

    wq = alloc_ordered_workqueue("mako_hotplug_workqueue", 0);
    
    if (!wq)
        return -ENOMEM;

	stats.notif.notifier_call = lcd_notifier_callback;
	if (lcd_register_client(&stats.notif))
		return -EINVAL;
    
    INIT_DELAYED_WORK(&decide_hotplug, decide_hotplug_func);
    queue_delayed_work(wq, &decide_hotplug, HZ*30);
    
    return 0;
}
late_initcall(mako_hotplug_init);

