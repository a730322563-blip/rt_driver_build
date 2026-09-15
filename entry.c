#include <linux/module.h>
#include <linux/tty.h>
#include <linux/miscdevice.h>
#include <linux/proc_fs.h>
#include "comm.h"
#include "memory.h"
#include "process.h"
#include "hide_process.h"
#include "pte_track.h"
#include "hw_breakpoint.h"

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0))
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
#endif

extern struct task_struct *task;
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
			hide_process(task, &hide_process_state);
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
	task = current;
	printk("隐藏进程成功pid:%d\n", task->pid);
	return 0;
}

int dispatch_close(struct inode *node, struct file *file)
{
	if (hide_process_state) {
		recover_process(task);
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

int resolve_hwbp_symbols(void);

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
