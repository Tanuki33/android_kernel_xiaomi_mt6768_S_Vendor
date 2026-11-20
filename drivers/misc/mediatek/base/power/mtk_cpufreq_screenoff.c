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
#include <linux/threads.h>
#include <linux/workqueue.h>

#define SCREENOFF_CAP_LITTLE_KHZ	1200000
#define SCREENOFF_CAP_BIG_KHZ		1443000
/* anything with a higher ceiling than this is treated as the big cluster */
#define SCREENOFF_BIG_THRESHOLD_KHZ	1500000

static bool cap_enable = true;
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
 * Resolved once: the event path must not walk every possible CPU and take
 * cpufreq references again on each panel transition. The SoC cpufreq driver
 * never unregisters, so the references held here are intentionally permanent.
 */
static struct cpufreq_policy *screenoff_policy[NR_CPUS];
static unsigned int screenoff_nr_policies;

static void screenoff_resolve(void)
{
	unsigned int cpu;

	if (screenoff_nr_policies)
		return;

	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

		if (!policy)
			continue;

		if (policy->cpuinfo.max_freq > SCREENOFF_BIG_THRESHOLD_KHZ)
			cpumask_set_cpu(cpu, &screenoff_big_mask);

		/* store one entry per policy, on the CPU that owns it */
		if (policy->cpu != cpu ||
		    screenoff_nr_policies >= ARRAY_SIZE(screenoff_policy)) {
			cpufreq_cpu_put(policy);
			continue;
		}

		screenoff_policy[screenoff_nr_policies++] = policy;
	}
}

static unsigned int screenoff_cap_of(struct cpufreq_policy *policy)
{
	return policy->cpuinfo.max_freq > SCREENOFF_BIG_THRESHOLD_KHZ ?
		cap_big_khz : cap_little_khz;
}

static void screenoff_caps(bool off)
{
	unsigned int i;

	if (off == screenoff_applied)
		return;

	screenoff_applied = off;

	if (!cap_enable)
		return;

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

#if IS_ENABLED(CONFIG_FB)
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
		schedule_work(&screenoff_work);
		break;
	case FB_BLANK_NORMAL:
	case FB_BLANK_VSYNC_SUSPEND:
	case FB_BLANK_HSYNC_SUSPEND:
	case FB_BLANK_POWERDOWN:
		screenoff_want_off = true;
		schedule_work(&screenoff_work);
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

	ret = fb_register_client(&screenoff_fb_notifier);
	if (ret) {
		pr_info("screenoff: display notifier registration failed (%d)\n",
			ret);
		return 0;
	}

	pr_info("screenoff: armed, little %u kHz / big %u kHz while panel is off%s\n",
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
