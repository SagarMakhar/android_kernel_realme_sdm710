/*
 * Detect Hung Task
 *
 * kernel/hung_task.c - kernel thread for detecting tasks stuck in D state
 *
 */

#include <linux/mm.h>
#include <linux/cpu.h>
#include <linux/nmi.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/freezer.h>
#include <linux/kthread.h>
#include <linux/lockdep.h>
#include <linux/export.h>
#include <linux/sysctl.h>
#include <linux/suspend.h>
#include <linux/utsname.h>
#include <trace/events/sched.h>
#include <linux/sched/sysctl.h>

#ifdef CONFIG_PRODUCT_REALME_SDM710
#include <soc/oppo/oppo_project.h>
#endif /* CONFIG_PRODUCT_REALME_SDM710 */

#ifdef CONFIG_PRODUCT_REALME_SDM710
#define HUNG_TASK_OPPO_KILL_LEN	128
char __read_mostly sysctl_hung_task_oppo_kill[HUNG_TASK_OPPO_KILL_LEN];
char last_stopper_comm[64];

#define TWICE_DEATH_PERIOD	300000000000ULL	//300s
#define MAX_DEATH_COUNT	3
#endif

#if defined(CONFIG_PRODUCT_REALME_SDM710) && defined(CONFIG_DEATH_HEALER)
#define MAX_IO_WAIT_HUNG 5
int __read_mostly sysctl_hung_task_maxiowait_count = MAX_IO_WAIT_HUNG;
#endif
/*
 * The number of tasks checked:
 */
int __read_mostly sysctl_hung_task_check_count = PID_MAX_LIMIT;

#if defined(CONFIG_PRODUCT_REALME_SDM710) && defined(CONFIG_OPPO_HEALTHINFO)
// Add for iowait hung monitor
#include <soc/oppo/oppo_healthinfo.h>
#endif

/*
 * Selective monitoring of hung tasks.
 *
 * if set to 1, khungtaskd skips monitoring tasks, which has
 * task_struct->hang_detection_enabled value not set, else monitors all tasks.
 */
int sysctl_hung_task_selective_monitoring = 1;

/*
 * Limit number of tasks checked in a batch.
 *
 * This value controls the preemptibility of khungtaskd since preemption
 * is disabled during the critical section. It also controls the size of
 * the RCU grace period. So it needs to be upper-bound.
 */
#define HUNG_TASK_LOCK_BREAK (HZ / 10)

/*
 * Zero means infinite timeout - no checking done:
 */
unsigned long __read_mostly sysctl_hung_task_timeout_secs = CONFIG_DEFAULT_HUNG_TASK_TIMEOUT;

int __read_mostly sysctl_hung_task_warnings = 10;

static int __read_mostly did_panic;

static struct task_struct *watchdog_task;

/*
 * Should we panic (and reboot, if panic_timeout= is set) when a
 * hung task is detected:
 */
unsigned int __read_mostly sysctl_hung_task_panic =
				CONFIG_BOOTPARAM_HUNG_TASK_PANIC_VALUE;

static int __init hung_task_panic_setup(char *str)
{
	int rc = kstrtouint(str, 0, &sysctl_hung_task_panic);

	if (rc)
		return rc;
	return 1;
}
__setup("hung_task_panic=", hung_task_panic_setup);

static int
hung_task_panic(struct notifier_block *this, unsigned long event, void *ptr)
{
	did_panic = 1;

	return NOTIFY_DONE;
}

static struct notifier_block panic_block = {
	.notifier_call = hung_task_panic,
};

#if defined(CONFIG_PRODUCT_REALME_SDM710)
static bool is_zygote_process(struct task_struct *t)
{
	const struct cred *tcred = __task_cred(t);
	if(!strcmp(t->comm, "main") && (tcred->uid.val == 0) && (t->parent != 0 && !strcmp(t->parent->comm,"init"))  )
		return true;
	else
		return false;
	return false;
}
#endif

#if defined(CONFIG_PRODUCT_REALME_SDM710) && defined(CONFIG_DEATH_HEALER)
static void check_hung_task(struct task_struct *t, unsigned long timeout, unsigned int *iowait_count)
#else
static void check_hung_task(struct task_struct *t, unsigned long timeout)
#endif
{
	unsigned long switch_count = t->nvcsw + t->nivcsw;

#if defined(CONFIG_PRODUCT_REALME_SDM710) && defined(CONFIG_DEATH_HEALER)
	static unsigned long long last_death_time = 0;
	unsigned long long cur_death_time = 0;
	static int death_count = 0;
#endif /* CONFIG_PRODUCT_REALME_SDM710 */

#ifdef CONFIG_PRODUCT_REALME_SDM710
#define DISP_TASK_COMM_LEN_MASK 10 //SDM845 change the new display thread with multi output, use len for masking
	if(!strncmp(t->comm,"mdss_dsi_event", TASK_COMM_LEN)||
		!strncmp(t->comm,"msm-core:sampli", TASK_COMM_LEN)||
		!strncmp(t->comm,"kworker/u16:1", TASK_COMM_LEN) ||
		!strncmp(t->comm,"mdss_fb0", TASK_COMM_LEN)||
		!strncmp(t->comm,"mdss_fb_ffl0", TASK_COMM_LEN)||
		!strncmp(t->comm,"panic_flush", TASK_COMM_LEN)||
		!strncmp(t->comm,"crtc_commit", DISP_TASK_COMM_LEN_MASK)||
		!strncmp(t->comm,"crtc_event", DISP_TASK_COMM_LEN_MASK)){
		return;
	}
#endif

	/*
	 * Ensure the task is not frozen.
	 * Also, skip vfork and any other user process that freezer should skip.
	 */
	if (unlikely(t->flags & (PF_FROZEN | PF_FREEZER_SKIP)))
#if defined(CONFIG_PRODUCT_REALME_SDM710) && defined(CONFIG_DEATH_HEALER)
	{
		if (is_zygote_process(t) || !strncmp(t->comm,"system_server", TASK_COMM_LEN)
			|| !strncmp(t->comm,"surfaceflinger", TASK_COMM_LEN)) {
			if (t->flags & PF_FROZEN)
				return;
		}
		else
			return;
	}
#else
		return;
#endif
	/*
	 * When a freshly created task is scheduled once, changes its state to
	 * TASK_UNINTERRUPTIBLE without having ever been switched out once, it
	 * musn't be checked.
	 */
	if (unlikely(!switch_count))
		return;

	if (switch_count != t->last_switch_count) {
		t->last_switch_count = switch_count;
		return;
	}

	trace_sched_process_hang(t);

#if defined(CONFIG_PRODUCT_REALME_SDM710) && defined(CONFIG_DEATH_HEALER)
	//if this task blocked at iowait. so maybe we should reboot system first
	if(t->in_iowait){
		printk(KERN_ERR "DeathHealer io wait too long time\n");
                if(t->mm != NULL && t == t->group_leader)// only work on user main thread
                {
                        *iowait_count = *iowait_count + 1;
                }
	}
	if (is_zygote_process(t) || !strncmp(t->comm,"system_server", TASK_COMM_LEN)
		|| !strncmp(t->comm,"surfaceflinger", TASK_COMM_LEN) ) {
		if (t->state == TASK_UNINTERRUPTIBLE)
			snprintf(sysctl_hung_task_oppo_kill, HUNG_TASK_OPPO_KILL_LEN, "%s,uninterruptible for %ld seconds", t->comm, timeout);
		else if (t->state == TASK_STOPPED)
			snprintf(sysctl_hung_task_oppo_kill, HUNG_TASK_OPPO_KILL_LEN, "%s,stopped for %ld seconds by %s", t->comm, timeout, last_stopper_comm);
		else if (t->state == TASK_TRACED)
			snprintf(sysctl_hung_task_oppo_kill, HUNG_TASK_OPPO_KILL_LEN, "%s,traced for %ld seconds", t->comm, timeout);
		else
			snprintf(sysctl_hung_task_oppo_kill, HUNG_TASK_OPPO_KILL_LEN, "%s,unknown hung for %ld seconds", t->comm, timeout);

		printk(KERN_ERR "DeathHealer: task %s:%d blocked for more than %ld seconds in state 0x%lx. Count:%d\n",
			t->comm, t->pid, timeout, t->state, death_count+1);

		death_count++;
		cur_death_time = local_clock();
		if (death_count >= MAX_DEATH_COUNT) {
			if (cur_death_time - last_death_time < TWICE_DEATH_PERIOD) {
				printk(KERN_ERR "DeathHealer has been triggered %d times, \
					last time at: %llu\n", death_count, last_death_time);
				BUG();
			}
		}
		last_death_time = cur_death_time;

		if (AGING == get_eng_version()) {
			BUG();
		} else {
			sched_show_task(t);
			debug_show_held_locks(t);
			trigger_all_cpu_backtrace();

			t->flags |= PF_OPPO_KILLING;
			do_send_sig_info(SIGKILL, SEND_SIG_FORCED, t, true);
			wake_up_process(t);
		}
	}
#endif
	if (!sysctl_hung_task_warnings && !sysctl_hung_task_panic)
		return;

	/*
	 * Ok, the task did not get scheduled for more than 2 minutes,
	 * complain:
	 */
	if (sysctl_hung_task_warnings) {
		sysctl_hung_task_warnings--;
		pr_err("INFO: task %s:%d blocked for more than %ld seconds.\n",
			t->comm, t->pid, timeout);
		pr_err("      %s %s %.*s\n",
			print_tainted(), init_utsname()->release,
			(int)strcspn(init_utsname()->version, " "),
			init_utsname()->version);
		pr_err("\"echo 0 > /proc/sys/kernel/hung_task_timeout_secs\""
			" disables this message.\n");
		sched_show_task(t);
		debug_show_all_locks();
	}

	touch_nmi_watchdog();

	if (sysctl_hung_task_panic) {
#ifdef CONFIG_PRODUCT_REALME_SDM710
		if (is_zygote_process(t) || !strncmp(t->comm,"system_server", TASK_COMM_LEN)
			|| !strncmp(t->comm,"surfaceflinger", TASK_COMM_LEN)) {
			trigger_all_cpu_backtrace();
			panic("hung_task: blocked tasks");
		}
#endif
	}
}

/*
 * To avoid extending the RCU grace period for an unbounded amount of time,
 * periodically exit the critical section and enter a new one.
 *
 * For preemptible RCU it is sufficient to call rcu_read_unlock in order
 * to exit the grace period. For classic RCU, a reschedule is required.
 */
static bool rcu_lock_break(struct task_struct *g, struct task_struct *t)
{
	bool can_cont;

	get_task_struct(g);
	get_task_struct(t);
	rcu_read_unlock();
	cond_resched();
	rcu_read_lock();
	can_cont = pid_alive(g) && pid_alive(t);
	put_task_struct(t);
	put_task_struct(g);

	return can_cont;
}

/*
 * Check whether a TASK_UNINTERRUPTIBLE does not get woken up for
 * a really long time (120 seconds). If that happens, print out
 * a warning.
 */
#if defined(CONFIG_PRODUCT_REALME_SDM710) && defined(CONFIG_OPPO_HEALTHINFO)
// Add for iowait hung ctrl set by QualityProtect APK RUS
extern bool ohm_iopanic_mon_ctrl;
extern bool ohm_iopanic_mon_logon;
extern bool ohm_iopanic_mon_trig;
extern unsigned int  iowait_hung_cnt;
extern unsigned int  iowait_panic_cnt;
#else
bool ohm_iopanic_mon_ctrl = true;
bool ohm_iopanic_mon_logon = false;
bool ohm_iopanic_mon_trig = false;
unsigned int  iowait_hung_cnt = 0;
unsigned int  iowait_panic_cnt = 0;
#endif /*CONFIG_PRODUCT_REALME_SDM710*/
static void check_hung_uninterruptible_tasks(unsigned long timeout)
{
	int max_count = sysctl_hung_task_check_count;
	unsigned long last_break = jiffies;
	struct task_struct *g, *t;
#if defined(CONFIG_PRODUCT_REALME_SDM710) && defined(CONFIG_DEATH_HEALER)
	unsigned int iowait_count = 0;
#endif

	/*
	 * If the system crashed already then all bets are off,
	 * do not report extra hung tasks:
	 */
	if (test_taint(TAINT_DIE) || did_panic)
		return;

	rcu_read_lock();
	for_each_process_thread(g, t) {
		if (!max_count--)
			goto unlock;
		if (time_after(jiffies, last_break + HUNG_TASK_LOCK_BREAK)) {
			if (!rcu_lock_break(g, t))
				goto unlock;
			last_break = jiffies;
		}
		/* use "==" to skip the TASK_KILLABLE tasks waiting on NFS */
#if defined(CONFIG_PRODUCT_REALME_SDM710) && defined(CONFIG_DEATH_HEALER)
		if (t->state == TASK_UNINTERRUPTIBLE || t->state == TASK_STOPPED || t->state == TASK_TRACED)
			check_hung_task(t, timeout,&iowait_count);
#else
		if (t->state == TASK_UNINTERRUPTIBLE)
			/* Check for selective monitoring */
			if (!sysctl_hung_task_selective_monitoring ||
			    t->hang_detection_enabled)
				check_hung_task(t, timeout);
#endif
	}
 unlock:
#if defined(CONFIG_PRODUCT_REALME_SDM710) && defined(CONFIG_DEATH_HEALER)
	if(iowait_count >= sysctl_hung_task_maxiowait_count){
                if (!ohm_iopanic_mon_ctrl){
			panic("hung_task:[%u]IO blocked too long time",iowait_count);
		}
		else
			iowait_panic_cnt++;
	}
        iowait_hung_cnt += iowait_count;
#endif
	rcu_read_unlock();
}

static long hung_timeout_jiffies(unsigned long last_checked,
				 unsigned long timeout)
{
	/* timeout of 0 will disable the watchdog */
	return timeout ? last_checked - jiffies + timeout * HZ :
		MAX_SCHEDULE_TIMEOUT;
}

/*
 * Process updating of timeout sysctl
 */
int proc_dohung_task_timeout_secs(struct ctl_table *table, int write,
				  void __user *buffer,
				  size_t *lenp, loff_t *ppos)
{
	int ret;

	ret = proc_doulongvec_minmax(table, write, buffer, lenp, ppos);

	if (ret || !write)
		goto out;

	wake_up_process(watchdog_task);

 out:
	return ret;
}

static atomic_t reset_hung_task = ATOMIC_INIT(0);

void reset_hung_task_detector(void)
{
	atomic_set(&reset_hung_task, 1);
}
EXPORT_SYMBOL_GPL(reset_hung_task_detector);

static bool hung_detector_suspended;

static int hungtask_pm_notify(struct notifier_block *self,
			      unsigned long action, void *hcpu)
{
	switch (action) {
	case PM_SUSPEND_PREPARE:
	case PM_HIBERNATION_PREPARE:
	case PM_RESTORE_PREPARE:
		hung_detector_suspended = true;
		break;
	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
		hung_detector_suspended = false;
		break;
	default:
		break;
	}
	return NOTIFY_OK;
}

/*
 * kthread which checks for tasks stuck in D state
 */
static int watchdog(void *dummy)
{
	unsigned long hung_last_checked = jiffies;

	set_user_nice(current, 0);

	for ( ; ; ) {
		unsigned long timeout = sysctl_hung_task_timeout_secs;
		long t = hung_timeout_jiffies(hung_last_checked, timeout);

		if (t <= 0) {
			if (!atomic_xchg(&reset_hung_task, 0) &&
			    !hung_detector_suspended)
				check_hung_uninterruptible_tasks(timeout);
			hung_last_checked = jiffies;
			continue;
		}
		schedule_timeout_interruptible(t);
	}

	return 0;
}

static int __init hung_task_init(void)
{
	atomic_notifier_chain_register(&panic_notifier_list, &panic_block);

	/* Disable hung task detector on suspend */
	pm_notifier(hungtask_pm_notify, 0);

	watchdog_task = kthread_run(watchdog, NULL, "khungtaskd");

	return 0;
}
subsys_initcall(hung_task_init);
