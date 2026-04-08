#include <linux/ktime.h>
#include <linux/timekeeping.h>
#include <linux/smp.h>

#include <linux/module.h>
#include <linux/sched/signal.h>
#include <linux/proc_fs.h>
#include <linux/mm.h>
#include <linux/delay.h>
#include <linux/slab.h>

#include "uh.h"

#define CPU_MAX 8
#define APP_MAX 6

#define UH_STAT_INIT    0x0D
#define UH_STAT_EXIT    0x0F

/* BUF define */
#define UH_STAT_SIZE    12288
#define RKP_LINE_MAX    80
#define WRITE_BUF_SIZE 32

#define CALL_TRAP_COUNT 200

static char uh_stat_buf[UH_STAT_SIZE];
static unsigned long uh_stat_len;
static u64 *ha1;

void rkp_buf_print(const char *fmt, ...)
{
	va_list aptr;

	if (uh_stat_len > UH_STAT_SIZE - RKP_LINE_MAX) {
		pr_err("UH STAT: Error Maximum buf");
		return;
	}
	va_start(aptr, fmt);
	uh_stat_len += vscnprintf(uh_stat_buf + uh_stat_len, (size_t)UH_STAT_SIZE - uh_stat_len, fmt, aptr);
	va_end(aptr);
}

static u64 get_ro_trap_delay(int test_cnt)
{
	u64 total_delay = 0, start, end;
	int i;

	for (i = 0; i < test_cnt; i++) {
		start = ktime_get_ns();
		// write ro page
		*ha1 = 0x1234;
		end = ktime_get_ns();
		total_delay += (end - start);
	}

	return total_delay;
}

int test_case_print_ro_trap_stat(void)
{
	unsigned long flags;
	int cur_cpu_id;
	u32 delay;

	local_irq_save(flags);
	cur_cpu_id = smp_processor_id();
	delay = get_ro_trap_delay(CALL_TRAP_COUNT);
	local_irq_restore(flags);

	rkp_buf_print("[RO][%d] total-cnt: %5d, total-avg: %10llu ns\n",
			cur_cpu_id, CALL_TRAP_COUNT, delay / CALL_TRAP_COUNT);

	return 0;
}

static u64 get_hvc_delay(int test_cnt)
{
	u64 total_delay = 0, start, end;
	int i;

	for (i = 0; i < test_cnt; i++) {
		start = ktime_get_ns();
		// hvc call
		uh_call(UH_APP_RKP, 0x21, 0, 0, 0, 0);
		end = ktime_get_ns();
		total_delay += (end - start);
	}

	return total_delay;
}

int test_case_print_hvc_stat(void)
{
	unsigned long flags;
	int cur_cpu_id;
	u32 delay;

	local_irq_save(flags);
	cur_cpu_id = smp_processor_id();
	delay = get_hvc_delay(CALL_TRAP_COUNT);
	local_irq_restore(flags);

	rkp_buf_print("[hvc][%d] total-cnt: %5d, total-avg: %10llu ns\n",
			cur_cpu_id, CALL_TRAP_COUNT, delay / CALL_TRAP_COUNT);

	return 0;
}

static u64 get_tvm_trap_delay(int test_cnt)
{
	u64 total_delay = 0, val, start, end;
	int i;

	for (i = 0; i < test_cnt; i++) {
		asm volatile("mrs %0,far_el1" : "=r"(val) :: "memory");
		start = ktime_get_ns();
		asm volatile("msr far_el1,%0" :: "r"(val));
		end = ktime_get_ns();
		total_delay += (end - start);
	}

	return total_delay;
}

int test_case_print_tvm_stat(void)
{
	unsigned long flags;
	char test_reg[15] = "far_el1";
	int cur_cpu_id;
	u32 delay;

	local_irq_save(flags);
	cur_cpu_id = smp_processor_id();
	delay = get_tvm_trap_delay(CALL_TRAP_COUNT);
	local_irq_restore(flags);

	rkp_buf_print("[TVM][%d] %14s, total-cnt: %5d, total-avg: %10llu ns\n",
			cur_cpu_id, test_reg, CALL_TRAP_COUNT, delay / CALL_TRAP_COUNT);

	return 0;
}

ssize_t uh_stat_read(struct file *filep, char __user *buffer, size_t count, loff_t *ppos)
{
	uh_stat_len = 0;

	test_case_print_tvm_stat();
	test_case_print_ro_trap_stat();
	test_case_print_hvc_stat();

	return simple_read_from_buffer(buffer, count, ppos, uh_stat_buf, uh_stat_len);
}

static ssize_t uh_stat_write(struct file *file, const char __user *buf_from_user,
			size_t count, loff_t *ppos)
{
	char buf[WRITE_BUF_SIZE];
	int buf_size;

	if (WRITE_BUF_SIZE < count)
		buf_size = WRITE_BUF_SIZE;
	else
		buf_size = count;

	if (copy_from_user(buf, buf_from_user, buf_size))
		return -EFAULT;

	return buf_size;
}

static const struct proc_ops uh_proc_fops = {
	.proc_read = uh_stat_read,
	.proc_write = uh_stat_write,
};

static int __init uh_stat_init(void)
{
	u64 va;

	if (proc_create("uh_stat", 0644, NULL, &uh_proc_fops) == NULL) {
		pr_err("UH_STAT: Error creating proc entry");
		return -1;
	}

	va = __get_free_page(GFP_KERNEL | __GFP_ZERO);
	if (!va)
		return -1;

	uh_call(UH_APP_RKP, UH_STAT_INIT, va, 0, 0, 1);
	ha1 = (u64 *)va;

	return 0;
}

static void __exit uh_stat_exit(void)
{
	remove_proc_entry("uh_stat", NULL);
}

module_init(uh_stat_init);
module_exit(uh_stat_exit);

MODULE_AUTHOR("MoonCheol Kang <mcneo.kang@samsung.com>");
MODULE_DESCRIPTION("UH STAT driver");
MODULE_LICENSE("GPL");
