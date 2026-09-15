#include <linux/module.h>
#include <linux/tty.h>
#include <linux/miscdevice.h>
#include <linux/proc_fs.h>
#include <linux/kprobes.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/uaccess.h>
#include <asm/ptrace.h>
#include <asm/current.h>
#include "comm.h"
#include "memory.h"
#include "process.h"
#include "hide_process.h"
#include "pte_track.h"

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0))
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
#endif

/* 注意：内核头文件里 task 是宏，不能作为变量名，所以改成 g_task */
struct task_struct *g_task;
struct task_struct *hide_pid_process_task;
int hide_process_pid = 0;
int hide_process_state = 0;

static struct mem_tool_device {
    struct cdev cdev;
    struct device *dev;
    int max;
} memdev;

static dev_t mem_tool_dev_t;
static struct class *mem_tool_class;
const char *devicename;

/* ================= 硬件断点追踪模块 ================= */

typedef struct perf_event *(*reg_user_hwbp_t)(struct perf_event_attr *attr,
                                              perf_overflow_handler_t triggered,
                                              void *context,
                                              struct task_struct *tsk);
typedef void (*unreg_hwbp_t)(struct perf_event *bp);

static reg_user_hwbp_t p_register_user_hw_breakpoint;
static unreg_hwbp_t p_unregister_hw_breakpoint;

static int resolve_hwbp_symbols(void)
{
    struct kprobe kp;
    int ret;

    memset(&kp, 0, sizeof(kp));
    kp.symbol_name = "register_user_hw_breakpoint";
    ret = register_kprobe(&kp);
    if (ret < 0) {
        pr_err("resolve register_user_hw_breakpoint failed: %d\n", ret);
        return ret;
    }
    p_register_user_hw_breakpoint = (reg_user_hwbp_t)kp.addr;
    unregister_kprobe(&kp);

    memset(&kp, 0, sizeof(kp));
    kp.symbol_name = "unregister_hw_breakpoint";
    ret = register_kprobe(&kp);
    if (ret < 0) {
        pr_err("resolve unregister_hw_breakpoint failed: %d\n", ret);
        return ret;
    }
    p_unregister_hw_breakpoint = (unreg_hwbp_t)kp.addr;
    unregister_kprobe(&kp);

    return 0;
}

#define OP_HWBP_INSTALL    0x820
#define OP_HWBP_SET_TARGET 0x821
#define OP_HWBP_REMOVE     0x822
#define OP_HWBP_GET_HITS   0x823
#define OP_HWBP_SET_PAUSE  0x824

#define HWBP_MAX_FP_REGS 8

struct hwbp_install_req {
    pid_t pid;
    uint64_t addr;
    uint32_t active;
};

struct hwbp_target_req {
    uint64_t addr;
    uint32_t active;
    uint32_t fp_count;
    uint32_t fp_indices[HWBP_MAX_FP_REGS];
    uint64_t fp_values[HWBP_MAX_FP_REGS][2];
};

static struct {
    bool installed;
    bool paused;
    pid_t pid;
    uint64_t target_addr;
    struct perf_event *bp_event;

    bool active;
    uint32_t fp_count;
    uint32_t fp_indices[HWBP_MAX_FP_REGS];
    uint64_t fp_values[HWBP_MAX_FP_REGS][2];

    uint64_t hit_count;
} g_hwbp_state;

static DEFINE_MUTEX(hwbp_lock);

static void hwbp_modify_fp(struct pt_regs *regs)
{
    struct task_struct *tsk = current;
    struct user_fpsimd_state *fpsimd;
    int i;

    if (!g_hwbp_state.active || g_hwbp_state.fp_count == 0)
        return;

    fpsimd = &tsk->thread.uw.fpsimd_state;
    if (!fpsimd) return;

    for (i = 0; i < g_hwbp_state.fp_count && i < HWBP_MAX_FP_REGS; i++) {
        uint32_t idx = g_hwbp_state.fp_indices[i];
        if (idx >= 32) continue;
        fpsimd->vregs[idx] = g_hwbp_state.fp_values[i][0] |
                             ((__uint128_t)g_hwbp_state.fp_values[i][1] << 64);
    }
}

static void hwbp_handler(struct perf_event *bp,
                         struct perf_sample_data *data,
                         struct pt_regs *regs)
{
    if (current->pid != g_hwbp_state.pid)
        return;

    if (g_hwbp_state.paused)
        return;

    g_hwbp_state.hit_count++;
    hwbp_modify_fp(regs);
}

static int hwbp_do_install(struct hwbp_install_req *req)
{
    struct task_struct *tsk;
    struct perf_event_attr attr;
    int ret;

    if (g_hwbp_state.installed)
        return -EALREADY;

    tsk = pid_task(find_vpid(req->pid), PIDTYPE_PID);
    if (!tsk) {
        pr_err("hwbp: target pid %d not found\n", req->pid);
        return -ESRCH;
    }

    hw_breakpoint_init(&attr);
    attr.bp_addr = req->addr;
    attr.bp_len = HW_BREAKPOINT_LEN_4;
    attr.bp_type = HW_BREAKPOINT_X;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;

    g_hwbp_state.bp_event =
        p_register_user_hw_breakpoint(&attr, hwbp_handler, NULL, tsk);

    if (IS_ERR(g_hwbp_state.bp_event)) {
        ret = PTR_ERR(g_hwbp_state.bp_event);
        pr_err("hwbp: p_register_user_hw_breakpoint failed: %d\n", ret);
        g_hwbp_state.bp_event = NULL;
        return ret;
    }

    g_hwbp_state.pid = req->pid;
    g_hwbp_state.target_addr = req->addr;
    g_hwbp_state.active = req->active;
    g_hwbp_state.hit_count = 0;
    g_hwbp_state.paused = false;
    g_hwbp_state.installed = true;

    pr_info("hwbp: installed pid=%d addr=0x%llx\n", g_hwbp_state.pid, g_hwbp_state.target_addr);
    return 0;
}

static void hwbp_do_remove(void)
{
    if (!g_hwbp_state.installed)
        return;

    if (g_hwbp_state.bp_event) {
        p_unregister_hw_breakpoint(g_hwbp_state.bp_event);
        g_hwbp_state.bp_event = NULL;
    }

    g_hwbp_state.installed = false;
    pr_info("hwbp: removed, hits=%llu\n", g_hwbp_state.hit_count);
}

static void hwbp_set_pause(bool pause)
{
    if (!g_hwbp_state.installed || !g_hwbp_state.bp_event)
        return;

    g_hwbp_state.paused = pause;

    if (pause) {
        perf_event_disable(g_hwbp_state.bp_event);
    } else {
        perf_event_enable(g_hwbp_state.bp_event);
    }
}

static long hwbp_dispatch_ioctl(unsigned int cmd, unsigned long arg)
{
    void __user *argp = (void __user *)arg;
    int ret = 0;

    mutex_lock(&hwbp_lock);

    switch (cmd) {
    case OP_HWBP_INSTALL: {
        struct hwbp_install_req req;
        if (copy_from_user(&req, argp, sizeof(req))) { ret = -EFAULT; break; }
        ret = hwbp_do_install(&req);
        break;
    }
    case OP_HWBP_SET_TARGET: {
        struct hwbp_target_req req;
        int i;
        if (copy_from_user(&req, argp, sizeof(req))) { ret = -EFAULT; break; }
        if (!g_hwbp_state.installed) { ret = -ENODEV; break; }
        g_hwbp_state.active = req.active;
        g_hwbp_state.fp_count = req.fp_count > HWBP_MAX_FP_REGS ? HWBP_MAX_FP_REGS : req.fp_count;
        for (i = 0; i < g_hwbp_state.fp_count; i++) {
            g_hwbp_state.fp_indices[i] = req.fp_indices[i];
            g_hwbp_state.fp_values[i][0] = req.fp_values[i][0];
            g_hwbp_state.fp_values[i][1] = req.fp_values[i][1];
        }
        break;
    }
    case OP_HWBP_REMOVE:
        hwbp_do_remove();
        break;
    case OP_HWBP_GET_HITS:
        if (copy_to_user(argp, &g_hwbp_state.hit_count, sizeof(uint64_t)))
            ret = -EFAULT;
        break;
    case OP_HWBP_SET_PAUSE: {
        int pause;
        if (copy_from_user(&pause, argp, sizeof(int))) { ret = -EFAULT; break; }
        hwbp_set_pause(!!pause);
        break;
    }
    default:
        ret = -ENOTTY;
    }

    mutex_unlock(&hwbp_lock);
    return ret;
}

static void hwbp_cleanup(void)
{
    mutex_lock(&hwbp_lock);
    hwbp_do_remove();
    mutex_unlock(&hwbp_lock);
}

/* ================= 硬件断点追踪模块结束 ================= */


long dispatch_ioctl(struct file *const file, unsigned int const cmd, unsigned long const arg)
{
	static COPY_MEMORY cm;
	static MODULE_BASE mb;
	static struct process p_process;
	static char name[0x100] = {0};

	switch (cmd) {
		case OP_READ_MEM:
			{
				if (copy_from_user(&cm, (void __user*)arg, sizeof(cm)) != 0) {
					return -1;
				}
				if (read_process_memory(cm.pid, cm.addr, cm.buffer, cm.size) == false) {
					return -1;
				}
			}
			break;

		case OP_WRITE_MEM:
			{
				if (copy_from_user(&cm, (void __user*)arg, sizeof(cm)) != 0) {
					return -1;
				}
				if (write_process_memory(cm.pid, cm.addr, cm.buffer, cm.size) == false) {
					return -1;
				}
			}
			break;

		case OP_MODULE_BASE:
			{
				if (copy_from_user(&mb, (void __user*)arg, sizeof(mb)) != 0 
				|| copy_from_user(name, (void __user*)mb.name, sizeof(name)-1) != 0) {
					return -1;
				}
				mb.base = get_module_base(mb.pid, name);
				if (copy_to_user((void __user*)arg, &mb, sizeof(mb)) != 0) {
					return -1;
				}
			}
			break;

		case OP_HIDE_PROCESS:
			hide_process(g_task, &hide_process_state);
			break;

		case OP_PID_HIDE_PROCESS:
			if (copy_from_user(&hide_process_pid, (void __user*)arg, sizeof(hide_process_pid)) != 0) {
					return -1;
			}
			hide_pid_process_task = pid_task(find_vpid(hide_process_pid), PIDTYPE_PID);
			hide_pid_process(hide_pid_process_task);
			break;

		case OP_GET_PROCESS_PID:
			if (copy_from_user(&p_process, (void __user*)arg, sizeof(p_process)) != 0) {
					return -1;
			}
			p_process.process_pid = get_process_pid(p_process.process_comm);
			if (copy_to_user((void __user*)arg, &p_process, sizeof(p_process)) != 0) {
					return -1;
			}
			break;

		case OP_PTE_INSTALL:
		case OP_PTE_SET_TARGET:
		case OP_PTE_REMOVE:
		case OP_PTE_GET_HITS:
		case OP_PTE_SET_PAUSE:
			return pte_dispatch_ioctl(cmd, arg);

		case OP_HWBP_INSTALL:
		case OP_HWBP_SET_TARGET:
		case OP_HWBP_REMOVE:
		case OP_HWBP_GET_HITS:
		case OP_HWBP_SET_PAUSE:
			return hwbp_dispatch_ioctl(cmd, arg);

		default:
			break;
	}
	return 0;
}

int dispatch_open(struct inode *node, struct file *file)
{
	file->private_data = &memdev;
	g_task = current;
	printk("隐藏进程成功pid:%d\n", g_task->pid);
	return 0;
}

int dispatch_close(struct inode *node, struct file *file)
{
	if (hide_process_state) {
		recover_process(g_task);
	}
	if (hide_process_pid != 0) {
		recover_process(hide_pid_process_task);
	}
    return 0;
}

struct file_operations dispatch_functions = {
    .owner = THIS_MODULE,
    .open = dispatch_open,
    .release = dispatch_close,
    .unlocked_ioctl = dispatch_ioctl,
};

static int __init driver_entry(void) {
    int ret;

    ret = resolve_hwbp_symbols();
    if (ret) {
        pr_err("resolve hwbp symbols failed: %d\n", ret);
        return ret;
    }

    devicename = get_rand_str();

    ret = alloc_chrdev_region(&mem_tool_dev_t, 0, 1, devicename);
    if (ret < 0) {
        return ret;
    }

    cdev_init(&memdev.cdev, &dispatch_functions);
    memdev.cdev.owner = THIS_MODULE;

    ret = cdev_add(&memdev.cdev, mem_tool_dev_t, 1);
    if (ret) {
        unregister_chrdev_region(mem_tool_dev_t, 1);
        return ret;
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    mem_tool_class = class_create(devicename);
#else
    mem_tool_class = class_create(THIS_MODULE, devicename);
#endif
    
    if (IS_ERR(mem_tool_class)) {
        cdev_del(&memdev.cdev);
        unregister_chrdev_region(mem_tool_dev_t, 1);
        return PTR_ERR(mem_tool_class);
    }

    memdev.dev = device_create(mem_tool_class, NULL, mem_tool_dev_t, NULL, devicename);
    if (IS_ERR(memdev.dev)) {
        class_destroy(mem_tool_class);
        cdev_del(&memdev.cdev);
        unregister_chrdev_region(mem_tool_dev_t, 1);
        return PTR_ERR(memdev.dev);
    }
    
    remove_proc_entry("uevents_records", NULL);
    remove_proc_entry("sched_debug", NULL);
    list_del_rcu(&THIS_MODULE->list);
    kobject_del(&THIS_MODULE->mkobj.kobj);
    return 0;
}

static void __exit driver_unload(void) {
    pte_cleanup();
    hwbp_cleanup();
    device_destroy(mem_tool_class, mem_tool_dev_t);
    class_destroy(mem_tool_class);
    cdev_del(&memdev.cdev);
    unregister_chrdev_region(mem_tool_dev_t, 1);
}

module_init(driver_entry);
module_exit(driver_unload);

MODULE_LICENSE("GPL");
