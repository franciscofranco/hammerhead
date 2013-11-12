/*
 * Copyright 2013 Francisco Franco
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/hotplug.h>

#define MAKO_HOTPLUG_CONTROL_VERSION 2

/*
 * Sysfs get/set entries
 */

static ssize_t first_level_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    return sprintf(buf, "%u\n", get_first_level());
}

static ssize_t first_level_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
    unsigned int new_val;
    
	sscanf(buf, "%u", &new_val);
    
    if (new_val != get_first_level() && new_val >= 0 && new_val <= 100)
    {
        update_first_level(new_val);
    }
    
    return size;
}

static ssize_t suspend_frequency_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    return sprintf(buf, "%u\n", get_suspend_frequency());
}

static ssize_t suspend_frequency_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
    unsigned int new_val;
    
	sscanf(buf, "%u", &new_val);
    
    if (new_val != get_suspend_frequency() && new_val >= 0 && new_val <= 1512000)
    {
        update_suspend_frequency(new_val);
    }
    
    return size;
}

static ssize_t cores_on_touch_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    return sprintf(buf, "%u\n", get_cores_on_touch());
}

static ssize_t cores_on_touch_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
    unsigned int new_val;
    
    sscanf(buf, "%u", &new_val);
    
    if (new_val != get_cores_on_touch() && new_val >= 2 && new_val <= 4)
    {
        update_cores_on_touch(new_val);
    }
    
    return size;
}

static ssize_t mako_hotplug_control_version(struct device *dev, struct device_attribute* attr, char *buf)
{
    return sprintf(buf, "%d\n", MAKO_HOTPLUG_CONTROL_VERSION);
}

static DEVICE_ATTR(first_level, 0777, first_level_show, first_level_store);
static DEVICE_ATTR(suspend_frequency, 0777, suspend_frequency_show, suspend_frequency_store);
static DEVICE_ATTR(cores_on_touch, 0777, cores_on_touch_show, cores_on_touch_store);
static DEVICE_ATTR(version, 0777 , mako_hotplug_control_version, NULL);

static struct attribute *mako_hotplug_control_attributes[] =
{
	&dev_attr_first_level.attr,
    &dev_attr_suspend_frequency.attr,
    &dev_attr_cores_on_touch.attr,
	&dev_attr_version.attr,
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

static int __init mako_hotplug_control_init(void)
{
    int ret;
    
    pr_info("%s misc_register(%s)\n", __FUNCTION__, mako_hotplug_control_device.name);
    
    ret = misc_register(&mako_hotplug_control_device);
    
    if (ret)
    {
	    pr_err("%s misc_register(%s) fail\n", __FUNCTION__, mako_hotplug_control_device.name);
	    return 1;
	}
    
    if (sysfs_create_group(&mako_hotplug_control_device.this_device->kobj, &mako_hotplug_control_group) < 0)
    {
	    pr_err("%s sysfs_create_group fail\n", __FUNCTION__);
	    pr_err("Failed to create sysfs group for device (%s)!\n", mako_hotplug_control_device.name);
	}
    
    return 0;
}
late_initcall(mako_hotplug_control_init);
