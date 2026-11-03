// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023-2024 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

/**
 * DOC: Capacity Aware Superset Scheduler (CASS) description
 *
 * The Capacity Aware Superset Scheduler (CASS) optimizes runqueue selection of
 * CFS tasks. By using CPU capacity as a basis for comparing the relative
 * utilization between different CPUs, CASS fairly balances load across CPUs of
 * varying capacities. This results in improved multi-core performance,
 * especially when CPUs are overutilized because CASS doesn't clip a CPU's
 * utilization when it eclipses the CPU's capacity.
 *
 * As a superset of capacity aware scheduling, CASS implements a hierarchy of
 * criteria to determine the better CPU to wake a task upon between CPUs that
 * have the same relative utilization. This way, single-core performance,
 * latency, and cache affinity are all optimized where possible.
 *
 * CASS doesn't feature explicit energy awareness but its basic load balancing
 * principle results in decreased overall energy, often better than what is
 * possible with explicit energy awareness. By fairly balancing load based on
 * relative utilization, all CPUs are kept at their lowest P-state necessary to
 * satisfy the overall load at any given moment.
 */

struct cass_cpu_cand {
	int cpu;
	unsigned int exit_lat;
	unsigned long cap;
	unsigned long util;
};

static __always_inline
unsigned long cass_cpu_util(int cpu, int this_cpu, bool sync)
{
	struct cfs_rq *cfs_rq = &cpu_rq(cpu)->cfs;
	unsigned long util = READ_ONCE(cfs_rq->avg.util_avg);

	/* Deduct @current's util from this CPU if this is a sync wake */
	if (sync && cpu == this_cpu)
		sub_positive(&util, task_util(current));

	if (sched_feat(UTIL_EST))
		util = max_t(unsigned long, util,
			     READ_ONCE(cfs_rq->avg.util_est.enqueued));

	return util;
}

/*
 * The margin used when comparing utilization with CPU capacity.
 *
 * (default: ~20%)
 */
#define fits_capacity(cap, max)	((cap) * 1280 < (max) * 1024)

/*
 * LazyExec: the foreground app's UI/render threads sleep between frames, so
 * their util "fits" a little core and CASS keeps waking them there (Insta's
 * main thread ran on big cores 1-3% of the time). uclamp.min steers them when
 * userspace sets it (see cass_fit_util()); on top of that, top-app tasks with
 * nice <= cass_boost_nice only count CPUs with room for CASS_BOOST_UTIL as
 * fitting (big cores), unless their uclamp.max caps them below their util.
 * echo -21 > /sys/module/fair/parameters/cass_boost_nice disables it.
 */
#define CASS_BOOST_UTIL	(SCHED_CAPACITY_SCALE / 2)
static int cass_boost_nice __read_mostly = -10;
module_param(cass_boost_nice, int, 0644);

/* Needs RCU read lock for the task group */
static inline bool cass_task_boosted(struct task_struct *p)
{
#ifdef CONFIG_CGROUP_SCHED
	struct cgroup *cgrp;

	if (task_nice(p) > READ_ONCE(cass_boost_nice))
		return false;

	cgrp = task_group(p)->css.cgroup;
	return cgrp && cgrp->kn && !strcmp(cgrp->kn->name, "top-app");
#else
	return false;
#endif
}

/*
 * The utilization CASS sizes @p with: its estimated util clamped to its
 * effective uclamp range. A uclamp.min boost makes only CPUs with room for it
 * count as fitting; a uclamp.max cap lets a heavy background task fit a little
 * CPU. The CPU candidates' own utilization keeps using @p's raw util.
 */
static inline unsigned long cass_fit_util(struct task_struct *p,
					  unsigned long p_util, bool *boosted)
{
#ifdef CONFIG_UCLAMP_TASK
	unsigned long uc_min = uclamp_eff_value(p, UCLAMP_MIN);
	unsigned long uc_max = uclamp_eff_value(p, UCLAMP_MAX);

	*boosted = uc_min > 0;
	/* Like uclamp_util_with(), a min above max wins */
	if (uc_min >= uc_max)
		return uc_min;
	return clamp(p_util, uc_min, uc_max);
#else
	*boosted = false;
	return p_util;
#endif
}

/* Returns true if @a is a better CPU than @b */
static __always_inline
bool cass_cpu_better(const struct cass_cpu_cand *a,
		     const struct cass_cpu_cand *b, unsigned long p_util,
		     int this_cpu, int prev_cpu, bool sync)
{
#define cass_cmp(a, b) ({ res = (a) - (b); })
#define cass_eq(a, b) ({ res = (a) == (b); })
	long res;

	/* Prefer the CPU that fits the task */
	if (cass_cmp(fits_capacity(p_util, a->cap),
		     fits_capacity(p_util, b->cap)))
		goto done;
		
	/* Prefer the current CPU for sync wakes */
	if (sync && (cass_eq(a->cpu, this_cpu) || !cass_cmp(b->cpu, this_cpu)))
		goto done;

	/* Prefer the CPU with lower relative utilization */
	if (cass_cmp(b->util, a->util))
		goto done;

	/* Prefer the CPU with higher capacity */
 	if (cass_cmp(a->cap, b->cap))
 		goto done;

	/* Prefer the CPU with higher idle exit latency */
	if (cass_cmp(a->exit_lat, b->exit_lat))
		goto done;

	/* Prefer the previous CPU */
	if (cass_eq(a->cpu, prev_cpu) || !cass_cmp(b->cpu, prev_cpu))
		goto done;

	/* Prefer the CPU that shares a cache with the previous CPU */
	if (cass_cmp(cpus_share_cache(a->cpu, prev_cpu),
		     cpus_share_cache(b->cpu, prev_cpu)))
		goto done;

	/* @a isn't a better CPU than @b. @res must be <=0 to indicate such. */
done:
	/* @a is a better CPU than @b if @res is positive */
	return res > 0;
}

static int cass_best_cpu(struct task_struct *p, int prev_cpu, bool sync)
{
	/* Initialize @best such that @best always has a valid CPU at the end */
	struct cass_cpu_cand cands[2], *best = cands, *curr;
	int this_cpu = raw_smp_processor_id();
	struct cpuidle_state *idle_state;
	bool has_idle = false, boosted;
	unsigned long p_util, fit_util;
	const struct cpumask *mask = &p->cpus_allowed;
	struct cpumask fit_mask;
	int cidx = 0, cpu;

	/* Get the utilization for this task */
	p_util = task_util_est(p);

	/*
	 * Find the best CPU to wake @p on. The RCU read lock is needed for
	 * idle_get_state().
	 */
	rcu_read_lock();
	fit_util = cass_fit_util(p, p_util, &boosted);
	/* The nice-based top-app boost stacks on uclamp, unless @p is capped */
	if (fit_util >= p_util && cass_task_boosted(p)) {
		fit_util = max_t(unsigned long, fit_util, CASS_BOOST_UTIL);
		boosted = true;
	}
	if (boosted) {
		/*
		 * An idle CPU always beats a busy one below, so a boosted task
		 * would still land on an idle little core whenever the big cores
		 * are busy (92% of Insta's UI wakeups). Only consider the CPUs
		 * that fit it, unless none of them are usable.
		 */
		cpumask_clear(&fit_mask);
		for_each_cpu_and(cpu, &p->cpus_allowed, cpu_active_mask) {
			if (fits_capacity(fit_util, capacity_of(cpu)))
				cpumask_set_cpu(cpu, &fit_mask);
		}
		if (!cpumask_empty(&fit_mask))
			mask = &fit_mask;
	}
	for_each_cpu_and(cpu, mask, cpu_active_mask) {
		/* Use the free candidate slot */
		struct rq *rq = cpu_rq(cpu);
		curr = &cands[cidx];
		curr->cpu = cpu;

		/*
		 * Check if this CPU is idle.
		 * For sync wakes, treat the current CPU as idle if @current is the
		 * only running task.
		 */
		if ((sync && cpu == this_cpu && rq->nr_running == 1) || idle_cpu(cpu)) {
			/* Discard any previous non-idle candidate */
			if (!has_idle)
				best = curr;
			has_idle = true;

			/* Nonzero exit latency indicates this CPU is idle */
			curr->exit_lat = 1;

			/* Add on the actual idle exit latency, if any */
			idle_state = idle_get_state(cpu_rq(cpu));
			if (idle_state)
				curr->exit_lat += idle_state->exit_latency;
		} else {
			/* Skip non-idle CPUs if there's an idle candidate */
			if (has_idle)
				continue;

			/* Zero exit latency indicates this CPU isn't idle */
			curr->exit_lat = 0;
		}

		/* Get this CPU's utilization, possibly without @current */
		curr->util = cass_cpu_util(cpu, this_cpu, sync);

		/*
		 * Add @p's utilization to this CPU if it's not @p's CPU, to
		 * find what this CPU's relative utilization would look like
		 * if @p were on it.
		 */
		if (cpu != task_cpu(p))
			curr->util += p_util;

		/*
		 * Get the current capacity of this CPU adjusted for thermal
		 * pressure as well as IRQ and RT-task time.
		 */
		curr->cap = capacity_of(cpu);

		/* Calculate the relative utilization for this CPU candidate */
		curr->util = curr->util * SCHED_CAPACITY_SCALE / curr->cap;

		/*
		 * Check if this CPU is better than the best CPU found so far.
		 * If @best == @curr then there's no need to compare them, but
		 * cidx still needs to be changed to the other candidate slot.
		 */
		if (best == curr ||
		    cass_cpu_better(curr, best, fit_util, this_cpu, prev_cpu,
				    sync)) {
			best = curr;
			cidx ^= 1;
		}
	}
	rcu_read_unlock();

	return best->cpu;
}

static int cass_select_task_rq_fair(struct task_struct *p, int prev_cpu,
				    int sd_flag, int wake_flags,
				    int sibling_count_hint)
{
	bool sync;

	/* Don't balance on exec since we don't know what @p will look like */
	if (sd_flag & SD_BALANCE_EXEC)
		return prev_cpu;

	/*
	 * If there aren't any valid CPUs which are active, then just return the
	 * first valid CPU since it's possible for certain types of tasks to run
	 * on inactive CPUs.
	 */
	if (unlikely(!cpumask_intersects(&p->cpus_allowed, cpu_active_mask)))
		return cpumask_first(&p->cpus_allowed);

	/* cass_best_cpu() needs the task's utilization, so sync it up */
	if (!(sd_flag & SD_BALANCE_FORK))
		sync_entity_load_avg(&p->se);

	sync = (wake_flags & WF_SYNC) && !(current->flags & PF_EXITING);
	return cass_best_cpu(p, prev_cpu, sync);
}
