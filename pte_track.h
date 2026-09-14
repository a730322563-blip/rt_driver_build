/*
 * PTE 追踪模块
 * 基于 rt 驱动的 translate_linear_address + 物理内存读写
 * 原理：将目标地址所在页的 PTE 设为无效 → 触发 page fault → kprobe 拦截 → 改写 V3/V4/V5 → 恢复 PTE → 单步 → 重新无效
 */
#ifndef PTE_TRACK_H
#define PTE_TRACK_H

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/kprobes.h>
#include <linux/uaccess.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>
#include <asm/ptrace.h>
#include <asm/current.h>

#include "memory.h"

/* ===== ioctl 命令码（在 comm.h 的 OPERATIONS 里也加了对应值） ===== */
#define OP_PTE_INSTALL    0x810
#define OP_PTE_SET_TARGET 0x811
#define OP_PTE_REMOVE     0x812
#define OP_PTE_GET_HITS   0x813
#define OP_PTE_SET_PAUSE  0x814

#define PTE_MAX_FP_REGS 8

struct pte_install_req {
    pid_t pid;
    uint64_t addr;
    uint32_t active;
};

struct pte_target_req {
    uint64_t addr;
    uint32_t active;
    uint32_t fp_count;
    uint32_t fp_indices[PTE_MAX_FP_REGS];
    uint64_t fp_values[PTE_MAX_FP_REGS][2];
};

/* ===== 全局状态 ===== */
static struct {
    bool installed;
    bool paused;
    pid_t pid;
    uint64_t target_addr;
    uint64_t page_addr;
    pte_t *target_ptep;
    pte_t original_pte;
    bool pte_invalidated;

    bool active;
    uint32_t fp_count;
    uint32_t fp_indices[PTE_MAX_FP_REGS];
    uint64_t fp_values[PTE_MAX_FP_REGS][2];

    uint64_t hit_count;
} g_pte_state;

static DEFINE_MUTEX(pte_lock);

/* ===== 页表遍历：找到虚拟地址对应的 PTE 指针 ===== */
static pte_t *pte_find_ptep(struct mm_struct *mm, unsigned long addr)
{
    pgd_t *pgdp;
    p4d_t *p4dp;
    pud_t *pudp;
    pmd_t *pmdp;
    pte_t *ptep;

    if (!mm) return NULL;

    pgdp = pgd_offset(mm, addr);
    if (pgd_none(*pgdp) || pgd_bad(*pgdp)) return NULL;

    p4dp = p4d_offset(pgdp, addr);
    if (p4d_none(*p4dp) || p4d_bad(*p4dp)) return NULL;

    pudp = pud_offset(p4dp, addr);
    if (pud_none(*pudp) || pud_bad(*pudp)) return NULL;

    pmdp = pmd_offset(pudp, addr);
    if (pmd_none(*pmdp) || pmd_bad(*pmdp)) return NULL;

    ptep = pte_offset_kernel(pmdp, addr);
    if (!ptep || pte_none(*ptep)) return NULL;

    return ptep;
}

/* ===== 使目标页 PTE 无效 ===== */
static void pte_invalidate(void)
{
    if (!g_pte_state.target_ptep || g_pte_state.pte_invalidated)
        return;
    g_pte_state.original_pte = *g_pte_state.target_ptep;
    {
        pte_t invalid = g_pte_state.original_pte;
        pte_val(invalid) &= ~(_AT(pteval_t, 1)); /* 清除 present 位 */
        set_pte(g_pte_state.target_ptep, invalid);
    }
    g_pte_state.pte_invalidated = true;
    flush_tlb_all();
}

/* ===== 恢复目标页 PTE ===== */
static void pte_restore(void)
{
    if (!g_pte_state.target_ptep || !g_pte_state.pte_invalidated)
        return;
    set_pte(g_pte_state.target_ptep, g_pte_state.original_pte);
    g_pte_state.pte_invalidated = false;
    flush_tlb_all();
}

/* ===== 改写浮点寄存器（V3/V4/V5） ===== */
static void pte_modify_fp(struct pt_regs *regs)
{
    struct task_struct *tsk = current;
    struct user_fpsimd_state *fpsimd;
    int i;

    if (!g_pte_state.active || g_pte_state.fp_count == 0)
        return;

    fpsimd = &tsk->thread.uw.fpsimd_state;
    if (!fpsimd) return;

    for (i = 0; i < g_pte_state.fp_count && i < PTE_MAX_FP_REGS; i++) {
        uint32_t idx = g_pte_state.fp_indices[i];
        if (idx >= 32) continue;
        fpsimd->vregs[idx] = g_pte_state.fp_values[i][0] |
                             ((__uint128_t)g_pte_state.fp_values[i][1] << 64);
    }
}

/* ===== kprobe: do_page_fault 前置处理 =====
 * ARM64 6.x: do_page_fault(unsigned long far, unsigned long esr, struct pt_regs *regs)
 * 注意：不同内核版本参数顺序可能不同，需根据实际调整
 */
static int pte_fault_pre(struct kprobe *p, struct pt_regs *regs)
{
    unsigned long far  = regs->regs[0];
    unsigned long esr  = regs->regs[1];
    struct pt_regs *user_regs = (struct pt_regs *)regs->regs[2];

    /* 只处理目标进程 */
    if (current->pid != g_pte_state.pid)
        return 0;

    /* 只处理目标页 */
    if ((far & PAGE_MASK) != g_pte_state.page_addr)
        return 0;

    /* 只处理指令访问异常（ESR_ELx_EC = 0x21 = INST_ABORT_LOWER） */
    if ((esr & 0xFC) != 0x20)
        return 0;

    g_pte_state.hit_count++;

    /* 精确命中目标地址时改写寄存器 */
    if (g_pte_state.active && far == g_pte_state.target_addr) {
        pte_modify_fp(user_regs);
    }

    /* 恢复 PTE 让指令能执行 */
    pte_restore();

    /* 设置单步标志，执行完一条指令后重新使 PTE 无效 */
    user_regs->pstate |= (1UL << 21); /* PSR_SS_BIT，arm64 单步标志 */

    return 0;
}

/* ===== kprobe: do_debug_exception（单步完成后重新无效化 PTE） ===== */
static int pte_debug_pre(struct kprobe *p, struct pt_regs *regs)
{
    if (current->pid != g_pte_state.pid)
        return 0;
    if (!g_pte_state.pte_invalidated && g_pte_state.installed && !g_pte_state.paused) {
        pte_invalidate();
    }
    return 0;
}

static struct kprobe pte_fault_kp = {
    .symbol_name = "do_page_fault",
    .pre_handler = pte_fault_pre,
};

static struct kprobe pte_debug_kp = {
    .symbol_name = "do_debug_exception",
    .pre_handler = pte_debug_pre,
};

/* ===== 安装 PTE 追踪 ===== */
static int pte_do_install(struct pte_install_req *req)
{
    struct task_struct *tsk;
    struct mm_struct *mm;
    pte_t *ptep;
    int ret;

    if (g_pte_state.installed)
        return -EALREADY;

    tsk = pid_task(find_vpid(req->pid), PIDTYPE_PID);
    if (!tsk) return -ESRCH;

    mm = get_task_mm(tsk);
    if (!mm) return -ESRCH;

    mmap_read_lock(mm);
    ptep = pte_find_ptep(mm, req->addr);
    mmap_read_unlock(mm);

    if (!ptep) {
        mmput(mm);
        pr_err("pte: cannot find PTE for addr 0x%llx\n", req->addr);
        return -EFAULT;
    }

    g_pte_state.pid = req->pid;
    g_pte_state.target_addr = req->addr;
    g_pte_state.page_addr = req->addr & PAGE_MASK;
    g_pte_state.target_ptep = ptep;
    g_pte_state.original_pte = *ptep;
    g_pte_state.pte_invalidated = false;
    g_pte_state.active = req->active;
    g_pte_state.hit_count = 0;
    g_pte_state.paused = false;
    g_pte_state.installed = true;

    mmput(mm);

    /* 注册 kprobe */
    ret = register_kprobe(&pte_fault_kp);
    if (ret) {
        pr_err("pte: register do_page_fault kprobe failed: %d\n", ret);
        g_pte_state.installed = false;
        g_pte_state.target_ptep = NULL;
        return ret;
    }

    ret = register_kprobe(&pte_debug_kp);
    if (ret) {
        pr_err("pte: register do_debug_exception kprobe failed: %d\n", ret);
        unregister_kprobe(&pte_fault_kp);
        g_pte_state.installed = false;
        g_pte_state.target_ptep = NULL;
        return ret;
    }

    /* 开始捕获 */
    pte_invalidate();

    pr_info("pte: installed pid=%d addr=0x%llx\n", g_pte_state.pid, g_pte_state.target_addr);
    return 0;
}

/* ===== 移除 PTE 追踪 ===== */
static void pte_do_remove(void)
{
    if (!g_pte_state.installed) return;

    pte_restore();
    unregister_kprobe(&pte_fault_kp);
    unregister_kprobe(&pte_debug_kp);

    g_pte_state.installed = false;
    g_pte_state.target_ptep = NULL;
    g_pte_state.pte_invalidated = false;

    pr_info("pte: removed, hits=%llu\n", g_pte_state.hit_count);
}

/* ===== ioctl 分发（在 entry.c 的 dispatch_ioctl 里调用） ===== */
static long pte_dispatch_ioctl(unsigned int cmd, unsigned long arg)
{
    void __user *argp = (void __user *)arg;
    int ret = 0;

    mutex_lock(&pte_lock);

    switch (cmd) {
    case OP_PTE_INSTALL: {
        struct pte_install_req req;
        if (copy_from_user(&req, argp, sizeof(req))) { ret = -EFAULT; break; }
        ret = pte_do_install(&req);
        break;
    }
    case OP_PTE_SET_TARGET: {
        struct pte_target_req req;
        int i;
        if (copy_from_user(&req, argp, sizeof(req))) { ret = -EFAULT; break; }
        if (!g_pte_state.installed) { ret = -ENODEV; break; }
        g_pte_state.active = req.active;
        g_pte_state.fp_count = req.fp_count > PTE_MAX_FP_REGS ? PTE_MAX_FP_REGS : req.fp_count;
        for (i = 0; i < g_pte_state.fp_count; i++) {
            g_pte_state.fp_indices[i] = req.fp_indices[i];
            g_pte_state.fp_values[i][0] = req.fp_values[i][0];
            g_pte_state.fp_values[i][1] = req.fp_values[i][1];
        }
        break;
    }
    case OP_PTE_REMOVE:
        pte_do_remove();
        break;
    case OP_PTE_GET_HITS:
        if (copy_to_user(argp, &g_pte_state.hit_count, sizeof(uint64_t)))
            ret = -EFAULT;
        break;
    case OP_PTE_SET_PAUSE: {
        int pause;
        if (copy_from_user(&pause, argp, sizeof(int))) { ret = -EFAULT; break; }
        g_pte_state.paused = !!pause;
        if (g_pte_state.paused) pte_restore();
        else pte_invalidate();
        break;
    }
    default:
        ret = -ENOTTY;
    }

    mutex_unlock(&pte_lock);
    return ret;
}

/* 模块退出时清理（在 driver_unload 里调用） */
static void pte_cleanup(void)
{
    mutex_lock(&pte_lock);
    pte_do_remove();
    mutex_unlock(&pte_lock);
}

#endif /* PTE_TRACK_H */
