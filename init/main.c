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
// 0x70是写地址端口号，0x80|addr是读取的CMOS内存地址
// 0x71 是读取数据端口号
#define CMOS_READ(addr) ({ \
outb_p(0x80|addr,0x70); \
inb_p(0x71); \
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
	mem_init(main_memory_start,memory_end); // 主内存区初始化。mm/memory.c
	trap_init();                            // 陷阱门(硬件中断向量)初始化，kernel/traps.c
	blk_dev_init();                         // 块设备初始化,kernel/blk_drv/ll_rw_blk.c
	chr_dev_init();                         // 字符设备初始化, kernel/chr_drv/tty_io.c
	tty_init();                             // tty初始化， kernel/chr_drv/tty_io.c
	time_init();                            // 设置开机启动时间 startup_time
	sched_init();                           // 调度程序初始化(加载任务0的tr,ldtr)(kernel/sched.c)
    // 缓冲管理初始化，建内存链表等。(fs/buffer.c)
	buffer_init(buffer_memory_end);
	hd_init();                              // 硬盘初始化，kernel/blk_drv/hd.c
	floppy_init();                          // 软驱初始化，kernel/blk_drv/floppy.c
	sti();                                  // 所有初始化工作都做完了，开启中断
    // 下面过程通过在堆栈中设置的参数，利用中断返回指令启动任务0执行。
	move_to_user_mode();                    // 移到用户模式下执行
	if (!fork()) {		/* we count on this going ok */
		init();                             // 在新建的子进程(任务1)中执行。
	}
/*
 *   NOTE!!   For any other task 'pause()' would mean we have to get a
 * signal to awaken, but task0 is the sole exception (see 'schedule()')
 * as task 0 gets activated at every idle moment (when no other tasks
 * can run). For task0 'pause()' just means we go check if some other
 * task can run, and if not we return here.
 */
    // pause系统调用会把任务0转换成可中断等待状态，再执行调度函数。但是调度函数只要发现系统中
    // 没有其他任务可以运行是就会切换到任务0，而不依赖于任务0的状态。
	for(;;) pause();
}

// 下面函数产生格式化信息并输出到标准输出设备stdout(1),这里是指屏幕上显示。参数'*fmt'
// 指定输出将采用的格式，具体可以看标准C语言书籍。该子程序正好是vsprintf如何使用一个
// 简单例子。该程序使用vsprintf()将格式化的字符串放入printfbuf缓冲区，然后用write()将
// 缓冲区的内容输出到标准设备(1--stdout).vsprintf()函数实现在kernel/vsprintf.c中。
static int printf(const char *fmt, ...)
{
	va_list args;
	int i;

	va_start(args, fmt);
	write(1,printbuf,i=vsprintf(printbuf, fmt, args));
	va_end(args);
	return i;
}

// 读取并执行/etc/rc文件时所使用的命令行参数和环境参数
static char * argv_rc[] = { "/bin/sh", NULL };      // 调用执行程序时参数字符串数组
static char * envp_rc[] = { "HOME=/", NULL };       // 调用执行程序时环境字符串数组

// 运行登录shell时所使用的命令行参数和环境参数
// 下面 argv[0]中的字符“-”是传递给shell程序sh的一个标志。通过识别该标志，
// sh程序会作为登录shell执行。其执行过程与在shell提示符下执行sh不一样。
static char * argv[] = { "-/bin/sh",NULL };
static char * envp[] = { "HOME=/usr/root", NULL };

// 在main()中已经进行了系统初始化，包括内存管理、各种硬件设备和驱动程序。init()函数
// 运行在任务0第1次创建的子进程(任务1)中。它首先对第一个将要执行的程序(shell)的环境
// 进行初始化，然后以登录shell方式加载该程序并执行。
void init(void)
{
	int pid,i;

    // setup()是一个系统调用。用于读取硬盘参数包括分区表信息并加载虚拟盘(若存在的话)
    // 和安装根文件系统设备。该函数用25行上的宏定义，对应函数是sys_setup()，在块设备
    // 子目录kernel/blk_drv/hd.c中。
	setup((void *) &drive_info);        // drive_info结构是2个硬盘参数表
    // 下面以读写访问方式打开设备"/dev/tty0",它对应终端控制台。由于这是第一次打开文件
    // 操作，因此产生的文件句柄号(文件描述符)肯定是0。该句柄是UNIX类操作系统默认的
    // 控制台标准输入句柄stdin。这里再把它以读和写的方式别人打开是为了复制产生标准输出(写)
    // 句柄stdout和标准出错输出句柄stderr。函数前面的"(void)"前缀用于表示强制函数无需返回值。
	(void) open("/dev/tty0",O_RDWR,0);
	(void) dup(0);                      // 复制句柄，产生句柄1号——stdout标准输出设备
	(void) dup(0);                      // 复制句柄，产生句柄2号——stderr标准出错输出设备
    // 打印缓冲区块数和总字节数，每块1024字节，以及主内存区空闲内存字节数
	printf("%d buffers = %d bytes buffer space\n\r",NR_BUFFERS,
		NR_BUFFERS*BLOCK_SIZE);
	printf("Free mem: %d bytes\n\r",memory_end-main_memory_start);
    // 下面fork()用于创建一个子进程(任务2)。对于被创建的子进程，fork()将返回0值，对于
    // 原进程(父进程)则返回子进程的进程号pid。该子进程关闭了句柄0(stdin)、以只读方式打开
    // /etc/rc文件，并使用execve()函数将进程自身替换成/bin/sh程序(即shell程序)，然后
    // 执行/bin/sh程序。然后执行/bin/sh程序。所携带的参数和环境变量分别由argv_rc和envp_rc
    // 数组给出。关闭句柄0并立即打开/etc/rc文件的作用是把标准输入stdin重定向到/etc/rc文件。
    // 这样shell程序/bin/sh就可以运行rc文件中的命令。由于这里的sh的运行方式是非交互的，
    // 因此在执行完rc命令后就会立刻退出，进程2也随之结束。
    // _exit()退出时出错码1 - 操作未许可；2 - 文件或目录不存在。
	if (!(pid=fork())) {
		close(0);
		if (open("/etc/rc",O_RDONLY,0))
			_exit(1);                       // 如果打开文件失败，则退出(lib/_exit.c)
		execve("/bin/sh",argv_rc,envp_rc);  // 替换成/bin/sh程序并执行
		_exit(2);                           // 若execve()执行失败则退出。
	}
    // 下面还是父进程(1)执行语句。wait()等待子进程停止或终止，返回值应是子进程的进程号(pid).
    // 这三句的作用是父进程等待子进程的结束。&i是存放返回状态信息的位置。如果wait()返回值
    // 不等于子进程号，则继续等待。
	if (pid>0)
		while (pid != wait(&i))
			/* nothing */;
    // 如果执行到这里，说明刚创建的子进程的执行已停止或终止了。下面循环中首先再创建
    // 一个子进程，如果出错，则显示“初始化程序创建子进程失败”信息并继续执行。对于所
    // 创建的子进程将关闭所有以前还遗留的句柄(stdin, stdout, stderr),新创建一个会话
    // 并设置进程组号，然后重新打开/dev/tty0作为stdin,并复制成stdout和sdterr.再次
    // 执行系统解释程序/bin/sh。但这次执行所选用的参数和环境数组另选了一套。然后父
    // 进程再次运行wait()等待。如果子进程又停止了执行，则在标准输出上显示出错信息
    // “子进程pid挺直了运行，返回码是i”,然后继续重试下去....，形成一个“大”循环。
    // 此外，wait()的另外一个功能是处理孤儿进程。如果一个进程的父进程先终止了，那么
    // 这个进程的父进程就会被设置为这里的init进程(进程1)，并由init进程负责释放一个
    // 已终止进程的任务数据结构等资源。
	while (1) {
		if ((pid=fork())<0) {
			printf("Fork failed in init\r\n");
			continue;
		}
		if (!pid) {                                 // 新的子进程
			close(0);close(1);close(2);
			setsid();                               // 创建一新的会话期
			(void) open("/dev/tty0",O_RDWR,0);
			(void) dup(0);
			(void) dup(0);
			_exit(execve("/bin/sh",argv,envp));
		}
		while (1)
			if (pid == wait(&i))
				break;
		printf("\n\rchild %d died with code %04x\n\r",pid,i);
		sync();                                     // 同步操作，刷新缓冲区。
	}
    // _exit()和exit()都用于正常终止一个函数。但_exit()直接是一个sys_exit系统调用，
    // 而exit()则通常是普通函数库中的一个函数。它会先执行一些清除操作，例如调用
    // 执行各终止处理程序、关闭所有标准IO等，然后调用sys_exit。
	_exit(0);	/* NOTE! _exit, not exit() */
}
