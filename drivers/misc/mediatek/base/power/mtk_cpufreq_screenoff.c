// SPDX-License-Identifier: GPL-2.0
/*
 * mtk_cpufreq_screenoff.c - cap CPU max frequency while the display is off
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
 * Runtime knobs, e.g. to turn the feature off for a battery comparison:
 *   /sys/module/mtk_cpufreq_screenoff/parameters/cap_enable      (bool)
 *   /sys/module/mtk_cpufreq_screenoff/parameters/cap_little_khz  (uint)
 *   /sys/module/mtk_cpufreq_screenoff/parameters/cap_big_khz     (uint)
 */

#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/threads.h>

#define SCREENOFF_CAP_LITTLE_KHZ	1200000
#define SCREENOFF_CAP_BIG_KHZ		1443000
/* anything with a higher ceiling than this is treated as the big cluster */
#define SCREENOFF_BIG_THRESHOLD_KHZ	1500000

static bool cap_enable = true;
static unsigned int cap_little_khz = SCREENOFF_CAP_LITTLE_KHZ;
static unsigned int cap_big_khz = SCREENOFF_CAP_BIG_KHZ;

module_param_named(cap_enable, cap_enable, bool, 0644);
module_param_named(cap_little_khz, cap_little_khz, uint, 0644);
module_param_named(cap_big_khz, cap_big_khz, uint, 0644);

/* max frequency to restore, indexed by the policy's owning CPU */
static unsigned int screenoff_saved_max[NR_CPUS];
static bool screenoff_applied;

static unsigned int screenoff_cap_of(struct cpufreq_policy *policy)
{
	return policy->cpuinfo.max_freq > SCREENOFF_BIG_THRESHOLD_KHZ ?
		cap_big_khz : cap_little_khz;
}

static void __maybe_unused screenoff_apply(bool off)
{
	unsigned int cpu;

	if (off == screenoff_applied)
		return;

	screenoff_applied = off;

	if (!cap_enable)
		return;

	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *policy;
		unsigned int applied;

		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;

		/* act once per policy, on the CPU that owns it */
		if (policy->cpu != cpu || policy->cpu >= NR_CPUS) {
			cpufreq_cpu_put(policy);
			continue;
		}

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
		cpufreq_cpu_put(policy);

		pr_info("screenoff: cpu%u max clamped to %u kHz (panel %s)\n",
			cpu, applied, off ? "off" : "on");
	}
}

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
		screenoff_apply(false);
		break;
	case FB_BLANK_NORMAL:
	case FB_BLANK_VSYNC_SUSPEND:
	case FB_BLANK_HSYNC_SUSPEND:
	case FB_BLANK_POWERDOWN:
		screenoff_apply(true);
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

	ret = fb_register_client(&screenoff_fb_notifier);
	if (ret) {
		pr_info("screenoff: display notifier registration failed (%d)\n",
			ret);
		return 0;
	}

	pr_info("screenoff: armed, little %u kHz / big %u kHz while panel is off\n",
		cap_little_khz, cap_big_khz);

	return 0;
}
#else
static int __init screenoff_cap_init(void)
{
	return 0;
}
#endif /* CONFIG_FB */

late_initcall(screenoff_cap_init);
