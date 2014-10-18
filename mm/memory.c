/*
 *  linux/mm/memory.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * demand-loading started 01.12.91 - seems it is high on the list of
 * things wanted, and it should be easy to implement. - Linus
 */

/*
 * Ok, demand-loading was easy, shared pages a little bit tricker. Shared
 * pages started 02.12.91, seems to work. - Linus.
 *
 * Tested sharing by executing about 30 /bin/sh: under the old kernel it
 * would have taken more than the 6M I have free, but it worked well as
 * far as I could see.
 *
 * Also corrected some "invalidate()"s - I wasn't doing enough of them.
 */

#include <signal.h>

#include <asm/system.h>

#include <linux/sched.h>
#include <linux/head.h>
#include <linux/kernel.h>

// 函数名前的关键字volatile用于告诉编译器gcc该函数不会返回。这样可以让gcc产生更
// 好一些的代码，更重要的是使用这个关键字可以避免产生某些(未初始化变量的)假警告信息。
volatile void do_exit(long code);

//// 显示内存已用完出错信息，并退出。
static inline volatile void oom(void)
{
	printk("out of memory\n\r");
    // do_exit应该使用退出代码，这里用了信号值SIGSEGV(11)相同值的出错码含义是
    // “资源暂时不可用”，正好同义。
	do_exit(SIGSEGV);
}

// 刷新页变换高速缓冲宏函数。
// 为了提高地址转换的效率，CPU将最近使用的页表数据存放在芯片中高速缓冲中。在修
// 改过页表信息之后，就需要刷新该缓冲区。这里使用重新加载页目录基地址寄存器cr3
// 的方法来进行刷新。下面eax=0,是页目录的基址。
#define invalidate() \
__asm__("movl %%eax,%%cr3"::"a" (0))

/* these are not to be changed without changing head.s etc */
// linux0.11内核默认支持的最大内存容量是16MB，可以修改这些定义适合更多的内存。
// 内存低端(1MB)
#define LOW_MEM 0x100000
// 分页内存15 MB，主内存区最多15M.
#define PAGING_MEMORY (15*1024*1024)
// 分页后的物理内存页面数（3840）
#define PAGING_PAGES (PAGING_MEMORY>>12)
// 指定地址映射为页号
#define MAP_NR(addr) (((addr)-LOW_MEM)>>12)
// 页面被占用标志.
#define USED 100

// CODE_SPACE(addr)((((addr)+0xfff)&~0xfff)<current->start_code+current->end_code).
// 该宏用于判断给定线性地址是否位于当前进程的代码段中，"(((addr)+4095)&~4095)"
// 用于取得线性地址addr所在内存页面的末端地址。
#define CODE_SPACE(addr) ((((addr)+4095)&~4095) < \
current->start_code + current->end_code)

static long HIGH_MEMORY = 0;            // 全局变量，存放实际物理内存最高端地址

// 从from处复制1页内存到to处(4K字节)。
#define copy_page(from,to) \
__asm__("cld ; rep ; movsl"::"S" (from),"D" (to),"c" (1024))

// 物理内存映射字节图（1字节代表1页内存）。每个页面对应的字节用于标志页面当前引
// 用（占用）次数。它最大可以映射15MB的内存空间。在初始化函数mem_init()中，对于
// 不能用做主内存页面的位置均都预先被设置成USED（100）.
static unsigned char mem_map [ PAGING_PAGES ] = {0,};

/*
 * Get physical address of first (actually last :-) free page, and mark it
 * used. If no free pages left, return 0.
 */
//// 在主内存区中取空闲屋里页面。如果已经没有可用物理内存页面，则返回0.
// 输入：%1(ax=0) - 0; %2(LOW_MEM)内存字节位图管理的其实位置；%3(cx=PAGING_PAGES);
// %4(edi=mem_map+PAGING_PAGES-1).
// 输出：返回%0(ax=物理内存页面起始地址)。
// 上面%4寄存器实际指向mem_map[]内存字节位图的最后一个字节。本函数从位图末端开
// 始向前扫描所有页面标志（页面总数PAGING_PAGE），若有页面空闲（内存位图字节为
// 0）则返回页面地址。注意！本函数只是指出在主内存区的一页空闲物理内存页面，但
// 并没有映射到某个进程的地址空间中去。后面的put_page()函数即用于把指定页面映射
// 到某个进程地址空间中。当然对于内核使用本函数并不需要再使用put_page()进行映射，
// 因为内核代码和数据空间（16MB）已经对等地映射到物理地址空间。
unsigned long get_free_page(void)
{
register unsigned long __res asm("ax");

__asm__("std ; repne ; scasb\n\t"   // 置方向位，al(0)与对应每个页面的(di)内容比较
	"jne 1f\n\t"                    // 如果没有等于0的字节，则跳转结束(返回0).
	"movb $1,1(%%edi)\n\t"          // 1 => [1+edi],将对应页面内存映像bit位置1.
	"sall $12,%%ecx\n\t"            // 页面数*4k = 相对页面其实地址
	"addl %2,%%ecx\n\t"             // 再加上低端内存地址，得页面实际物理起始地址
	"movl %%ecx,%%edx\n\t"          // 将页面实际其实地址->edx寄存器。
	"movl $1024,%%ecx\n\t"          // 寄存器ecx置计数值1024
	"leal 4092(%%edx),%%edi\n\t"    // 将4092+edx的位置->dei（该页面的末端地址）
	"rep ; stosl\n\t"               // 将edi所指内存清零(反方向，即将该页面清零)
	"movl %%edx,%%eax\n"            // 将页面起始地址->eax（返回值）
	"1:"
	:"=a" (__res)
	:"0" (0),"i" (LOW_MEM),"c" (PAGING_PAGES),
	"D" (mem_map+PAGING_PAGES-1)
	);
return __res;           // 返回空闲物理页面地址(若无空闲页面则返回0).
}

/*
 * Free a page of memory at physical address 'addr'. Used by
 * 'free_page_tables()'
 */
//// 释放物理地址addr开始的1页面内存。
// 物理地址1MB以下的内容空间用于内核程序和缓冲，不作为分配页面的内存空间。因此
// 参数addr需要大于1MB.
void free_page(unsigned long addr)
{
    // 首先判断参数给定的物理地址addr的合理性。如果物理地址addr小于内存低端(1MB)
    // 则表示在内核程序或高速缓冲中，对此不予处理。如果物理地址addr>=系统所含物
    // 理内存最高端，则显示出错信息并且内核停止工作。
	if (addr < LOW_MEM) return;
	if (addr >= HIGH_MEMORY)
		panic("trying to free nonexistent page");
    // 如果对参数addr验证通过，那么就根据这个物理地址换算出从内存低端开始记起的
    // 内存页面号。页面号 ＝ (addr - LOW_MEM)/4096.可见页面号从0号开始记起。此时
    // addr中存放着页面号。如果该页面号对应的页面映射字节不等于0，则减1返回。此
    // 时该映射字节值应该为0，表示页面已释放。如果对应页面字节原本就是0，表示该
    // 物理页面本来就是空闲的，说明内核代码出问题。于是显示出错信息并停机。
	addr -= LOW_MEM;
	addr >>= 12;
	if (mem_map[addr]--) return;
	mem_map[addr]=0;
	panic("trying to free free page");
}

/*
 * This function frees a continuos block of page tables, as needed
 * by 'exit()'. As does copy_page_tables(), this handles only 4Mb blocks.
 */
//// 根据指定的线性地址和限长(页表个数)，释放对应内存页表指定的内存块并置表项为
// 空闲。页目录位于物理地址0开始处，共1024项，每项4字节，共占4K字节。每个目录项
// 指定一个页表。内核页表从物理地址0x1000处开始(紧接着目录空间)，共4个页表。每
// 个页表有1024项，每项4字节。因此占4K（1页）内存。各进程（除了在内核代码中的进
// 程0和1）的页表所占据的页面在进程被创建时由内核为其主内存区申请得到。每个页表
// 项对应1耶物理内存，因此一个页表最多可映射4MB的物理内存。
// 参数：from - 起始线性基地址；size - 释放的字节长度。
int free_page_tables(unsigned long from,unsigned long size)
{
	unsigned long *pg_table;
	unsigned long * dir, nr;

    // 首先检测参数from给出的线性基地址是否在4MB的边界处。因为该函数只能处理这
    // 种情况。若from=0,则出错。说明视图释放内核和缓冲所占空间。
	if (from & 0x3fffff)
		panic("free_page_tables called with wrong alignment");
	if (!from)
		panic("Trying to free up swapper memory space");
    // 然后计算参数size给出的长度所占的页目录项数（4MB的进位整数倍），也即所占
    // 页表数。因为1个页表可管理4MB物理内存，所以这里用右移22位的方式把需要复制
    // 的内存长度值除以4MB.其中加上0x3fffff(即4MB-1)用于得到进位整数倍结果，即
    // 除操作若有余数则进1。例如，如果原size=4.01Mb，那么可得到结果sieze=2。接
    // 着结算给出的线性基地址对应的其实目录项。对应的目录项号＝from>>22.因为每
    // 项占4字节，并且由于页目录表从物理地址0开始存放，因此实际目录项指针＝目录
    // 项号<<2，也即(from>>20)。& 0xffc确保目录项指针范围有效，即用于屏蔽目录项
    // 指针最后2位。因为只移动了20位，因此最后2位是页表项索引的内容，应屏蔽掉。
	size = (size + 0x3fffff) >> 22;
	dir = (unsigned long *) ((from>>20) & 0xffc); /* _pg_dir = 0 */
    // 此时size是释放的页表个数，即页目录项数，而dir是起始目录项指针。现在开始
    // 循环操作页目录项，依次释放每个页表中的页表项。如果当前目录项无效（P位＝0）
    // 表示该目录项没有使用(对应的页表不存在)，则继续处理下一个目录项。否则从目
    // 录项总取出页表地址pg_table，并对该页表中的1024个表项进行处理。释放有效页
    // 表项(P位＝1)对应的物理内存页表。然后该页表项清零，并继续处理下一页表项。
    // 当一个页表所有表项都处理完毕就释放该页表自身占据的内存页面，并继续处理下
    // 一页目录项。最后刷新也页变换高速缓冲，并返回0.
	for ( ; size-->0 ; dir++) {
		if (!(1 & *dir))
			continue;
		pg_table = (unsigned long *) (0xfffff000 & *dir);  // 取页表地址
		for (nr=0 ; nr<1024 ; nr++) {
			if (1 & *pg_table)                          // 若该项有效，则释放对应页。 
				free_page(0xfffff000 & *pg_table);
			*pg_table = 0;                              // 该页表项内容清零。
			pg_table++;                                 // 指向页表中下一项。
		}
		free_page(0xfffff000 & *dir);                   // 释放该页表所占内存页面。
		*dir = 0;                                       // 对应页表的目录项清零
	}
	invalidate();                                       // 刷新页变换高速缓冲。
	return 0;
}

/*
 *  Well, here is one of the most complicated functions in mm. It
 * copies a range of linerar addresses by copying only the pages.
 * Let's hope this is bug-free, 'cause this one I don't want to debug :-)
 *
 * Note! We don't copy just any chunks of memory - addresses have to
 * be divisible by 4Mb (one page-directory entry), as this makes the
 * function easier. It's used only by fork anyway.
 *
 * NOTE 2!! When from==0 we are copying kernel space for the first
 * fork(). Then we DONT want to copy a full page-directory entry, as
 * that would lead to some serious memory waste - we just copy the
 * first 160 pages - 640kB. Even that is more than we need, but it
 * doesn't take any more memory - we don't copy-on-write in the low
 * 1 Mb-range, so the pages can be shared with the kernel. Thus the
 * special case for nr=xxxx.
 */
//// 复制页目录表项和页表项
// 复制指定线性地址和长度内存对应的页目录项和页表项，从而被复制的页目录和页表对
// 应的原物理内存页面区被两套页表映射而共享使用。复制时，需申请新页面来存放新页
// 表，原物理内存区将被共享。此后两个进程（父进程和其子进程）将共享内存区，直到
// 有一个进程执行谢操作时，内核才会为写操作进程分配新的内存页(写时复制机制)。
// 参数from、to是线性地址，size是需要复制（共享）的内存长度，单位是byte.
int copy_page_tables(unsigned long from,unsigned long to,long size)
{
	unsigned long * from_page_table;
	unsigned long * to_page_table;
	unsigned long this_page;
	unsigned long * from_dir, * to_dir;
	unsigned long nr;

    // 首先检测参数给出的原地址from和目的地址to的有效性。原地址和目的地址都需要
    // 在4Mb内存边界地址上。否则出错死机。作这样的要求是因为一个页表的1024项可
    // 管理4Mb内存。源地址from和目的地址to只有满足这个要求才能保证从一个页表的
    // 第一项开始复制页表项，并且新页表的最初所有项都是有效的。然后取得源地址和
    // 目的地址的其实目录项指针(from_dir 和 to_dir).再根据参数给出的长度size计
    // 算要复制的内存块占用的页表数(即目录项数)。
	if ((from&0x3fffff) || (to&0x3fffff))
		panic("copy_page_tables called with wrong alignment");
	from_dir = (unsigned long *) ((from>>20) & 0xffc); /* _pg_dir = 0 */
	to_dir = (unsigned long *) ((to>>20) & 0xffc);
	size = ((unsigned) (size+0x3fffff)) >> 22;
    // 在得到了源起始目录项指针from_dir和目的起始目录项指针to_dir以及需要复制的
    // 页表个数size后，下面开始对每个页目录项依次申请1页内存来保存对应的页表，并
    // 且开始页表项复制操作。如果目的目录指定的页表已经存在(P=1)，则出错死机。
    // 如果源目录项无效，即指定的页表不存在(P=1),则继续循环处理下一个页目录项。
	for( ; size-->0 ; from_dir++,to_dir++) {
		if (1 & *to_dir)
			panic("copy_page_tables: already exist");
		if (!(1 & *from_dir))
			continue;
        // 在验证了当前源目录项和目的项正常之后，我们取源目录项中页表地址
        // from_page_table。为了保存目的目录项对应的页表，需要在住内存区中申请1
        // 页空闲内存页。如果取空闲页面函数get_free_page()返回0，则说明没有申请
        // 到空闲内存页面，可能是内存不够。于是返回-1值退出。
		from_page_table = (unsigned long *) (0xfffff000 & *from_dir);
		if (!(to_page_table = (unsigned long *) get_free_page()))
			return -1;	/* Out of memory, see freeing */
        // 否则我们设置目的目录项信息，把最后3位置位，即当前目录的目录项 | 7，
        // 表示对应页表映射的内存页面是用户级的，并且可读写、存在(Usr,R/W,Present).
        // (如果U/S位是0，则R/W就没有作用。如果U/S位是1，而R/W是0，那么运行在用
        // 户层的代码就只能读页面。如果U/S和R/W都置位，则就有读写的权限)。然后
        // 针对当前处理的页目录项对应的页表，设置需要复制的页面项数。如果是在内
        // 核空间，则仅需复制头160页对应的页表项(nr=160),对应于开始640KB物理内存
        // 否则需要复制一个页表中的所有1024个页表项(nr=1024)，可映射4MB物理内存。
		*to_dir = ((unsigned long) to_page_table) | 7;
		nr = (from==0)?0xA0:1024;
        // 此时对于当前页表，开始循环复制指定的nr个内存页面表项。先取出源页表的
        // 内容，如果当前源页表没有使用，则不用复制该表项，继续处理下一项。否则
        // 复位表项中R/W标志(位1置0)，即让页表对应的内存页面只读。然后将页表项复制
        // 到目录页表中。
		for ( ; nr-- > 0 ; from_page_table++,to_page_table++) {
			this_page = *from_page_table;
			if (!(1 & this_page))
				continue;
			this_page &= ~2;
			*to_page_table = this_page;
            // 如果该页表所指物理页面的地址在1MB以上，则需要设置内存页面映射数
            // 组mem_map[]，于是计算页面号，并以它为索引在页面映射数组相应项中
            // 增加引用次数。而对于位于1MB以下的页面，说明是内核页面，因此不需
            // 要对mem_map[]进行设置。因为mem_map[]仅用于管理主内存区中的页面使
            // 用情况。因此对于内核移动到任务0中并且调用fork()创建任务1时(用于
            // 运行init())，由于此时复制的页面还仍然都在内核代码区域，因此以下
            // 判断中的语句不会执行，任务0的页面仍然可以随时读写。只有当调用fork()
            // 的父进程代码处于主内存区(页面位置大于1MB)时才会执行。这种情况需要
            // 在进程调用execve()，并装载执行了新程序代码时才会出现。
            // *from_page_table = this_page; 这句是令源页表项所指内存页也为只读。
            // 因为现在开始有两个进程公用内存区了。若其中1个进程需要进行写操作，
            // 则可以通过页异常写保护处理为执行写操作的进程匹配1页新空闲页面，也
            // 即进行写时复制(copy on write)操作。
			if (this_page > LOW_MEM) {
				*from_page_table = this_page;
				this_page -= LOW_MEM;
				this_page >>= 12;
				mem_map[this_page]++;
			}
		}
	}
	invalidate();
	return 0;
}

/*
 * This function puts a page in memory at the wanted address.
 * It returns the physical address of the page gotten, 0 if
 * out of memory (either when trying to access page-table or
 * page.)
 */
//// 把一物理内存页面映射到线性地址空间指定处。
// 或者说是把线性地址空间中指定地址address出的页面映射到主内存区页面page上。主
// 要工作是在相关页面目录项和页表项中设置指定页面的信息。若成功则返回物理页面地
// 址。在处理缺页异常的C函数do_no_page()中会调用此函数。对于缺页引起的异常，由于
// 任何缺页缘故而对页表作修改时，并不需要刷新CPU的页变换缓冲(或称Translation Lookaside
// Buffer - TLB),即使页表中标志P被从0修改成1.因为无效叶项不会被缓冲，因此当修改
// 了一个无效的页表项时不需要刷新。在次就表现为不用调用Invalidate()函数。
// 参数page是分配的主内存区中某一页面(页帧，页框)的指针;address是线性地址。
unsigned long put_page(unsigned long page,unsigned long address)
{
	unsigned long tmp, *page_table;

/* NOTE !!! This uses the fact that _pg_dir=0 */

    // 首先判断参数给定物理内存页面page的有效性。如果该页面位置低于LOW_MEM（1MB）
    // 或超出系统实际含有内存高端HIGH_MEMORY，则发出警告。LOW_MEM是主内存区可能
    // 有的最小起始位置。当系统物理内存小于或等于6MB时，主内存区起始于LOW_MEM处。
    // 再查看一下该page页面是否已经申请的页面，即判断其在内存页面映射字节图mem_map[]
    // 中相应字节是否已经置位。若没有则需发出警告。
	if (page < LOW_MEM || page >= HIGH_MEMORY)
		printk("Trying to put page %p at %p\n",page,address);
	if (mem_map[(page-LOW_MEM)>>12] != 1)
		printk("mem_map disagrees with %p at %p\n",page,address);
    // 然后根据参数指定的线性地址address计算其在也目录表中对应的目录项指针，并
    // 从中取得二级页表地址。如果该目录项有效(P=1),即指定的页表在内存中，则从中
    // 取得指定页表地址放到page_table 变量中。否则就申请一空闲页面给页表使用，并
    // 在对应目录项中置相应标志(7 - User、U/S、R/W).然后将该页表地址放到page_table
    // 变量中。
	page_table = (unsigned long *) ((address>>20) & 0xffc);
	if ((*page_table)&1)
		page_table = (unsigned long *) (0xfffff000 & *page_table);
	else {
		if (!(tmp=get_free_page()))
			return 0;
		*page_table = tmp|7;
		page_table = (unsigned long *) tmp;
	}
    // 最后在找到的页表page_table中设置相关页表内容，即把物理页面page的地址填入
    // 表项同时置位3个标志(U/S、W/R、P)。该页表项在页表中索引值等于线性地址位21
    // -- 位12组成的10bit的值。每个页表共可有1024项(0 -- 0x3ff)。
	page_table[(address>>12) & 0x3ff] = page | 7;
/* no need for invalidate */
	return page;
}

//// 取消写保护页面函数。用于页异常中断过程中写保护异常的处理(写时复制)。
// 在内核创建进程时，新进程与父进程被设置成共享代码和数据内存页面，并且所有这些
// 页面均被设置成只读页面。而当新进程或原进程需要向内存页面写数据时，CPU就会检测
// 到这个情况并产生页面写保护异常。于是在这个函数中内核就会首先判断要写的页面是
// 否被共享。若没有则把页面设置成可写然后退出。若页面是出于共享状态，则需要重新
// 申请一新页面并复制被写页面内容，以供写进程单独使用。共享被取消。本函数供下面
// do_wp_page()调用。
// 输入参数为页表项指针，是物理地址。[up_wp_page -- Un-Write Protect Page]
void un_wp_page(unsigned long * table_entry)
{
	unsigned long old_page,new_page;

    // 首先取参数指定的页表项中物理页面位置(地址)并判断该页面是否是共享页面。如
    // 果原页面地址大于内存低端LOW_MEM（表示在主内存区中），并且其在页面映射字节
    // 图数组中值为1（表示页面仅被引用1次，页面没有被共享），则在该页面的页表项
    // 中置R/W标志(可写),并刷新页变换高速缓冲，然后返回。即如果该内存页面此时只
    // 被一个进程使用，并且不是内核中的进程，就直接把属性改为可写即可，不用再重
    // 新申请一个新页面。
	old_page = 0xfffff000 & *table_entry;
	if (old_page >= LOW_MEM && mem_map[MAP_NR(old_page)]==1) {
		*table_entry |= 2;
		invalidate();
		return;
	}
    // 否则就需要在主内存区申请一页空闲页面给执行写操作的进程单独使用，取消页面
    // 共享。如果原页面大于内存低端(则意味着mem_map[]>1,页面是共享的)，则将原页
    // 面的页面映射字节数组递减1。然后将指定页表项内容更新为新页面地址，并置可读
    // 写等标志（U/S、R/W、P）。在刷新页变换高速缓冲之后，最后将原页面内容复制
    // 到新页面上。
	if (!(new_page=get_free_page()))
		oom();
	if (old_page >= LOW_MEM)
		mem_map[MAP_NR(old_page)]--;
	*table_entry = new_page | 7;
	invalidate();
	copy_page(old_page,new_page);
}	

/*
 * This routine handles present pages, when users try to write
 * to a shared page. It is done by copying the page to a new address
 * and decrementing the shared-page counter for the old page.
 *
 * If it's in code space we exit with a segment error.
 */
//// 执行写保护页处理。
// 是写共享页面处理函数。是页异常中断处理过程中调用的C函数。在page.s程序中被调用。
// 参数error_code是进程在写写保护页面时由CPU自动产生，address是页面线性地址。
// 写共享页面时，需复制页面（写时复制）.
void do_wp_page(unsigned long error_code,unsigned long address)
{
#if 0
/* we cannot do this yet: the estdio library writes to code space */
/* stupid, stupid. I really want the libc.a from GNU */
	if (CODE_SPACE(address))
		do_exit(SIGSEGV);
#endif
    // 调用上面函数un_wp_page()来处理取消页面保护。但首先需要为其准备好参数。参
    // 数是线性地址address指定页面在页表中的页表项指针，其计算方法是：
    // 1.((address>>10) & 0xffc): 计算指定线性地址中页表项在页表中的偏移地址；因
    // 为根据线性地址结构，(address>>12)就是页表项中的索引，但每项占4个字节，因
    // 此乘4后：(address>>12)<<2=(address>>10)&0xffc就可得到页表项在表中的偏移
    // 地址。与操作&0xffc用于限制地址范围在一个页面内。又因为只移动了10位，因此
    // 最后2位是线性地址低12位中的最高2位，也应屏蔽掉。因此求线性地址中页表项在
    // 页表中偏移地址直观一些的表示方法是(((address>>12)&ox3ff)<<2).
    // 2.(0xfffff000 & *((address>>20) &0xffc)):用于取目录项中页表的地址值；其中，
    // ((address>>20) &0xffc)用于取线性地址中的目录索引项在目录表中的便宜地址。
    // 因为address>>22是目录项索引值，但每项4个字节，因此乘以4后：(address>>22)<<2
    // = (address>>20)就是指定在目录表中的偏移地址。&0xffc用于屏蔽目录项索引值中
    // 最后2位。因为只移动了20位，因此最后2位是页表索引的内容，应该屏蔽掉。而
    // *((address>>20) &0xffc)则是取指定目录表项内容中对应页表的物理地址。最后与
    // 上0xfffff000用于屏蔽掉页目录项内容中的一些标志位(目录项低12位)。直观表示为
    // (0xfffff000 & *(unsigned log *) (((address>>22) & 0x3ff)<<2)).
    // 3.由1中页表项中偏移地址加上2中目录表项内容中对应页表的物理地址即可得到页
    // 表项的指针(物理地址)。这里对共享的页面进行复制。
	un_wp_page((unsigned long *)
		(((address>>10) & 0xffc) + (0xfffff000 &
		*((unsigned long *) ((address>>20) &0xffc)))));

}

//// 写页面验证
// 若页面不可写，则复制页面。
// 参数address是指定页面的线性地址。
void write_verify(unsigned long address)
{
	unsigned long page;

    // 首先取指定线性地址对应的页目录项，根据目录项中的存在位P判断目录项对应的
    // 页表是否存在(存在位P=12),若不存在(P=0)则返回。这样处理是因为对于不存在的
    // 页面没有共享和写时复制可言，并且若程序对此不存在的页面执行写操作时，系统
    // 就会因为缺页异常而去执行do_no_page()，并为这个地方使用put_page()函数映射
    // 一个物理页面。
    // 接着程序从目录项中取页表地址，加上指定页面在页表中的页表项偏移值，得对应
    // 地址的页表项指针。在该表项中包含这给定线性地址对应的物理页面。
	if (!( (page = *((unsigned long *) ((address>>20) & 0xffc)) )&1))
		return;
	page &= 0xfffff000;
	page += ((address>>10) & 0xffc);
    // 然后判断该页表项中的位1(R/W)、位0(P)标志。如果该页面不可写(R/W=0)且存在，
    // 那么就执行共享检验和复制页面操作(写时复制)。否则什么也不做，直接退出。
	if ((3 & *(unsigned long *) page) == 1)  /* non-writeable, present */
		un_wp_page((unsigned long *) page);
	return;
}

//// 取得一页空闲内存页并映射到指定线性地址处。
// get_free_page()仅是申请取得了主内存区的一页物理内存。而本函数则不仅是获取到
// 一页物理内存页面，还进一步调用put_page()，将物理页面映射到指定的线性地址处。
// 参数address是指定页面的线性地址。
void get_empty_page(unsigned long address)
{
	unsigned long tmp;

    // 如果不能取得有一空闲页面，或者不能将所取页面放置到指定地址处，则显示内存不够信息。
	if (!(tmp=get_free_page()) || !put_page(tmp,address)) {
		free_page(tmp);		/* 0 is ok - ignored */
		oom();
	}
}

/*
 * try_to_share() checks the page at address "address" in the task "p",
 * to see if it exists, and if it is clean. If so, share it with the current
 * task.
 *
 * NOTE! This assumes we have checked that p != current, and that they
 * share the same executable.
 */
//// 尝试对当前进程指定地址处的页面进行共享处理。
// 当前进程与进程p是同一执行代码，也可以认为当前进程是由p进程执行fork操作产生的
// 进程，因此它们的代码内容一样。如果未对数据段内容做过修改那么数据段内容也应一
// 样。参数address是进程中的逻辑地址，即是当前进程欲与p进程共享页面的逻辑页面地
// 址。进程P是将被共享页面的进程。如果P进程address出的页面存在并且没有被修改过的
// 话，就让当前进程与p进程共享之。同时还需要验证指定地址处是否已经申请了页面，若
// 是则出错，死机。返回：1 - 页面共享处理成功；0 - 失败。
static int try_to_share(unsigned long address, struct task_struct * p)
{
	unsigned long from;
	unsigned long to;
	unsigned long from_page;
	unsigned long to_page;
	unsigned long phys_addr;

    // 首先分别求的指定进程p中和当前进程中逻辑地址address对应的页目录项。为了计
    // 算方便先求出指定逻辑地址address出的'逻辑'页目录项号，即以进程空间(0 - 64 MB)
    // 算出的页目录项号。该'逻辑'页目录项号加上进程p在CPU 4G线性空间中的实际页目
    // 录项from_page。而'逻辑'页目录项号加上当前进程CPU 4G线性空间中起始地址对应
    // 的页目录项，即可最后得到当前进程中地址address处页面所对应的4G线性空间中的
    // 实际页目录项to_page。
	from_page = to_page = ((address>>20) & 0xffc);
	from_page += ((p->start_code>>20) & 0xffc);
	to_page += ((current->start_code>>20) & 0xffc);
    // 在得到p进程和当前进程address对应的目录项后，下面分别对进程p和当前进程进行
    // 处理。下面首先对p进程的表项进行操作。目标是取得p进程中address对应的物理内
    // 存页面地址，并且该物理页面存在，而且干净(没有被修改过)。
    // 方法是先取目录项内容。如果该目录项无效(P=0),表示目录项对应的二级页表不存
    // 在，于是返回。否则取该目录项对应页表地址from，从而计算出逻辑地址address
    // 对应的页表项指针，并取出该页表项内容临时保存在phys_addr中。
/* is there a page-directory at from? */
	from = *(unsigned long *) from_page;
	if (!(from & 1))
		return 0;
	from &= 0xfffff000;
	from_page = from + ((address>>10) & 0xffc);
	phys_addr = *(unsigned long *) from_page;
/* is the page clean and present? */
    // 接着看看页表项映射的物理页面是否存在并且干净。0x41对应页表项中的D(dirty)
    // 和P（present）标志。如果页面不干净或无效则返回。然后我们从该表项中取出物
    // 理页面地址再保存在phys_addr中。最后我们再检查一下这个物理页面地址的有效性，
    // 即它不应该超过机器最大物理地址值，也不应该小于内存低端(1 MB).
	if ((phys_addr & 0x41) != 0x01)
		return 0;
	phys_addr &= 0xfffff000;
	if (phys_addr >= HIGH_MEMORY || phys_addr < LOW_MEM)
		return 0;
    // 下面首先对当前进程的表项进行操作。目标是取得当前进程中address对应的页表
    // 项地址，并且该页表项还没有映射物理页面，即其P=0。
    // 首先取当前进程页目录项内容->to.如果该目录项无效(P=0),即目录项对应的二级
    // 页表不存在，则申请一空闲页面来存放页表，并更新目录项to_page内容，让其指向
    // 内存页面。
	to = *(unsigned long *) to_page;
	if (!(to & 1)) {
		if ((to = get_free_page()))
			*(unsigned long *) to_page = to | 7;
		else
			oom();
	}
    // 否则取目录项中的页表地址->to，加上页表项索引值<<2，即页表项在表中偏移地址，
    // 得到页表地址->to_page.针对页表项，如果我们此时我们检查出其对应的物理页面
    // 已经存在，即页表的存在位P=1，则说明原本我们想共享进程p中对应的物理页面，
    // 但现在我们自己已经占有了(映射有)物理页面。于是说明内核出错，死机。
	to &= 0xfffff000;
	to_page = to + ((address>>10) & 0xffc);
	if (1 & *(unsigned long *) to_page)
		panic("try_to_share: to_page already exists");
    // 在找到了进程p中逻辑地址address处对应的干净且存在的物理页面，而且也确定了
    // 当前进程中逻辑地址address所对应的耳机页表项地址之后，我们现在对他们进行
    // 共享处理。方法很简单，就是首先对p进程的页表项进行修改，设置其写保护(R/W=0,
    // 只读)标志，然后让当前进程复制p进程的这个页表项。此时当前进程逻辑地址address
    // 处页面即被映射到p进程逻辑地址address处页面映射的物理页面上。
/* share them: write-protect */
	*(unsigned long *) from_page &= ~2;
	*(unsigned long *) to_page = *(unsigned long *) from_page;
    // 随后刷新页变换高速缓冲。计算所操作屋里页面的页面号，并将对应页面映射字节数
    // 组项中的引用递增1。最后返回1，表示共享处理成功。
	invalidate();
	phys_addr -= LOW_MEM;
	phys_addr >>= 12;
	mem_map[phys_addr]++;
	return 1;
}

/*
 * share_page() tries to find a process that could share a page with
 * the current one. Address is the address of the wanted page relative
 * to the current data space.
 *
 * We first check if it is at all feasible by checking executable->i_count.
 * It should be >1 if there are other tasks sharing this inode.
 */
//// 共享页面处理。
// 在发生缺页异常时，首先看看能否与运行同一个执行文件的其他进程进行页面共享处理
// 该函数首先判断系统是否有另一个进程也在运行当前进程一样的执行文件。若有，则在
// 系统当前所有任务中寻找这样的任务。若找到了这样的任务就尝试与其共享指定地址处
// 的页面。若系统中没有其他任务正在运行与当前进程相同的执行文件，那么共享页面操
// 作的前提条件不存在，因此函数立刻退出。判断系统中是否有另一个进程也在执行同一
// 个执行文件的方法是零用进程任务数据结构中的executable字段。该字段指向进程正在
// 执行程序在内存中的i节点。根据该i节点的引用次数i_count我们可以进行这种判断。若
// executable->i_count值大于1，则表明系统中可能有两个进程运行同一个执行文件，于
// 是可以再对任务结构数组中所有任务比较是否有相同的executable字段来最后确定多个
// 进程运行着相同执行文件的情况。
// 参数address是进程中的逻辑地址，即是当前进程欲与p进程共享页面的逻辑页面地址。
// 返回：1 - 共享操作成功，0 - 失败。
static int share_page(unsigned long address)
{
	struct task_struct ** p;

    // 首先检查一下当前进程的executable字段是否指向某执行文件的i节点，以判断本
    // 进程是否有对应的执行文件。如果没有，则返回0.如果executable的确指向某个i
    // 节点，则检查该i节点引用计数值。如果当前进程运行的执行文件的内存i节点引用
    // 计数等于1(executable->i_count=1),表示当前系统中只有1个进程(即当前进程)在
    // 运行该执行文件。因此无共享可言，直接退出函数。
	if (!current->executable)
		return 0;
	if (current->executable->i_count < 2)
		return 0;
    // 否则搜索任务数组中所有任务。寻找与当前进程可共享页面的进程，即运行相同的
    // 执行文件的另一个进程，并尝试对指定地址的页面进行共享。如果找到某个进程p，
    // 其executable字段值与当前进程的相同，则调用try_to_share()尝试页面共享。若
    // 共享操作成功，则函数返回1。否则返回0，表示共享页面操作失败.
	for (p = &LAST_TASK ; p > &FIRST_TASK ; --p) {
		if (!*p)
			continue;
		if (current == *p)
			continue;
        // 如果executable不等，表示运行的不是与当前进程相同的执行文件，因此也继续
        // 寻找。
		if ((*p)->executable != current->executable)
			continue;
		if (try_to_share(address,*p))
			return 1;
	}
	return 0;
}

//// 执行缺页处理
// 是访问不存在页面处理函数。页异常中断处理过程中调用的函数。在page.s程序中被调
// 用。函数参数error_code和address是进程在访问页面时由CPU因缺页产生异常而自动生
// 成。该函数首先尝试与已加载的相同文件进行页面共享，或者只是由于进程动态申请内
// 存页面而只需映射一页物理内存即可。若共享操作不成功，那么只能从相应文件中读入
// 所缺的数据页面到指定线性地址处。
void do_no_page(unsigned long error_code,unsigned long address)
{
	int nr[4];
	unsigned long tmp;
	unsigned long page;
	int block,i;

    // 首先取线性空间中指定地址address处页面地址。从而可算出指定线性地址在进程
    // 空间相对于进程基地址的偏移长度值tmp，即对应的逻辑地址。
	address &= 0xfffff000;
	tmp = address - current->start_code;
    // 若当进程的executable节点指针空，或者指定地址超出(代码+数据)长度，则申请
    // 一页物理内存，并映射到指定的线性地址处。executable是进程正在运行的执行文
    // 件的i节点结构。由于任务0和任务1的代码在内核中，因此任务0，任务1以及任务1
    // 派生的没有调用过execute()的所有任务的executable都为0.若该值为0，或者参数
    // 指定的线性地址超出代码加数据长度，则表明进程在申请新的内存页面存放堆或栈
    // 中数据。因此直接调用取空闲页面函数get_empty_page()为进程申请一页物理内存
    // 并映射到指定线性地址处。进程任务结构字段start_code是线性地址空间中进程代
    // 码段地址，字段end_data是代码加数据长度。对于Linux0.11内核，它的代码段和
    // 数据段其实基址相同。
	if (!current->executable || tmp >= current->end_data) {
		get_empty_page(address);
		return;
	}
	if (share_page(tmp))
		return;
	if (!(page = get_free_page()))
		oom();
/* remember that 1 block is used for header */
    // 因为块设备上存放的执行文件映象第1块数据是程序头结构，因此在读取该文件时
    // 需要跳过第1块数据。所以需要首先计算缺页所在数据块号。因为每块数据长度为
    // BLOCK_SIZE=1KB，因此一页内存课存放4个数据块。进程逻辑地址tmp除以数据块大
    // 小再加上1即可得出缺少的页面在执行映象文件中的起始块号block。根据这个块号
    // 和执行文件的i节点，我们就可以从映射位图中找到对应块设备中对应的设备逻辑块
    // 号(保存在nr[]数组中)。利用bread_page()即可把这4个逻辑块读入到物理页面page中。
	block = 1 + tmp/BLOCK_SIZE;
	for (i=0 ; i<4 ; block++,i++)
		nr[i] = bmap(current->executable,block);
	bread_page(page,current->executable->i_dev,nr);
    // 在读设备逻辑块操作时，可能会出现这样一种情况，即在执行文件中的读取页面位
    // 置可能离文件尾不到1个页面的长度。因此就可能读入一些无用的信息，下面的操作
    // 就是把这部分超出执行文件end_data以后的部分清零处理。
	i = tmp + 4096 - current->end_data;
	tmp = page + 4096;
	while (i-- > 0) {
		tmp--;
		*(char *)tmp = 0;
	}
    // 最后把引起缺页异常的一页物理页面映射到指定线性地址address处。若操作成功
    // 就返回。否则就释放内存页，显示内存不够。
	if (put_page(page,address))
		return;
	free_page(page);
	oom();
}

// 物理内存管理初始化
// 该函数对1MB以上的内存区域以页面为单位进行管理前的初始化设置工作。一个页面长度
// 为4KB bytes.该函数把1MB以上所有物理内存划分成一个个页面，并使用一个页面映射字节
// 数组mem_map[]来管理所有这些页面。对于具有16MB内存容量的机器，该数组共有3840
// 项((16MB-1MB)/4KB)，即可管理3840个物理页面。每当一个物理内存页面被占用时就把
// mem_map[]中对应的字节值增1；若释放一个物理页面，就把对应字节值减1。若字节值为0，
// 则表示对应页面空闲；若字节值大于或等于1，则表示对应页面被占用或被不同程序共享占用。
// 在该版本的Linux内核中，最多能管理16MB的物理内存，大于16MB的内存将弃之不用。
// 对于具有16MB内存的PC机系统，在没有设置虚拟盘RAMDISK的情况下start_mem通常是4MB，
// end_mem是16MB。因此此时主内存区范围是4MB-16MB,共有3072个物理页面可供分配。而
// 范围0-1MB内存空间用于内核系统（其实内核只使用0-640Kb，剩下的部分被部分高速缓冲和
// 设备内存占用）。
// 参数start_mem是可用做页面分配的主内存区起始地址（已去除RANDISK所占内存空间）。
// end_mem是实际物理内存最大地址。而地址范围start_mem到end_mem是主内存区。
void mem_init(long start_mem, long end_mem)
{
	int i;

    // 首先将1MB到16MB范围内所有内存页面对应的内存映射字节数组项置为已占用状态，
    // 即各项字节全部设置成USED(100)。PAGING_PAGES被定义为(PAGING_MEMORY>>12)，
    // 即1MB以上所有物理内存分页后的内存页面数(15MB/4KB = 3840).
	HIGH_MEMORY = end_mem;                  // 设置内存最高端(16MB)
	for (i=0 ; i<PAGING_PAGES ; i++)
		mem_map[i] = USED;
    // 然后计算主内存区起始内存start_mem处页面对应内存映射字节数组中项号i和主内存区页面数。
    // 此时mem_map[]数组的第i项正对应主内存区中第1个页面。最后将主内存区中页面对应的数组项
    // 清零(表示空闲)。对于具有16MB物理内存的系统，mem_map[]中对应4MB-16MB主内存区的项被清零。
	i = MAP_NR(start_mem);      // 主内存区其实位置处页面号
	end_mem -= start_mem;
	end_mem >>= 12;             // 主内存区中的总页面数
	while (end_mem-->0)
		mem_map[i++]=0;         // 主内存区页面对应字节值清零
}

//// 计算内存空闲页面数并显示
// [?? 内核中没有其他地方调用该函数，Linus调试过程中用的]
void calc_mem(void)
{
	int i,j,k,free=0;
	long * pg_tbl;

    // 扫描内存页面映射数组mem_map[]，获取空闲页面数并显示。然后扫描所有的页目
    // 录项(除0，1项)，如果页目录项有效，则统计对应页表中有效页面数，并显示。页
    // 目录项0-3被内核使用，因此应该从第5个目录项(i＝4)开始扫描。
	for(i=0 ; i<PAGING_PAGES ; i++)
		if (!mem_map[i]) free++;
	printk("%d pages free (of %d)\n\r",free,PAGING_PAGES);
	for(i=2 ; i<1024 ; i++) {               // 初始值应该等于4
		if (1&pg_dir[i]) {
			pg_tbl=(long *) (0xfffff000 & pg_dir[i]);
			for(j=k=0 ; j<1024 ; j++)
				if (pg_tbl[j]&1)
					k++;
			printk("Pg-dir[%d] uses %d pages\n",i,k);
		}
	}
}
