/*
 *  linux/init/main.c
 *
 *  (C) 1991  Linus Torvalds
 */

// 宏定义"__LIBRARY__" 是为了包括定义再unistd.h中的内嵌汇编代码等信息。
#define __LIBRARY__
// *.h头文件所在的默认目录是include/，则再代码中就不用明确指明其位置。
// 如果不是unix的标准头文件，则需要指明所在的目录，并用双引号括住。
// unistd.h是标准符号常数与类型头文件。其中定义了各种符号常数和类型，
// 并声明了各种函数。如果还定义了符号__LIBRARY__,则还会包含系统调用和
// 内嵌汇编代码syscall10()等。
#include <unistd.h>
#include <time.h>       // 时间类型头文件。其中主要定义了tm结构和一些有关时间的函数原型

/*
 * we need this inline - forking from kernel space will result
 * in NO COPY ON WRITE (!!!), until an execve is executed. This
 * is no problem, but for the stack. This is handled by not letting
 * main() use the stack at all after fork(). Thus, no function
 * calls - which means inline code for fork too, as otherwise we
 * would use the stack upon exit from 'fork()'.
 *
 * Actually only pause and fork are needed inline, so that there
 * won't be any messing with the stack from main(), but we define
 * some others too.
 */
// Linux在内核空间创建进程时不使用写时复制技术(Copy on write).main()在移动到用户
// 模式（到任务0）后执行内嵌方式的fork()和pause(),因此可保证不适用任务0的用户栈。
// 在执行moveto_user_mode()之后，本程序main()就以任务0的身份在运行了。而任务0是
// 所有将将创建子进程的父进程。当它创建ygie子进程时(init进程)，由于任务1代码属于
// 内核空间，因此没有使用写时复制功能。此时任务0的用户栈就是任务1的用户栈，即它们
// 共同使用一个栈空间。因此希望在main.c运行在任务0的环境下不要有对堆栈的任何操作，
// 以免弄乱堆栈。而在再次执行fork()并执行过execve()函数后，被加载程序已不属于内核空间
// 因此可以使用写时复制技术了。
//
// 下面_syscall0()是unistd.h中的内嵌宏代码。以嵌入汇编的形式调用Linux的系统调用中断
// 0x80.该中断是所有系统调用的入口。该条语句实际上是int fork()创建进程系统调用。可展
// 开看之就会立刻明白。syscall0名称中最后的0表示无参数，1表示1个参数。
static inline _syscall0(int,fork)
// int pause() 系统调用，暂停进程的执行，直到收到一个信号
static inline _syscall0(int,pause)
// int setup(void * BIOS)系统调用，仅用于linux初始化(仅在这个程序中被调用)
static inline _syscall1(int,setup,void *,BIOS)
// int sync()系统调用：更新文件系统。
static inline _syscall0(int,sync)

// tty头文件，定义了有关tty_io, 串行通信方面的参数、常数
#include <linux/tty.h>
// 调度程序头文件，定义了任务结构task_struct、第1个初始任务的数据。还有一些以宏的形式
// 定义的有关描述符参数设置和获取的嵌入式汇编函数程序。
#include <linux/sched.h>
#include <linux/head.h>
// 以宏的形式定义了许多有关设置或修改描述符/中断门等嵌入式汇编子程序
#include <asm/system.h>
// 以宏的嵌入式汇编程序形式定义对IO端口操作的函数
#include <asm/io.h>

#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
// 用于文件及描述符的操作控制常数符号的定义
#include <fcntl.h>
#include <sys/types.h>
// 定义文件结构(file,buffer_head,m_inode等)
#include <linux/fs.h>

// 用于内核显示信息的缓存
static char printbuf[1024];

extern int vsprintf();
extern void init(void);
extern void blk_dev_init(void);
extern void chr_dev_init(void);
extern void hd_init(void);
extern void floppy_init(void);
extern void mem_init(long start, long end);
// 虚拟盘初始化
extern long rd_init(long mem_start, int length);
extern long kernel_mktime(struct tm * tm);      //计算系统开始启动时间（秒）
extern long startup_time;       // 内核启动时间（开机时间）（秒）

/*
 * This is set up by the setup-routine at boot-time
 */
// 下面三行分别将指定的线性地址强行转换为给定数据类型的指针，并获取指针所指
// 的内容。由于内核代码段被映射到从物理地址零开始的地方，因此这些线性地址
// 正好也是对应的物理地址。这些指定地址处内存值的含义请参见setup程序读取并保存的参数。
#define EXT_MEM_K (*(unsigned short *)0x90002)
#define DRIVE_INFO (*(struct drive_info *)0x90080)
#define ORIG_ROOT_DEV (*(unsigned short *)0x901FC)

/*
 * Yeah, yeah, it's ugly, but I cannot find how to do this correctly
 * and this seems to work. I anybody has more info on the real-time
 * clock I'd be interested. Most of this was trial and error, and some
 * bios-listing reading. Urghh.
 */
// 这段宏读取CMOS实时时钟信息，outb_p,inb_p是include/asm/io.h中定义的端口输入输出宏
#define CMOS_READ(addr) ({ \
outb_p(0x80|addr,0x70); \       // 0x70是写地址端口号，0x80|addr是读取的CMOS内存地址
inb_p(0x71); \                  // 0x71 是读取数据端口号
})

// 将BCD码转换成二进制数值。BCD码利用半个字节（4 bit）表示一个10进制数，因此
// 一个字节表示2个10进制数。（val）&15取BCD表示10进制个位数，而(val)>>4 取BCD表示
// 的10进制十位数，再乘以10.因此最后两者相加就是一个字节BCD码的实际二进制数值。
#define BCD_TO_BIN(val) ((val)=((val)&15) + ((val)>>4)*10)

// 该函数取CMOS实时时钟信息作为开机时间，并保存到全局变量startup_time（秒）中。
// kernel_mktime()用于计算从1970年1月1号0时起到开机当日经过的秒数，作为开机时间。
static void time_init(void)
{
	struct tm time;

    // CMOS的访问速度很慢，为了减少时间误差，在读取了下面循环中的所有数值后，如果此时
    // CMOS中秒值发生了变化，那么就重新读取所有值。这样内核就能把与CMOS时间误差控制在1秒之内。
	do {
		time.tm_sec = CMOS_READ(0);
		time.tm_min = CMOS_READ(2);
		time.tm_hour = CMOS_READ(4);
		time.tm_mday = CMOS_READ(7);
		time.tm_mon = CMOS_READ(8);
		time.tm_year = CMOS_READ(9);
	} while (time.tm_sec != CMOS_READ(0));
	BCD_TO_BIN(time.tm_sec);
	BCD_TO_BIN(time.tm_min);
	BCD_TO_BIN(time.tm_hour);
	BCD_TO_BIN(time.tm_mday);
	BCD_TO_BIN(time.tm_mon);
	BCD_TO_BIN(time.tm_year);
	time.tm_mon--;                              // tm_mon中月份的范围是0-11
	startup_time = kernel_mktime(&time);        // 计算开机时间。kernel/mktime.c文件
}

// 下面定义一些局部变量
static long memory_end = 0;                     // 机器具有的物理内存容量（字节数）
static long buffer_memory_end = 0;              // 高速缓冲区末端地址
static long main_memory_start = 0;              // 主内存（将用于分页）开始的位置

struct drive_info { char dummy[32]; } drive_info;  // 用于存放硬盘参数表信息

// 内核初始化主程序。初始化结束后将以任务0（idle任务即空闲任务）的身份运行。
void main(void)		/* This really IS void, no error here. */
{			/* The startup routine assumes (well, ...) this */
/*
 * Interrupts are still disabled. Do necessary setups, then
 * enable them
 */
    // 下面这段代码用于保存：
    // 根设备号 ->ROOT_DEV；高速缓存末端地址->buffer_memory_end;
    // 机器内存数->memory_end；主内存开始地址->main_memory_start；
    // 其中ROOT_DEV已在前面包含进的fs.h文件中声明为extern int
 	ROOT_DEV = ORIG_ROOT_DEV;
 	drive_info = DRIVE_INFO;        // 复制0x90080处的硬盘参数
	memory_end = (1<<20) + (EXT_MEM_K<<10);     // 内存大小=1Mb + 扩展内存(k)*1024 byte
	memory_end &= 0xfffff000;                   // 忽略不到4kb(1页)的内存数
	if (memory_end > 16*1024*1024)              // 内存超过16Mb，则按16Mb计
		memory_end = 16*1024*1024;
	if (memory_end > 12*1024*1024)              // 如果内存>12Mb,则设置缓冲区末端=4Mb 
		buffer_memory_end = 4*1024*1024;
	else if (memory_end > 6*1024*1024)          // 否则若内存>6Mb,则设置缓冲区末端=2Mb
		buffer_memory_end = 2*1024*1024;
	else
		buffer_memory_end = 1*1024*1024;        // 否则设置缓冲区末端=1Mb
	main_memory_start = buffer_memory_end;
    // 如果在Makefile文件中定义了内存虚拟盘符号RAMDISK,则初始化虚拟盘。此时主内存将减少。
#ifdef RAMDISK
	main_memory_start += rd_init(main_memory_start, RAMDISK*1024);
#endif
    // 以下是内核进行所有方面的初始化工作。阅读时最好跟着调用的程序深入进去看，若实在
    // 看不下去了，就先放一放，继续看下一个初始化调用。——这是经验之谈。o(∩_∩)o 。;-)
	mem_init(main_memory_start,memory_end);
	trap_init();
	blk_dev_init();
	chr_dev_init();
	tty_init();
	time_init();
	sched_init();
	buffer_init(buffer_memory_end);
	hd_init();
	floppy_init();
	sti();
	move_to_user_mode();
	if (!fork()) {		/* we count on this going ok */
		init();
	}
/*
 *   NOTE!!   For any other task 'pause()' would mean we have to get a
 * signal to awaken, but task0 is the sole exception (see 'schedule()')
 * as task 0 gets activated at every idle moment (when no other tasks
 * can run). For task0 'pause()' just means we go check if some other
 * task can run, and if not we return here.
 */
	for(;;) pause();
}

static int printf(const char *fmt, ...)
{
	va_list args;
	int i;

	va_start(args, fmt);
	write(1,printbuf,i=vsprintf(printbuf, fmt, args));
	va_end(args);
	return i;
}

static char * argv_rc[] = { "/bin/sh", NULL };
static char * envp_rc[] = { "HOME=/", NULL };

static char * argv[] = { "-/bin/sh",NULL };
static char * envp[] = { "HOME=/usr/root", NULL };

void init(void)
{
	int pid,i;

	setup((void *) &drive_info);
	(void) open("/dev/tty0",O_RDWR,0);
	(void) dup(0);
	(void) dup(0);
	printf("%d buffers = %d bytes buffer space\n\r",NR_BUFFERS,
		NR_BUFFERS*BLOCK_SIZE);
	printf("Free mem: %d bytes\n\r",memory_end-main_memory_start);
	if (!(pid=fork())) {
		close(0);
		if (open("/etc/rc",O_RDONLY,0))
			_exit(1);
		execve("/bin/sh",argv_rc,envp_rc);
		_exit(2);
	}
	if (pid>0)
		while (pid != wait(&i))
			/* nothing */;
	while (1) {
		if ((pid=fork())<0) {
			printf("Fork failed in init\r\n");
			continue;
		}
		if (!pid) {
			close(0);close(1);close(2);
			setsid();
			(void) open("/dev/tty0",O_RDWR,0);
			(void) dup(0);
			(void) dup(0);
			_exit(execve("/bin/sh",argv,envp));
		}
		while (1)
			if (pid == wait(&i))
				break;
		printf("\n\rchild %d died with code %04x\n\r",pid,i);
		sync();
	}
	_exit(0);	/* NOTE! _exit, not exit() */
}
