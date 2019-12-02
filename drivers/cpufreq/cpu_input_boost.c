// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018-2019 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

#define pr_fmt(fmt) "cpu_input_boost: " fmt

#include <linux/cpu_input_boost.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kthread.h>
#include <linux/moduleparam.h>
#include <linux/pm_qos.h>
#include <linux/sched.h>

static unsigned int idle_min_freq_lp __read_mostly =
	CONFIG_IDLE_MIN_FREQ_LP;
static unsigned int boost_min_freq_lp __read_mostly =
	CONFIG_BASE_BOOST_FREQ_LP;
static unsigned short powerhal_boost_duration __read_mostly =
	CONFIG_POWERHAL_BOOST_DURATION_MS;

module_param(idle_min_freq_lp, uint, 0644);
module_param_named(remove_input_boost_freq_lp,
	boost_min_freq_lp, uint, 0644);
module_param(powerhal_boost_duration, short, 0644);

enum {
	SCREEN_OFF,
	POWERHAL_BOOST,
	POWERHAL_MAX_BOOST,
};

struct boost_drv {
	struct delayed_work powerhal_unboost;
	struct delayed_work powerhal_max_unboost;
	struct notifier_block cpu_notif;
	struct notifier_block fb_notif;
	wait_queue_head_t boost_waitq;
	atomic_long_t powerhal_max_boost_expires;
	unsigned long state;
	struct pm_qos_request pm_qos_req;
};

static void powerhal_unboost_worker(struct work_struct *work);
static void powerhal_max_unboost_worker(struct work_struct *work);

static struct boost_drv boost_drv_g __read_mostly = {
	.powerhal_unboost = __DELAYED_WORK_INITIALIZER(boost_drv_g.powerhal_unboost,
							powerhal_unboost_worker, 0),
	.powerhal_max_unboost = __DELAYED_WORK_INITIALIZER(boost_drv_g.powerhal_max_unboost,
							powerhal_max_unboost_worker, 0),
	.boost_waitq = __WAIT_QUEUE_HEAD_INITIALIZER(boost_drv_g.boost_waitq)
};

static unsigned int get_min_freq(struct cpufreq_policy *policy)
{
	unsigned int freq;

	if (cpumask_test_cpu(policy->cpu, cpu_lp_mask))
		freq = boost_min_freq_lp;

	return max(freq, policy->cpuinfo.min_freq);
}

static unsigned int get_idle_freq(struct cpufreq_policy *policy)
{
	unsigned int freq;
 
	if (cpumask_test_cpu(policy->cpu, cpu_lp_mask))
		freq = idle_min_freq_lp;
 
	return max(freq, policy->cpuinfo.min_freq);
}

static void update_online_cpu_policy(void)
{
	unsigned int cpu;

	/* Only one CPU from each cluster needs to be updated */
	get_online_cpus();
	cpu = cpumask_first_and(cpu_lp_mask, cpu_online_mask);
	cpufreq_update_policy(cpu);
	put_online_cpus();
}

static void __powerhal_boost_kick(struct boost_drv *b)
{
	if (test_bit(SCREEN_OFF, &b->state))
		return;

	if (!powerhal_boost_duration)
		return;

	set_bit(POWERHAL_BOOST, &b->state);
	if (!mod_delayed_work(system_unbound_wq, &b->powerhal_unboost,
				msecs_to_jiffies(powerhal_boost_duration)))
		wake_up(&b->boost_waitq);
}

void powerhal_boost_kick(void)
{
	struct boost_drv *b = &boost_drv_g;

	__powerhal_boost_kick(b);
}

static void __powerhal_boost_kick_max(struct boost_drv *b,
				       unsigned int duration_ms)
{
	unsigned long boost_jiffies = msecs_to_jiffies(duration_ms);
	unsigned long curr_expires, new_expires;

	if (test_bit(SCREEN_OFF, &b->state))
		return;

	do {
		curr_expires = atomic_long_read(&b->powerhal_max_boost_expires);
		new_expires = jiffies + boost_jiffies;

		/* Skip this boost if there's a longer boost in effect */
		if (time_after(curr_expires, new_expires))
			return;
	} while (atomic_long_cmpxchg(&b->powerhal_max_boost_expires, curr_expires,
				     new_expires) != curr_expires);

	set_bit(POWERHAL_MAX_BOOST, &b->state);
	if (!mod_delayed_work(system_unbound_wq, &b->powerhal_max_unboost,
			      boost_jiffies))
		wake_up(&b->boost_waitq);
}

void powerhal_boost_kick_max(unsigned int duration_ms)
{
	struct boost_drv *b = &boost_drv_g;

	__powerhal_boost_kick_max(b, duration_ms);
}

static void powerhal_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work),
					   typeof(*b), powerhal_unboost);

	clear_bit(POWERHAL_BOOST, &b->state);
	wake_up(&b->boost_waitq);
}

static void powerhal_max_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work),
					   typeof(*b), powerhal_max_unboost);

	clear_bit(POWERHAL_MAX_BOOST, &b->state);
	wake_up(&b->boost_waitq);
}

static int cpu_boost_thread(void *data)
{
	static const struct sched_param sched_max_rt_prio = {
		.sched_priority = MAX_RT_PRIO - 1
	};
	struct boost_drv *b = data;
	unsigned long old_state = 0;

	sched_setscheduler_nocheck(current, SCHED_FIFO, &sched_max_rt_prio);

	while (1) {
		bool should_stop = false;
		unsigned long curr_state;

		wait_event(b->boost_waitq,
			(curr_state = READ_ONCE(b->state)) != old_state ||
			(should_stop = kthread_should_stop()));

		if (should_stop)
			break;

		old_state = curr_state;
		update_online_cpu_policy();
	}

	return 0;
}

static int cpu_notifier_cb(struct notifier_block *nb, unsigned long action,
			   void *data)
{
	struct boost_drv *b = container_of(nb, typeof(*b), cpu_notif);
	struct cpufreq_policy *policy = data;

	if (action != CPUFREQ_ADJUST)
		return NOTIFY_OK;

	/* Unboost when the screen is off */
	if (test_bit(SCREEN_OFF, &b->state)) {
		policy->min = get_idle_freq(policy);
		disable_schedtune_boost("top-app", true);
		/* CPUBW unboost */
		set_hyst_trigger_count_val(3);
		set_hist_memory_val(20);
		set_hyst_length_val(10);
		return NOTIFY_OK;
	}

	/* Do powerhal boost for powerhal_max_boost */
	if (test_bit(POWERHAL_MAX_BOOST, &b->state)) {
		/* CPUBW boost */
		set_hyst_trigger_count_val(0);
		set_hist_memory_val(0);
		set_hyst_length_val(0);
		/*
		 * max("wfi" latency-us val from dt) + 1 = 43
		 * val + 1 to prevent CPU from entering lower idle
		 * states than WFI.
		 */
		/* prevent CPU from entering deeper sleep states */
		pm_qos_update_request(&b->pm_qos_req, 43);
	} else {
		/* Restore default CPU DMA Latency value */
		pm_qos_update_request(&b->pm_qos_req,
			PM_QOS_CPU_DMA_LAT_DEFAULT_VALUE);
	}

	if (test_bit(POWERHAL_BOOST, &b->state)) {
		/* CPUBW boost */
		set_hyst_trigger_count_val(0);
		set_hist_memory_val(0);
		set_hyst_length_val(0);
	} else {
		/* CPUBW unboost */
		set_hyst_trigger_count_val(3);
		set_hist_memory_val(20);
		set_hyst_length_val(10);
	}

		policy->min = get_min_freq(policy);

	return NOTIFY_OK;
}

static int fb_notifier_cb(struct notifier_block *nb, unsigned long action,
			  void *data)
{
	struct boost_drv *b = container_of(nb, typeof(*b), fb_notif);
	int *blank = ((struct fb_event *)data)->data;

	/* Parse framebuffer blank events as soon as they occur */
	if (action != FB_EARLY_EVENT_BLANK)
		return NOTIFY_OK;

	/* Boost when the screen turns on and unboost when it turns off */
	if (*blank == FB_BLANK_UNBLANK) {
		disable_schedtune_boost("top-app", false);
		clear_bit(SCREEN_OFF, &b->state);
	} else {
		set_bit(SCREEN_OFF, &b->state);
		wake_up(&b->boost_waitq);
	}

	return NOTIFY_OK;
}

static void cpu_input_boost_input_event(struct input_handle *handle,
					unsigned int type, unsigned int code,
					int value)
{
	struct boost_drv *b = handle->handler->private;

	__powerhal_boost_kick(b);
}

static int cpu_input_boost_input_connect(struct input_handler *handler,
					 struct input_dev *dev,
					 const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpu_input_boost_handle";

	ret = input_register_handle(handle);
	if (ret)
		goto free_handle;

	ret = input_open_device(handle);
	if (ret)
		goto unregister_handle;

	return 0;

unregister_handle:
	input_unregister_handle(handle);
free_handle:
	kfree(handle);
	return ret;
}

static void cpu_input_boost_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id cpu_input_boost_ids[] = {
	/* Multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) }
	},
	/* Touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) }
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) }
	},
	{ }
};

static struct input_handler cpu_input_boost_input_handler = {
	.event		= cpu_input_boost_input_event,
	.connect	= cpu_input_boost_input_connect,
	.disconnect	= cpu_input_boost_input_disconnect,
	.name		= "cpu_input_boost_handler",
	.id_table	= cpu_input_boost_ids
};

static int __init cpu_input_boost_init(void)
{
	struct boost_drv *b = &boost_drv_g;
	struct task_struct *thread;
	int ret;

	pm_qos_add_request(&b->pm_qos_req, PM_QOS_CPU_DMA_LATENCY,
		PM_QOS_CPU_DMA_LAT_DEFAULT_VALUE);

	b->cpu_notif.notifier_call = cpu_notifier_cb;
	ret = cpufreq_register_notifier(&b->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
	if (ret) {
		pr_err("Failed to register cpufreq notifier, err: %d\n", ret);
		return ret;
	}

	cpu_input_boost_input_handler.private = b;
	ret = input_register_handler(&cpu_input_boost_input_handler);
	if (ret) {
		pr_err("Failed to register input handler, err: %d\n", ret);
		goto unregister_cpu_notif;
	}

	b->fb_notif.notifier_call = fb_notifier_cb;
	b->fb_notif.priority = INT_MAX;
	ret = fb_register_client(&b->fb_notif);
	if (ret) {
		pr_err("Failed to register fb notifier, err: %d\n", ret);
		goto unregister_handler;
	}

	thread = kthread_run_perf_critical(cpu_boost_thread, b, "cpu_boostd");
	if (IS_ERR(thread)) {
		ret = PTR_ERR(thread);
		pr_err("Failed to start CPU boost thread, err: %d\n", ret);
		goto unregister_fb_notif;
	}

	return 0;

unregister_fb_notif:
	fb_unregister_client(&b->fb_notif);
unregister_handler:
	input_unregister_handler(&cpu_input_boost_input_handler);
unregister_cpu_notif:
	cpufreq_unregister_notifier(&b->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
	pm_qos_remove_request(&b->pm_qos_req);
	return ret;
}
subsys_initcall(cpu_input_boost_init);
