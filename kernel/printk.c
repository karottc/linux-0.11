/*
 *  linux/kernel/printk.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * When in kernel-mode, we cannot use printf, as fs is liable to
 * point to 'interesting' things. Make a printf with fs-saving, and
 * all is well.
 */
#include <stdarg.h>
#include <stddef.h>  //定义了NULL，offsetof(TYPE, MEMBER)

#include <linux/kernel.h>

static char buf[1024];      // 显示用临时缓冲区

// vsprintf 定义在vsprintf.c中
extern int vsprintf(char * buf, const char * fmt, va_list args);

// 内核使用的显示函数
// 只能在内核代码中使用。由于实际调用的显示函数tty_write()默认使用的显示数据在
// 段寄存器fs所指向的用户数据区中，因此这里需要暂时保存fs,并让fs指向内核数据段。
// 在显示完之后再恢复原fs段的内容。
int printk(const char *fmt, ...)
{
	va_list args;       // va_list是一个字符指针类型
	int i;

    // 运行参数处理开始函数。然后使用格式串fmt将参数列表args输出到buf中，返回值i等于
    // 输出字符串的长度，再运行参数处理结束函数。
	va_start(args, fmt);
	i=vsprintf(buf,fmt,args);
	va_end(args);
	__asm__("push %%fs\n\t"         // 保存fs
		"push %%ds\n\t"
		"pop %%fs\n\t"              // 令fs = ds
		"pushl %0\n\t"              // 将字符串长度压入堆栈（这三个入栈是调用参数）
		"pushl $buf\n\t"            // 将buf的地址压入堆栈
		"pushl $0\n\t"              // 将数值0压入堆栈，是显示通道号 channel
		"call tty_write\n\t"        // 调用tty_write函数（tty_io.c）
		"addl $8,%%esp\n\t"         // 跳过（丢弃）两个入栈参数（buf, channel）
		"popl %0\n\t"               // 弹出字符串长度值，作为返回值
		"pop %%fs"                  // 恢复原fs寄存器
		::"r" (i):"ax","cx","dx");  // 通知编译器，寄存器ax,cx,dx值肯能已经改变。
	return i;                       // 返回字符串长度
}
