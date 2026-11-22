// SPDX-License-Identifier: GPL-2.0
/*
 * mtk_cpufreq_screenoff.c - cap CPU max frequency with the panel off
 *
 * Redmi Note 9 (merlin, MT6768 / Helio G85): with the panel off there is no
 * reason for background sync or browser prefetch bursts to climb to 2 GHz.
 * Clamping the per-policy maximum keeps that work on the low OPPs, which is
 * where the screen-off drain difference comes from.
 *
 * The limit goes in through the same path userspace uses for scaling_max_freq
 * (policy->max + cpufreq_update_policy()), so MTK PPM still arbitrates its
 * thermal, PTPOD and power limits on top of it, and cpu-hotplug still works.
 *
 * Optionally the big cluster can be taken offline entirely while the panel is
 * off (hotplug_big). That is off by default: it saves more current than a
 * frequency cap, but the wake-up path then has to bring two cores up before
 * the first frame can be drawn, and only a soak test can tell whether that is
 * visible on this device.
 *
 * Runtime knobs, e.g. to turn the feature off for a battery comparison:
 *   /sys/module/mtk_cpufreq_screenoff/parameters/cap_enable      (bool)
 *   /sys/module/mtk_cpufreq_screenoff/parameters/cap_little_khz  (uint)
 *   /sys/module/mtk_cpufreq_screenoff/parameters/cap_big_khz     (uint)
 *   /sys/module/mtk_cpufreq_screenoff/parameters/hotplug_big     (bool)
 */

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/suspend.h>
#include <linux/threads.h>
#include <linux/workqueue.h>

#define SCREENOFF_CAP_LITTLE_KHZ	1200000
#define SCREENOFF_CAP_BIG_KHZ		1443000
/*
 * The big cluster is the policy with the highest cpuinfo.max_freq, not simply
 * anything above a fixed threshold: on MT6768 the little cluster already tops
 * out at 1.8 GHz, so a 1.5 GHz threshold classifies all eight CPUs as big and
 * hotplug_big then offlines every core but one.
 */

static bool cap_enable;
static unsigned int cap_little_khz = SCREENOFF_CAP_LITTLE_KHZ;
static unsigned int cap_big_khz = SCREENOFF_CAP_BIG_KHZ;
static bool hotplug_big;

module_param_named(cap_enable, cap_enable, bool, 0644);
module_param_named(cap_little_khz, cap_little_khz, uint, 0644);
module_param_named(cap_big_khz, cap_big_khz, uint, 0644);
module_param_named(hotplug_big, hotplug_big, bool, 0644);

/* max frequency to restore, indexed by the policy's owning CPU */
static unsigned int screenoff_saved_max[NR_CPUS];
static bool screenoff_applied;
static bool screenoff_want_off;
static struct cpumask screenoff_big_mask;

/*
 * This work re-applies limits through cpufreq_update_policy(), which takes
 * policy->rwsem and, on this SoC, cpufreq_lock() plus PMIC/clock reads. Running
 * it while the system is suspending or resuming deadlocks against the device
 * suspend path and the platform watchdog then resets the phone. A freezable
 * workqueue stops the work from running at all between
 * freeze_workqueues_begin() and thaw_workqueues(), so it can only execute once
 * every device - the cpufreq driver and the PMIC included - is back.
 */
static struct workqueue_struct *screenoff_wq;
static bool screenoff_suspending;

/*
 * Resolved once: the event path must not walk every possible CPU and take
 * cpufreq references again on each panel transition. The SoC cpufreq driver
 * never unregisters, so the references held here are intentionally permanent.
 */
static struct cpufreq_policy *screenoff_policy[NR_CPUS];
static unsigned int screenoff_nr_policies;
/* highest cpuinfo.max_freq of all policies, i.e. the big cluster's ceiling */
static unsigned int screenoff_top_khz;

static void screenoff_resolve(void)
{
	unsigned int cpu, i;

	if (screenoff_nr_policies)
		return;

	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
		bool owner;

		if (!policy)
			continue;

		if (policy->cpuinfo.max_freq > screenoff_top_khz)
			screenoff_top_khz = policy->cpuinfo.max_freq;

		/* store one entry per policy, on the CPU that owns it */
		owner = policy->cpu == cpu &&
			screenoff_nr_policies < ARRAY_SIZE(screenoff_policy);
		if (owner)
			screenoff_policy[screenoff_nr_policies++] = policy;
		else
			cpufreq_cpu_put(policy);
	}

	for (i = 0; i < screenoff_nr_policies; i++) {
		struct cpufreq_policy *policy = screenoff_policy[i];

		if (policy->cpuinfo.max_freq != screenoff_top_khz)
			continue;

		cpumask_or(&screenoff_big_mask, &screenoff_big_mask, policy->cpus);
	}
}

static unsigned int screenoff_cap_of(struct cpufreq_policy *policy)
{
	return policy->cpuinfo.max_freq >= screenoff_top_khz ?
		cap_big_khz : cap_little_khz;
}

static void screenoff_caps(bool off)
{
	unsigned int i;

	/*
	 * Only the "apply" direction is conditional: disabling the cap at runtime
	 * must still restore the ceilings the next time the panel comes back. And
	 * a blank event must not leave screenoff_applied set while nothing was
	 * applied, or re-enabling cap_enable would never take effect again.
	 */
	if (off && !cap_enable)
		off = false;

	if (off == screenoff_applied)
		return;

	screenoff_applied = off;

	screenoff_resolve();

	for (i = 0; i < screenoff_nr_policies; i++) {
		struct cpufreq_policy *policy = screenoff_policy[i];
		unsigned int applied;

		if (off) {
			screenoff_saved_max[policy->cpu] = policy->max;
			policy->max = clamp_t(unsigned int,
					      screenoff_cap_of(policy),
					      policy->cpuinfo.min_freq,
					      policy->max);
		} else if (screenoff_saved_max[policy->cpu]) {
			policy->max = screenoff_saved_max[policy->cpu];
		}

		/* keep the pair consistent, then re-apply through the core */
		policy->min = min(policy->min, policy->max);
		applied = policy->max;
		cpufreq_update_policy(policy->cpu);

		pr_debug("screenoff: cpu%u max clamped to %u kHz (panel %s)\n",
			 policy->cpu, applied, off ? "off" : "on");
	}
}

/* The big-cluster mask is filled by screenoff_resolve(), while every CPU is
 * still online: cpufreq_cpu_get() cannot be trusted on an offlined CPU. */
static void screenoff_hotplug(bool off)
{
	unsigned int cpu;

	if (!hotplug_big)
		return;

	screenoff_resolve();
	if (cpumask_empty(&screenoff_big_mask))
		return;

	/* never offline so many cores that the system is left without one */
	if (off && cpumask_weight(&screenoff_big_mask) >= num_online_cpus())
		return;

	for_each_cpu(cpu, &screenoff_big_mask) {
		int ret = 0;

		if (off) {
			if (!cpu_online(cpu))
				continue;
			ret = cpu_down(cpu);
		} else {
			if (cpu_online(cpu))
				continue;
			ret = cpu_up(cpu);
		}

		if (ret)
			pr_info("screenoff: cpu%u %s failed (%d)\n", cpu,
				off ? "down" : "up", ret);
		else
			pr_debug("screenoff: cpu%u %s\n", cpu,
				 off ? "offlined" : "onlined");
	}
}

static void screenoff_work_fn(struct work_struct *work)
{
	bool off = screenoff_want_off;

	/* the freezable workqueue should make this impossible, but be explicit */
	if (screenoff_suspending)
		return;

	screenoff_resolve();

	/*
	 * Order matters: cores come back before the ceiling is lifted, and the
	 * ceiling is lowered before the cores go away, so work never lands on a
	 * cluster that is being taken down at that exact moment.
	 */
	if (!off)
		screenoff_hotplug(false);
	screenoff_caps(off);
	if (off)
		screenoff_hotplug(true);
}

static DECLARE_WORK(screenoff_work, screenoff_work_fn);

static void screenoff_queue(void)
{
	if (screenoff_suspending)
		return;

	queue_work(screenoff_wq, &screenoff_work);
}

#if IS_ENABLED(CONFIG_FB)
/*
 * Keep the ceiling applied for the whole sleep even if no blank event arrived,
 * and restore it only after the device tree (PMIC, clocks, cpufreq) is back.
 */
static int screenoff_pm_notifier_cb(struct notifier_block *nb,
				    unsigned long pm_event, void *data)
{
	switch (pm_event) {
	case PM_SUSPEND_PREPARE:
		screenoff_suspending = true;
		cancel_work_sync(&screenoff_work);
		/* process context, every device still up: safe to clamp here */
		screenoff_want_off = true;
		screenoff_caps(true);
		break;
	case PM_POST_SUSPEND:
		screenoff_suspending = false;
		/* runs once the workqueue is thawed, i.e. after resume finished */
		queue_work(screenoff_wq, &screenoff_work);
		break;
	default:
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block screenoff_pm_notifier = {
	.notifier_call = screenoff_pm_notifier_cb,
};

static int screenoff_fb_notifier_cb(struct notifier_block *nb,
				   unsigned long event, void *data)
{
	struct fb_event *evdata = data;
	int blank;

	if (event != FB_EVENT_BLANK || !evdata || !evdata->data)
		return 0;

	blank = *(int *)evdata->data;

	switch (blank) {
	case FB_BLANK_UNBLANK:
		screenoff_want_off = false;
		screenoff_queue();
		break;
	case FB_BLANK_NORMAL:
	case FB_BLANK_VSYNC_SUSPEND:
	case FB_BLANK_HSYNC_SUSPEND:
	case FB_BLANK_POWERDOWN:
		screenoff_want_off = true;
		screenoff_queue();
		break;
	default:
		break;
	}

	return 0;
}

static struct notifier_block screenoff_fb_notifier = {
	.notifier_call = screenoff_fb_notifier_cb,
};

static int __init screenoff_cap_init(void)
{
	int ret;

	/* Caches the policies and the big-cluster mask while all CPUs are up. */
	screenoff_resolve();

	screenoff_wq = alloc_workqueue("screenoff_cap",
				       WQ_FREEZABLE | WQ_MEM_RECLAIM, 0);
	if (!screenoff_wq)
		return -ENOMEM;

	ret = register_pm_notifier(&screenoff_pm_notifier);
	if (ret) {
		pr_info("screenoff: pm notifier registration failed (%d)\n", ret);
		return 0;
	}

	ret = fb_register_client(&screenoff_fb_notifier);
	if (ret) {
		pr_info("screenoff: display notifier registration failed (%d)\n",
			ret);
		return 0;
	}

	pr_info("screenoff: armed (%s), little %u kHz / big %u kHz while the panel is off%s\n",
		cap_enable ? "enabled" : "disabled, set cap_enable=1 to use",
		cap_little_khz, cap_big_khz,
		hotplug_big ? ", big cluster offline" : "");

	return 0;
}
#else
static int __init screenoff_cap_init(void)
{
	return 0;
}
#endif /* CONFIG_FB */

late_initcall(screenoff_cap_init);
