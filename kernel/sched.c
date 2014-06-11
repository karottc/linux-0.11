/*
 *  linux/kernel/sched.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * 'sched.c' is the main kernel file. It contains scheduling primitives
 * (sleep_on, wakeup, schedule etc) as well as a number of simple system
 * call functions (type getpid(), which just extracts a field from
 * current-task
 */
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/sys.h>
#include <linux/fdreg.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

#include <signal.h>

// 该宏取信号nr在信号位图中对应位的二进制数值。信号编号1-32.比如信号5的位图
// 数值等于 1 <<(5-1) = 16 = 00010000b
#define _S(nr) (1<<((nr)-1))
// 除了SIGKILL 和SIGSTOP信号以外其他信号都是可阻塞的(...1011,1111,1110,1111,111b)
#define _BLOCKABLE (~(_S(SIGKILL) | _S(SIGSTOP)))

// 内核调试函数。显示任务号nr的进程号、进程状态和内核堆栈空闲字节数(大约)
void show_task(int nr,struct task_struct * p)
{
	int i,j = 4096-sizeof(struct task_struct);

	printk("%d: pid=%d, state=%d, ",nr,p->pid,p->state);
	i=0;
	while (i<j && !((char *)(p+1))[i])      // 检测指定任务数据结构以后等于0的字节数。
		i++;
	printk("%d (of %d) chars free in kernel stack\n\r",i,j);
}

// 显示所有任务的任务号、进程号、进程状态和内核堆栈空闲字节数
// NR_TASKS是系统能容纳的最大进程(任务)数量(64个)。
void show_stat(void)
{
	int i;

	for (i=0;i<NR_TASKS;i++)
		if (task[i])
			show_task(i,task[i]);
}

// PC机8253定时芯片的输入时钟频率约为1.193180MHz. Linux内核希望定时器发出中断的频率是
// 100Hz，也即没10ms发出一次时钟中断。因此这里的LATCH是设置8253芯片的初值。
#define LATCH (1193180/HZ)

extern void mem_use(void);      // 没有任何地方定义和引用该函数

extern int timer_interrupt(void);       // 时钟中断处理程序
extern int system_call(void);           // 系统调用中断处理程序

// 每个任务(进程)在内核态运行时都有自己的内核态堆栈。这里定义了任务的内核态堆栈结构。
// 定义任务联合(任务结构成员和stack字符数组成员)。因为一个任务的数据结构与其内核态堆栈
// 在同一内存页中，所以从堆栈段寄存器ss可以获得其数据端选择符。
union task_union {
	struct task_struct task;
	char stack[PAGE_SIZE];
};

static union task_union init_task = {INIT_TASK,};   // 定义初始任务的数据

// 从开机开始算起的滴答数时间值全局变量(10ms/滴答)。系统时钟中断每发生一次即一个滴答。
// 前面的限定符volatile,英文解释是易改变的、不稳定的意思。这个限定词的含义是向编译器
// 指明变量的内容可能会由于被其他程序修改而变化。通常在程序中声明一个变量时，编译器
// 会尽量把它存放在通用寄存器中，例如ebx，以提高访问效率。当CPU把其值放到ebx中后一般
// 就不会再关心该变量对应内存位置中的内容。若此时其他程序(例如内核程序或一个中断过程)
// 修改了内存中该变量的值，ebx中的值并不会随之更新。为了解决这种情况就创建了volatile
// 限定符，让代码在引用该变量时一定要从指定内存位置中取得其值。这里即是要求gcc不要对
// jiffies进行优化处理，也不要挪动位置，并且需要从内存中取其值。因此时钟中断处理过程
// 等程序会修改它的值。
long volatile jiffies=0;
long startup_time=0;                                // 开机时间，从1970:0:0:0开始计时
struct task_struct *current = &(init_task.task);    // 当前任务指针(初始化指向任务0)
struct task_struct *last_task_used_math = NULL;     // 使用过协处理器任务的指针。

struct task_struct * task[NR_TASKS] = {&(init_task.task), }; // 定义任务指针数组

// 定义用户堆栈，共1K项，容量4K字节。在内核初始化操作过程中被用作内核栈，初始化完成
// 以后将被用作任务0的用户态堆栈。在运行任务0之前它是内核栈，以后用作任务0和1的用
// 户态栈。下面结构用于设置堆栈ss:esp(数据的选择符，指针)。ss被设置为内核数据段
// 选择符(0x10),指针esp指在user_stack数组最后一项后面。这是因为Intel CPU执行堆栈操作
// 时是先递减堆栈指针sp值，然后在sp指针处保存入栈内容。
long user_stack [ PAGE_SIZE>>2 ] ;

struct {
	long * a;
	short b;
	} stack_start = { & user_stack [PAGE_SIZE>>2] , 0x10 };
/*
 *  'math_state_restore()' saves the current math information in the
 * old math state array, and gets the new ones from the current task
 */
// 当任务被调度交换过以后，该函数用以保存原任务的协处理器状态(上下文)并恢复新调度
// 进来的当前任务的协处理器执行状态。
void math_state_restore()
{
    // 如果任务没变则返回(上一个任务就是当前任务)。这里“上一个任务”是指刚被交换出去的任务。
	if (last_task_used_math == current)
		return;
    // 在发送协处理器命令之前要先发WAIT指令。如果上个任务使用了协处理器。则保存其状态。
	__asm__("fwait");
	if (last_task_used_math) {
		__asm__("fnsave %0"::"m" (last_task_used_math->tss.i387));
	}
    // 现在，last_task_used_math指向当前任务，以备当前任务交换出去时使用。此时如果当前任务
    // 用过协处理器，则恢复其状态。否则的话说明是第一次使用，于是就向协处理器发初始化命令，
    // 并设置使用了协处理器标志。
	last_task_used_math=current;
	if (current->used_math) {
		__asm__("frstor %0"::"m" (current->tss.i387));
	} else {
		__asm__("fninit"::);        // 向协处理器发初始化命令
		current->used_math=1;       // 设置已使用协处理器标志
	}
}

/*
 *  'schedule()' is the scheduler function. This is GOOD CODE! There
 * probably won't be any reason to change this, as it should work well
 * in all circumstances (ie gives IO-bound processes good response etc).
 * The one thing you might take a look at is the signal-handler code here.
 *
 *   NOTE!!  Task 0 is the 'idle' task, which gets called when no other
 * tasks can run. It can not be killed, and it cannot sleep. The 'state'
 * information in task[0] is never used.
 */
void schedule(void)
{
	int i,next,c;
	struct task_struct ** p;

/* check alarm, wake up any interruptible tasks that have got a signal */

    // 从任务数组中最后一个任务开始循环检测alarm。在循环时跳过空指针项。
	for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
		if (*p) {
            // 如果设置过任务的定时值alarm，并且已经过期(alarm<jiffies)，则在
            // 信号位图中置SIGALRM信号，即向任务发送SIGALARM信号。然后清alarm。
            // 该信号的默认操作是终止进程。jiffies是系统从开机开始算起的滴答数(10ms/滴答)。
			if ((*p)->alarm && (*p)->alarm < jiffies) {
					(*p)->signal |= (1<<(SIGALRM-1));
					(*p)->alarm = 0;
				}
            // 如果信号位图中除被阻塞的信号外还有其他信号，并且任务处于可中断状态，则
            // 置任务为就绪状态。其中'~(_BLOCKABLE & (*p)->blocked)'用于忽略被阻塞的信号，但
            // SIGKILL 和SIGSTOP不能呗阻塞。
			if (((*p)->signal & ~(_BLOCKABLE & (*p)->blocked)) &&
			(*p)->state==TASK_INTERRUPTIBLE)
				(*p)->state=TASK_RUNNING;
		}

/* this is the scheduler proper: */

	while (1) {
		c = -1;
		next = 0;
		i = NR_TASKS;
		p = &task[NR_TASKS];
        // 这段代码也是从任务数组的最后一个任务开始循环处理，并跳过不含任务的数组槽。比较
        // 每个就绪状态任务的counter(任务运行时间的递减滴答计数)值，哪一个值大，运行时间还
        // 不长，next就值向哪个的任务号。
		while (--i) {
			if (!*--p)
				continue;
			if ((*p)->state == TASK_RUNNING && (*p)->counter > c)
				c = (*p)->counter, next = i;
		}
        // 如果比较得出有counter值不等于0的结果，或者系统中没有一个可运行的任务存在(此时c
        // 仍然为-1，next=0),则退出while(1)_的循环，执行switch任务切换操作。否则就根据每个
        // 任务的优先权值，更新每一个任务的counter值，然后回到while(1)循环。counter值的计算
        // 方式counter＝counter/2 + priority.注意：这里计算过程不考虑进程的状态。
		if (c) break;
		for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
			if (*p)
				(*p)->counter = ((*p)->counter >> 1) +
						(*p)->priority;
	}
    // 用下面的宏把当前任务指针current指向任务号Next的任务，并切换到该任务中运行。上面Next
    // 被初始化为0。此时任务0仅执行pause()系统调用，并又会调用本函数。
	switch_to(next);     // 切换到Next任务并运行。
}

// 转换当前任务状态为可中断的等待状态，并重新调度。
// 该系统调用将导致进程进入睡眠状态，知道收到一个信号。该信号用于终止进程或者使进程调用
// 一个信号捕获函数。只有当捕获了一个信号，并且信号捕获处理函数返回，pause()才会返回。此时
// pause()返回值应该是-1，并且errno被置为EINTR。这里还没有完全实现(直到0.95版)
int sys_pause(void)
{
	current->state = TASK_INTERRUPTIBLE;
	schedule();
	return 0;
}

// 把当前任务置为不可中断的等待状态，并让睡眠队列指针指向当前任务。
// 只有明确的唤醒时才会返回。该函数提供了进程与中断处理程序之间的同步机制。函数参数P是等待
// 任务队列头指针。指针是含有一个变量地址的变量。这里参数p使用了指针的指针形式'**p',这是因为
// C函数参数只能传值，没有直接的方式让被调用函数改变调用该函数程序中变量的值。但是指针'*p'
// 指向的目标(这里是任务结构)会改变，因此为了能修改调用该函数程序中原来就是指针的变量的值，
// 就需要传递指针'*p'的指针，即'**p'.
void sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

    // 若指针无效，则退出。(指针所指向的对象可以是NULL，但指针本身不应该为0).另外，如果
    // 当前任务是任务0，则死机。因为任务0的运行不依赖自己的状态，所以内核代码把任务0置为
    // 睡眠状态毫无意义。
	if (!p)
		return;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
    // 让tmp指向已经在等待队列上的任务(如果有的话)，例如inode->i_wait.并且将睡眠队列头的
    // 等等指针指向当前任务。这样就把当前任务插入到了*p的等待队列中。然后将当前任务置为
    // 不可中断的等待状态，并执行重新调度。
	tmp = *p;
	*p = current;
	current->state = TASK_UNINTERRUPTIBLE;
	schedule();
    // 只有当这个等待任务被唤醒时，调度程序才又返回到这里，表示本进程已被明确的唤醒(就
    // 续态)。既然大家都在等待同样的资源，那么在资源可用时，就有必要唤醒所有等待该该资源
    // 的进程。该函数嵌套调用，也会嵌套唤醒所有等待该资源的进程。这里嵌套调用是指一个
    // 进程调用了sleep_on()后就会在该函数中被切换掉，控制权呗转移到其他进程中。此时若有
    // 进程也需要使用同一资源，那么也会使用同一个等待队列头指针作为参数调用sleep_on()函数，
    // 并且也会陷入该函数而不会返回。只有当内核某处代码以队列头指针作为参数wake_up了队列，
    // 那么当系统切换去执行头指针所指的进程A时，该进程才会继续执行下面的代码，把队列后一个
    // 进程B置位就绪状态(唤醒)。而当轮到B进程执行时，它也才可能继续执行下面的代码。若它
    // 后面还有等待的进程C，那它也会把C唤醒等。在这前面还应该添加一行：*p = tmp.
	if (tmp)                    // 若在其前还有存在的等待的任务，则也将其置为就绪状态(唤醒).
		tmp->state=0;
}

// 将当前任务置为可中断的等待状态，并放入*p指定的等待队列中。
void interruptible_sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

    // 若指针无效，则退出。(指针所指向的对象可以是NULL，但指针本身不会为0).如果当前任务是
    // 任务0，则死机。
	if (!p)
		return;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
    // 让tmp指向已经在等待队列上的任务(如果有的话)，例如inode->i_wait。并且将睡眠队列头的
    // 等待指针指向当前任务。这样就把当前任务插入到了*p的等待队列中。然后将当前任务置为可
    // 中断的等待状态，并执行重新调度。
	tmp=*p;
	*p=current;
repeat:	current->state = TASK_INTERRUPTIBLE;
	schedule();
    // 只有当这个等待任务被唤醒时，程序才又会回到这里，标志进程已被明确的唤醒执行。如果等待
    // 队列中还有等待任务，并且队列头指针所指向的任务不是当前任务时，则将该等待任务置为可运行
    // 的就绪状态，并重新执行调度程序。当指针*p所指向的不是当前任务时，表示在当前任务被被放入
    // 队列后，又有新的任务被插入等待队列前部。因此我们先唤醒他们，而让自己仍然等等。等待这些
    // 后续进入队列的任务被唤醒执行时来唤醒本任务。于是去执行重新调度。
	if (*p && *p != current) {
		(**p).state=0;
		goto repeat;
	}
    // 下一句代码有误：应该是 *p = tmp, 让队列头指针指向其余等待任务，否则在当前任务之前插入
    // 等待队列的任务均被抹掉了。当然同时也需要删除下面行数中同样的语句
	*p=NULL;
	if (tmp)
		tmp->state=0;
}

void wake_up(struct task_struct **p)
{
	if (p && *p) {
		(**p).state=0;
		*p=NULL;
	}
}

/*
 * OK, here are some floppy things that shouldn't be in the kernel
 * proper. They are here because the floppy needs a timer, and this
 * was the easiest way of doing it.
 */
static struct task_struct * wait_motor[4] = {NULL,NULL,NULL,NULL};
static int  mon_timer[4]={0,0,0,0};
static int moff_timer[4]={0,0,0,0};
unsigned char current_DOR = 0x0C;

int ticks_to_floppy_on(unsigned int nr)
{
	extern unsigned char selected;
	unsigned char mask = 0x10 << nr;

	if (nr>3)
		panic("floppy_on: nr>3");
	moff_timer[nr]=10000;		/* 100 s = very big :-) */
	cli();				/* use floppy_off to turn it off */
	mask |= current_DOR;
	if (!selected) {
		mask &= 0xFC;
		mask |= nr;
	}
	if (mask != current_DOR) {
		outb(mask,FD_DOR);
		if ((mask ^ current_DOR) & 0xf0)
			mon_timer[nr] = HZ/2;
		else if (mon_timer[nr] < 2)
			mon_timer[nr] = 2;
		current_DOR = mask;
	}
	sti();
	return mon_timer[nr];
}

void floppy_on(unsigned int nr)
{
	cli();
	while (ticks_to_floppy_on(nr))
		sleep_on(nr+wait_motor);
	sti();
}

void floppy_off(unsigned int nr)
{
	moff_timer[nr]=3*HZ;
}

void do_floppy_timer(void)
{
	int i;
	unsigned char mask = 0x10;

	for (i=0 ; i<4 ; i++,mask <<= 1) {
		if (!(mask & current_DOR))
			continue;
		if (mon_timer[i]) {
			if (!--mon_timer[i])
				wake_up(i+wait_motor);
		} else if (!moff_timer[i]) {
			current_DOR &= ~mask;
			outb(current_DOR,FD_DOR);
		} else
			moff_timer[i]--;
	}
}

#define TIME_REQUESTS 64

static struct timer_list {
	long jiffies;
	void (*fn)();
	struct timer_list * next;
} timer_list[TIME_REQUESTS], * next_timer = NULL;

void add_timer(long jiffies, void (*fn)(void))
{
	struct timer_list * p;

	if (!fn)
		return;
	cli();
	if (jiffies <= 0)
		(fn)();
	else {
		for (p = timer_list ; p < timer_list + TIME_REQUESTS ; p++)
			if (!p->fn)
				break;
		if (p >= timer_list + TIME_REQUESTS)
			panic("No more time requests free");
		p->fn = fn;
		p->jiffies = jiffies;
		p->next = next_timer;
		next_timer = p;
		while (p->next && p->next->jiffies < p->jiffies) {
			p->jiffies -= p->next->jiffies;
			fn = p->fn;
			p->fn = p->next->fn;
			p->next->fn = fn;
			jiffies = p->jiffies;
			p->jiffies = p->next->jiffies;
			p->next->jiffies = jiffies;
			p = p->next;
		}
	}
	sti();
}

void do_timer(long cpl)
{
	extern int beepcount;
	extern void sysbeepstop(void);

	if (beepcount)
		if (!--beepcount)
			sysbeepstop();

	if (cpl)
		current->utime++;
	else
		current->stime++;

	if (next_timer) {
		next_timer->jiffies--;
		while (next_timer && next_timer->jiffies <= 0) {
			void (*fn)(void);
			
			fn = next_timer->fn;
			next_timer->fn = NULL;
			next_timer = next_timer->next;
			(fn)();
		}
	}
	if (current_DOR & 0xf0)
		do_floppy_timer();
	if ((--current->counter)>0) return;
	current->counter=0;
	if (!cpl) return;
	schedule();
}

int sys_alarm(long seconds)
{
	int old = current->alarm;

	if (old)
		old = (old - jiffies) / HZ;
	current->alarm = (seconds>0)?(jiffies+HZ*seconds):0;
	return (old);
}

int sys_getpid(void)
{
	return current->pid;
}

int sys_getppid(void)
{
	return current->father;
}

int sys_getuid(void)
{
	return current->uid;
}

int sys_geteuid(void)
{
	return current->euid;
}

int sys_getgid(void)
{
	return current->gid;
}

int sys_getegid(void)
{
	return current->egid;
}

int sys_nice(long increment)
{
	if (current->priority-increment>0)
		current->priority -= increment;
	return 0;
}

// 内核调度程序的初始化子程序
void sched_init(void)
{
	int i;
	struct desc_struct * p;                 // 描述符表结构指针

    // Linux系统开发之初，内核不成熟。内核代码会被经常修改。Linus怕自己无意中修改了
    // 这些关键性的数据结构，造成与POSIX标准的不兼容。这里加入下面这个判断语句并无
    // 必要，纯粹是为了提醒自己以及其他修改内核代码的人。
	if (sizeof(struct sigaction) != 16)         // sigaction 是存放有关信号状态的结构
		panic("Struct sigaction MUST be 16 bytes");
    // 在全局描述符表中设置初始任务(任务0)的任务状态段描述符和局部数据表描述符。
    // FIRST_TSS_ENTRY和FIRST_LDT_ENTRY的值分别是4和5，定义在include/linux/sched.h
    // 中；gdt是一个描述符表数组(include/linux/head.h)，实际上对应程序head.s中
    // 全局描述符表基址（_gdt）.因此gtd+FIRST_TSS_ENTRY即为gdt[FIRST_TSS_ENTRY](即为gdt[4]),
    // 也即gdt数组第4项的地址。
	set_tss_desc(gdt+FIRST_TSS_ENTRY,&(init_task.task.tss));
	set_ldt_desc(gdt+FIRST_LDT_ENTRY,&(init_task.task.ldt));
    // 清任务数组和描述符表项(注意 i=1 开始，所以初始任务的描述符还在)。描述符项结构
    // 定义在文件include/linux/head.h中。
	p = gdt+2+FIRST_TSS_ENTRY;
	for(i=1;i<NR_TASKS;i++) {
		task[i] = NULL;
		p->a=p->b=0;
		p++;
		p->a=p->b=0;
		p++;
	}
/* Clear NT, so that we won't have troubles with that later on */
    // NT标志用于控制程序的递归调用(Nested Task)。当NT置位时，那么当前中断任务执行
    // iret指令时就会引起任务切换。NT指出TSS中的back_link字段是否有效。
	__asm__("pushfl ; andl $0xffffbfff,(%esp) ; popfl");        // 复位NT标志
	ltr(0);
	lldt(0);
    // 下面代码用于初始化8253定时器。通道0，选择工作方式3，二进制计数方式。通道0的
    // 输出引脚接在中断控制主芯片的IRQ0上，它每10毫秒发出一个IRQ0请求。LATCH是初始
    // 定时计数值。
	outb_p(0x36,0x43);		/* binary, mode 3, LSB/MSB, ch 0 */
	outb_p(LATCH & 0xff , 0x40);	/* LSB */
	outb(LATCH >> 8 , 0x40);	/* MSB */
    // 设置时钟中断处理程序句柄(设置时钟中断门)。修改中断控制器屏蔽码，允许时钟中断。
    // 然后设置系统调用中断门。这两个设置中断描述符表IDT中描述符在宏定义在文件
    // include/asm/system.h中。
	set_intr_gate(0x20,&timer_interrupt);
	outb(inb_p(0x21)&~0x01,0x21);
	set_system_gate(0x80,&system_call);
}
