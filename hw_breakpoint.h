 *
 * 硬件断点追踪模块
 * 使用 Linux hw_breakpoint 框架（ARM64 调试寄存器 BRP0-15）
 * 原理：在目标地址注册执行断点 → CPU 执行到该地址触发调试异常 → 回调中改写 V3/V4/V5 → 内核自动单步跳过 → 继续
 */
#ifndef HW_BREAKPOINT_TRACK_H
#define HW_BREAKPOINT_TRACK_H

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <linux/uaccess.h>
#include <asm/ptrace.h>
#include <asm/current.h>
#include <linux/kprobes.h>

/* 函数指针类型 */
typedef struct perf_event *(*reg_user_hwbp_t)(struct perf_event_attr *attr,
                                              perf_overflow_handler_t triggered,
                                              void *context,
                                              struct task_struct *tsk);
typedef void (*unreg_hwbp_t)(struct perf_event *bp);

/* 全局函数指针，用 p_ 前缀区分原函数 */
static reg_user_hwbp_t p_register_user_hw_breakpoint;
static unreg_hwbp_t p_unregister_hw_breakpoint;

/* 使用 kprobe 动态解析未导出符号地址 */
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

/* ===== ioctl 命令码 ===== */
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

/* ===== 全局状态 ===== */
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

/* ===== 改写浮点寄存器 ===== */
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

/* ===== 断点回调函数 ===== */
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

/* ===== 安装硬件断点 ===== */
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

/* ===== 移除硬件断点 ===== */
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

/* ===== 暂停/恢复 ===== */
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

/* ===== ioctl 分发 ===== */
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

/* 模块退出时清理 */
static void hwbp_cleanup(void)
{
    mutex_lock(&hwbp_lock);
    hwbp_do_remove();
    mutex_unlock(&hwbp_lock);
}

#endif /* HW_BREAKPOINT_TRACK_H */
