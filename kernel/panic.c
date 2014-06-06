/*
 *  linux/kernel/panic.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * This function is used through-out the kernel (includeinh mm and fs)
 * to indicate a major problem.
 */
#define PANIC

// 内核头文件，包括一些内核常用的函数原型定义
#include <linux/kernel.h>
// 调度程序头文件，定义了任务结构task_struct、初始任务0的数据，
// 还有一些有关描述符参数设置和获取的嵌入式汇编函数宏语句
#include <linux/sched.h>

void sys_sync(void);	/* it's really int */

// 该函数用来显示内核中出现重大错误信息，并运行文件系统同步函数，然后进入
// 死循环——死机。如果当前进程是任务0的话，还说明是交换任务出错，
// 并且还没有运行文件系统同步函数。函数名前的关键字 volatile 用于告诉编译器gcc
// 该函数不会返回。这样可以让gcc产生更好的代码，更重要的是使用这个关键字可以避免
// 产生某些（未初始化变量的）假警告信息。
// 等同于现在gcc的函数属性说明：void panic(const char *s) __attribute__((noreturn));
volatile void panic(const char * s)
{
	printk("Kernel panic: %s\n\r",s);
	if (current == task[0])
		printk("In swapper task - not syncing\n\r");
	else
		sys_sync();
	for(;;);
}
