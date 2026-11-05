// SPDX-License-Identifier: GPL-2.0
/*
 * Debug: keep the kernel log across hangs by writing it to the raw "logdump"
 * partition. RAM (pstore) may not survive a hard reset and /data is not up early
 * in boot, so this only depends on the storage driver.
 *
 * Layout: one header page, then PLOG_SLOTS equal slots, one per boot, reused
 * in turn. A slot's first PLOG_HEAD_BYTES keep the start of the boot; the rest
 * is a ring holding the newest lines. Text is plain, so `strings` can read it.
 * The reader lives in xdkit: workbase/tools/persistent-log-read.py.
 *
 * A hard freeze loses at most the last PLOG_FLUSH_MS of log.
 */

#define pr_fmt(fmt) "persistent_log: " fmt

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/freezer.h>
#include <linux/genhd.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kmsg_dump.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>

#define PLOG_PART		"logdump"
#define PLOG_MAGIC		0x31474f4c53524550ULL	/* "PERSLOG1" */
#define PLOG_SLOTS		8ULL
#define PLOG_HEAD_BYTES	(1024ULL * 1024)
#define PLOG_FLUSH_MS		500
#define PLOG_WAIT_MS		100
#define PLOG_WAIT_TRIES	600	/* 60 s for the storage driver */
#define PLOG_MAX_ERRORS	20

struct plog_header {
	__le64 magic;
	__le64 boot;
	__le64 slots;
	__le64 slot_bytes;
};

static struct block_device *plog_bdev;
static struct page *plog_page;		/* the slot page being filled */
static char *plog_line;
static u64 plog_slot_start, plog_slot_bytes, plog_pos;
static bool plog_ready;
static unsigned int plog_errors;
static DEFINE_MUTEX(plog_lock);
static struct kmsg_dumper plog_dumper = { .active = true };

static int plog_match_part(struct device *dev, const void *data)
{
	struct hd_struct *part;

	if (dev->type != &part_type)
		return 0;
	part = dev_to_part(dev);
	return part->info && !strcmp(part->info->volname, PLOG_PART);
}

static int plog_io(unsigned int op, u64 byte_pos, struct page *page)
{
	struct bio *bio;
	int ret;

	bio = bio_alloc(GFP_KERNEL, 1);
	if (!bio)
		return -ENOMEM;
	bio_set_dev(bio, plog_bdev);
	bio->bi_iter.bi_sector = byte_pos >> 9;
	bio->bi_opf = op;
	bio_add_page(bio, page, PAGE_SIZE, 0);
	ret = submit_bio_wait(bio);
	bio_put(bio);
	return ret;
}

static int plog_write_page(void)
{
	/* the tail page is rewritten as it fills, so FUA makes each flush durable */
	return plog_io(REQ_OP_WRITE | REQ_SYNC | REQ_FUA,
		     plog_slot_start + round_down(plog_pos - 1, PAGE_SIZE), plog_page);
}

static void plog_put(const char *s, size_t len)
{
	char *page = page_address(plog_page);

	while (len) {
		size_t off = plog_pos % PAGE_SIZE;
		size_t n = min_t(size_t, len, PAGE_SIZE - off);

		memcpy(page + off, s, n);
		plog_pos += n;
		s += n;
		len -= n;

		if (!(plog_pos % PAGE_SIZE)) {
			if (plog_write_page())
				plog_errors++;
			memset(page, 0, PAGE_SIZE);
			if (plog_pos >= plog_slot_bytes)
				plog_pos = PLOG_HEAD_BYTES;
		}
	}
}

/* copy new log lines into the slot and make them durable */
static void plog_flush(void)
{
	size_t len;
	bool wrote = false;

	mutex_lock(&plog_lock);
	if (!plog_ready || plog_errors > PLOG_MAX_ERRORS)
		goto out;
	while (kmsg_dump_get_line(&plog_dumper, true, plog_line, PAGE_SIZE, &len)) {
		plog_put(plog_line, len);
		wrote = true;
	}
	if (wrote && (plog_pos % PAGE_SIZE) && plog_write_page())
		plog_errors++;
out:
	mutex_unlock(&plog_lock);
}

static int plog_reboot_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	/* runs before devices shut down, so storage still works */
	plog_flush();
	return NOTIFY_DONE;
}

static struct notifier_block plog_reboot_nb = {
	.notifier_call = plog_reboot_notify,
	.priority = INT_MIN,	/* last, to include what other notifiers logged */
};

static struct device *plog_wait_for_part(void)
{
	struct device *dev;
	int i;

	for (i = 0; i < PLOG_WAIT_TRIES; i++) {
		dev = class_find_device(&block_class, NULL, NULL, plog_match_part);
		if (dev)
			return dev;
		msleep(PLOG_WAIT_MS);
	}
	return NULL;
}

static int plog_open(void)
{
	const fmode_t mode = FMODE_READ | FMODE_WRITE;
	struct plog_header *hdr;
	struct page *hdr_page;
	struct device *dev;
	u64 boot = 0, size;
	int ret, n;

	dev = plog_wait_for_part();
	if (!dev) {
		pr_err("no %s partition\n", PLOG_PART);
		return -ENODEV;
	}
	plog_bdev = blkdev_get_by_dev(dev->devt, mode, THIS_MODULE);
	put_device(dev);
	if (IS_ERR(plog_bdev)) {
		ret = PTR_ERR(plog_bdev);
		plog_bdev = NULL;
		return ret;
	}

	size = i_size_read(plog_bdev->bd_inode);
	plog_slot_bytes = rounddown((size - PAGE_SIZE) / PLOG_SLOTS, PAGE_SIZE);
	if (plog_slot_bytes <= PLOG_HEAD_BYTES * 2) {
		ret = -ENOSPC;
		goto put;
	}

	hdr_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!hdr_page) {
		ret = -ENOMEM;
		goto put;
	}
	hdr = page_address(hdr_page);
	ret = plog_io(REQ_OP_READ, 0, hdr_page);
	if (!ret && le64_to_cpu(hdr->magic) == PLOG_MAGIC)
		boot = le64_to_cpu(hdr->boot) + 1;
	if (ret)
		goto free_hdr;

	memset(hdr, 0, PAGE_SIZE);
	hdr->magic = cpu_to_le64(PLOG_MAGIC);
	hdr->boot = cpu_to_le64(boot);
	hdr->slots = cpu_to_le64(PLOG_SLOTS);
	hdr->slot_bytes = cpu_to_le64(plog_slot_bytes);
	ret = plog_io(REQ_OP_WRITE | REQ_SYNC | REQ_FUA, 0, hdr_page);
	if (ret)
		goto free_hdr;

	plog_slot_start = PAGE_SIZE + do_div(boot, PLOG_SLOTS) * plog_slot_bytes;
	plog_pos = 0;

	/* an older boot's text must not read as part of this one */
	ret = blkdev_issue_zeroout(plog_bdev, plog_slot_start >> 9, plog_slot_bytes >> 9,
				   GFP_KERNEL, 0);
	if (ret)
		goto free_hdr;

	mutex_lock(&plog_lock);
	plog_ready = true;
	mutex_unlock(&plog_lock);

	n = scnprintf(plog_line, PAGE_SIZE, "=== persistent log boot %llu ===\n",
		      le64_to_cpu(hdr->boot));
	kmsg_dump_rewind(&plog_dumper);
	mutex_lock(&plog_lock);
	plog_put(plog_line, n);
	mutex_unlock(&plog_lock);
free_hdr:
	__free_page(hdr_page);
	if (!ret)
		return 0;
put:
	blkdev_put(plog_bdev, mode);
	plog_bdev = NULL;
	return ret;
}

static int plog_thread(void *arg)
{
	int ret = plog_open();

	if (ret) {
		pr_err("disabled: %d\n", ret);
		return ret;
	}
	pr_info("logging to %s, slot at %llu\n", PLOG_PART, plog_slot_start);
	register_reboot_notifier(&plog_reboot_nb);

	set_freezable();
	set_user_nice(current, -10);
	while (!kthread_should_stop()) {
		plog_flush();
		try_to_freeze();
		schedule_timeout_interruptible(msecs_to_jiffies(PLOG_FLUSH_MS));
	}
	return 0;
}

static int __init plog_init(void)
{
	struct task_struct *t;

	plog_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	plog_line = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!plog_page || !plog_line)
		return -ENOMEM;
	t = kthread_run(plog_thread, NULL, "persistent_log");
	return PTR_ERR_OR_ZERO(t);
}
late_initcall(plog_init);
