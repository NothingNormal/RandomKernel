// SPDX-License-Identifier: GPL-2.0
/*
 * /sys/class/thermal/thermal_message
 *
 * Xiaomi userspace (thermal-engine, PowerKeeper, miui-services, camera,
 * NetworkBoost, mcd, ...) talks to the kernel through this node: it reads
 * board temperatures from it and writes thermal modes and scene hints into
 * it. Stock Xiaomi kernels add it inside thermal_core.c; this is a small,
 * self-contained version of that interface.
 *
 * Every node keeps the last value written to it. The temperature nodes fall
 * back to a real thermal zone until something writes them, so readers get a
 * sane value even when no daemon feeds them.
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/thermal.h>

#include "thermal_core.h"

#define TM_BUF_LEN	64

/* thermal zone names used while a temperature node was never written */
static char *board_sensor = "quiet-therm-usr";
module_param(board_sensor, charp, 0444);
static char *board_sensor_second = "xo-therm-usr";
module_param(board_sensor_second, charp, 0444);
static char *display_sensor = "backlight_therm";
module_param(display_sensor, charp, 0444);

struct tm_node {
	struct device_attribute attr;
	char val[TM_BUF_LEN];
	bool written;
	char **fallback;	/* zone name, only for temperature nodes */
};

static DEFINE_MUTEX(tm_lock);

static ssize_t tm_show(struct device *dev, struct device_attribute *attr,
		       char *buf)
{
	struct tm_node *n = container_of(attr, struct tm_node, attr);
	ssize_t ret;

	mutex_lock(&tm_lock);
	if (!n->written && n->fallback && *n->fallback) {
		struct thermal_zone_device *tz;
		int temp;

		tz = thermal_zone_get_zone_by_name(*n->fallback);
		if (!IS_ERR(tz) && !thermal_zone_get_temp(tz, &temp)) {
			mutex_unlock(&tm_lock);
			return scnprintf(buf, PAGE_SIZE, "%d\n", temp);
		}
	}
	ret = scnprintf(buf, PAGE_SIZE, "%s\n", n->val);
	mutex_unlock(&tm_lock);

	return ret;
}

static ssize_t tm_store(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t len)
{
	struct tm_node *n = container_of(attr, struct tm_node, attr);
	size_t l = min_t(size_t, len, TM_BUF_LEN - 1);

	mutex_lock(&tm_lock);
	memcpy(n->val, buf, l);
	n->val[l] = '\0';
	strim(n->val);
	n->written = true;
	mutex_unlock(&tm_lock);

	return len;
}

#define TM_NODE(_name, _def) {						\
	.attr = __ATTR(_name, 0664, tm_show, tm_store),			\
	.val = _def,							\
}
#define TM_TEMP(_name, _zone) {						\
	.attr = __ATTR(_name, 0664, tm_show, tm_store),			\
	.val = "0",							\
	.fallback = &_zone,						\
}

static struct tm_node tm_nodes[] = {
	/* thermal-engine profile selected by PowerKeeper; -1 = default */
	TM_NODE(sconfig, "-1"),
	TM_NODE(temp_state, "0"),
	TM_NODE(boost, "0"),
	TM_NODE(balance_mode, "0"),
	TM_NODE(cpu_limits, "0"),
	TM_NODE(screen_state, "0"),
	TM_NODE(charger_temp, "0"),
	TM_NODE(cloud_game, "0"),
	TM_NODE(connsys_limit, "0"),
	TM_NODE(wifi_limit, "0"),
	TM_NODE(flash_state, "0"),
	TM_NODE(torch_real_level, "0"),
	TM_NODE(super_hdr, "0"),
	TM_NODE(temp_aware, "0"),
	TM_NODE(dynamic_tj, "0"),
	TM_NODE(voice_receiver_state, "0"),
	TM_NODE(thermal_max_brightness, "0"),
	TM_NODE(board_sensor_trip, "0"),
	TM_NODE(ipa_cdev_load, "0"),
	TM_NODE(ipa_dump_info, "0"),
	TM_TEMP(board_sensor_temp, board_sensor),
	TM_TEMP(board_sensor_second_temp, board_sensor_second),
	TM_TEMP(board_sensor_other_temp, board_sensor_second),
	TM_TEMP(ambient_sensor_temp, board_sensor),
	TM_TEMP(display_therm_temp, display_sensor),
};

static ssize_t board_sensor_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", board_sensor);
}
static DEVICE_ATTR_RO(board_sensor);

static struct attribute *tm_attrs[ARRAY_SIZE(tm_nodes) + 2];

static const struct attribute_group tm_group = {
	.attrs = tm_attrs,
};

static const struct attribute_group *tm_groups[] = {
	&tm_group,
	NULL,
};

static void tm_release(struct device *dev)
{
}

static struct device tm_dev = {
	.init_name = "thermal_message",
	.groups = tm_groups,
	.release = tm_release,
};

int thermal_message_init(struct class *cls)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(tm_nodes); i++)
		tm_attrs[i] = &tm_nodes[i].attr.attr;
	tm_attrs[i] = &dev_attr_board_sensor.attr;

	tm_dev.class = cls;
	ret = device_register(&tm_dev);
	if (ret) {
		pr_err("thermal_message: register failed: %d\n", ret);
		put_device(&tm_dev);
	}

	return ret;
}
