// SPDX-License-Identifier: GPL-2.0
/*
 * Debug boot watchdog.
 *
 * If system_server has never started BOOT_WATCHDOG_SECS after the kernel
 * came up, dump the stuck tasks and panic. The panic does a warm reset,
 * so the dump survives in pstore (console-ramoops) and can be read from
 * recovery. It disarms the first time it sees system_server, so a normal
 * shutdown shortly after boot does not look like a stuck boot.
 */

#define pr_fmt(fmt) "boot_watchdog: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/nmi.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/sched/signal.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#define BOOT_WATCHDOG_SECS	90
#define BOOT_WATCHDOG_POLL_SECS	5

static struct delayed_work boot_wd_work;
static unsigned int boot_wd_waited;

static bool boot_wd_system_server_up(void)
{
	struct task_struct *p;
	bool found = false;

	rcu_read_lock();
	for_each_process(p) {
		if (!strcmp(p->comm, "system_server")) {
			found = true;
			break;
		}
	}
	rcu_read_unlock();

	return found;
}

static void boot_wd_dump_userspace(void)
{
	struct task_struct *g, *t;

	rcu_read_lock();
	for_each_process_thread(g, t) {
		/* kernel threads are covered by the D-state dump below */
		if (!t->mm)
			continue;
		sched_show_task(t);
		touch_nmi_watchdog();
	}
	rcu_read_unlock();
}

static void boot_wd_fn(struct work_struct *work)
{
	if (boot_wd_system_server_up()) {
		pr_info("system_server is up, disarmed\n");
		return;
	}

	boot_wd_waited += BOOT_WATCHDOG_POLL_SECS;
	if (boot_wd_waited < BOOT_WATCHDOG_SECS) {
		schedule_delayed_work(&boot_wd_work, BOOT_WATCHDOG_POLL_SECS * HZ);
		return;
	}

	pr_emerg("no system_server after %ds, dumping tasks\n",
		 BOOT_WATCHDOG_SECS);
	pr_emerg("---- userspace tasks ----\n");
	boot_wd_dump_userspace();
	pr_emerg("---- blocked (D state) tasks ----\n");
	show_state_filter(TASK_UNINTERRUPTIBLE);

	panic("boot_watchdog: boot stuck before system_server");
}

static int __init boot_wd_init(void)
{
	INIT_DELAYED_WORK(&boot_wd_work, boot_wd_fn);
	schedule_delayed_work(&boot_wd_work, BOOT_WATCHDOG_POLL_SECS * HZ);
	pr_info("armed, %ds\n", BOOT_WATCHDOG_SECS);

	return 0;
}
late_initcall(boot_wd_init);
