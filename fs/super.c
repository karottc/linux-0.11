/*
 *  linux/fs/super.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * super.c contains code to handle the super-block tables.
 */
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>

#include <errno.h>
#include <sys/stat.h>

// 对指定设备执行高速缓冲与设备上数据的同步操作
int sync_dev(int dev);
// 等待击键
void wait_for_keypress(void);

/* set_bit uses setb, as gas doesn't recognize setc */
//// 测试指定位偏移处bit位的值，并返回该原bit位值。
// 嵌入式汇编宏，参数bitnr是bit位偏移值，addr是测试bit位操作的起始地址。
// %0 - ax(__res), %1 - 0, %2 - bitnr, %3 - addr, 下面一行定义了一个寄存器变量。
// 该变量将被保存在eax寄存器中，以便于高效访问和操作，指令bt用于对bit位进行测试，它
// 会把地址addr（%3）和bit位偏移量bitnr（%2）指定的bit位的值放入进位标志CF中，指令setb
// 用于根据进位标志CF设置操作数%al.如果CF=1，则%al=1,否则%al=0.
#define set_bit(bitnr,addr) ({ \
register int __res ; \
__asm__("bt %2,%3;setb %%al":"=a" (__res):"a" (0),"r" (bitnr),"m" (*(addr))); \
__res; })

// 超级块结构表数组（NR_SUPER = 8）
struct super_block super_block[NR_SUPER];
/* this is initialized in init/main.c */
int ROOT_DEV = 0;       // 根文件系统设备号。

// 以下3个函数（lock_super()、free_super()和wait_on_super()）的作用与inode.c文件中头
// 3个函数的作用雷同，只是这里操作的对象换成了超级块。
//// 锁定超级块
// 如果超级块已被锁定，则将当前任务置为不可中断的等待状态，并添加到该超级块等待队列
// s_wait中。直到该超级块解锁并明确地唤醒本地任务。然后对其上锁。
static void lock_super(struct super_block * sb)
{
	cli();                          // 关中断
	while (sb->s_lock)              // 如果该超级块已经上锁，则睡眠等待。
		sleep_on(&(sb->s_wait));
	sb->s_lock = 1;                 // 会给超级块加锁（置锁定标志）
	sti();                          // 开中断
}

//// 对指定超级块解锁
// 复位超级块的锁定标志，并明确地唤醒等待在此超级块等待队列s_wait上的所有进程。
// 如果使用ulock_super这个名称则可能更妥贴。
static void free_super(struct super_block * sb)
{
	cli();
	sb->s_lock = 0;             // 复位锁定标志
	wake_up(&(sb->s_wait));     // 唤醒等待该超级块的进程。
	sti();
}

//// 睡眠等待超级解锁
// 如果超级块已被锁定，则将当前任务置为不可中断的等待状态，并添加到该超级块的等待
// 队列s_wait中。知道该超级块解锁并明确的唤醒本地任务.
static void wait_on_super(struct super_block * sb)
{
	cli();
	while (sb->s_lock)
		sleep_on(&(sb->s_wait));
	sti();
}

struct super_block * get_super(int dev)
{
	struct super_block * s;

	if (!dev)
		return NULL;
	s = 0+super_block;
	while (s < NR_SUPER+super_block)
		if (s->s_dev == dev) {
			wait_on_super(s);
			if (s->s_dev == dev)
				return s;
			s = 0+super_block;
		} else
			s++;
	return NULL;
}

void put_super(int dev)
{
	struct super_block * sb;
	/* struct m_inode * inode;*/
	int i;

	if (dev == ROOT_DEV) {
		printk("root diskette changed: prepare for armageddon\n\r");
		return;
	}
	if (!(sb = get_super(dev)))
		return;
	if (sb->s_imount) {
		printk("Mounted disk changed - tssk, tssk\n\r");
		return;
	}
	lock_super(sb);
	sb->s_dev = 0;
	for(i=0;i<I_MAP_SLOTS;i++)
		brelse(sb->s_imap[i]);
	for(i=0;i<Z_MAP_SLOTS;i++)
		brelse(sb->s_zmap[i]);
	free_super(sb);
	return;
}

static struct super_block * read_super(int dev)
{
	struct super_block * s;
	struct buffer_head * bh;
	int i,block;

	if (!dev)
		return NULL;
	check_disk_change(dev);
	if ((s = get_super(dev)))
		return s;
	for (s = 0+super_block ;; s++) {
		if (s >= NR_SUPER+super_block)
			return NULL;
		if (!s->s_dev)
			break;
	}
	s->s_dev = dev;
	s->s_isup = NULL;
	s->s_imount = NULL;
	s->s_time = 0;
	s->s_rd_only = 0;
	s->s_dirt = 0;
	lock_super(s);
	if (!(bh = bread(dev,1))) {
		s->s_dev=0;
		free_super(s);
		return NULL;
	}
	*((struct d_super_block *) s) =
		*((struct d_super_block *) bh->b_data);
	brelse(bh);
	if (s->s_magic != SUPER_MAGIC) {
		s->s_dev = 0;
		free_super(s);
		return NULL;
	}
	for (i=0;i<I_MAP_SLOTS;i++)
		s->s_imap[i] = NULL;
	for (i=0;i<Z_MAP_SLOTS;i++)
		s->s_zmap[i] = NULL;
	block=2;
	for (i=0 ; i < s->s_imap_blocks ; i++)
		if ((s->s_imap[i]=bread(dev,block)))
			block++;
		else
			break;
	for (i=0 ; i < s->s_zmap_blocks ; i++)
		if ((s->s_zmap[i]=bread(dev,block)))
			block++;
		else
			break;
	if (block != 2+s->s_imap_blocks+s->s_zmap_blocks) {
		for(i=0;i<I_MAP_SLOTS;i++)
			brelse(s->s_imap[i]);
		for(i=0;i<Z_MAP_SLOTS;i++)
			brelse(s->s_zmap[i]);
		s->s_dev=0;
		free_super(s);
		return NULL;
	}
	s->s_imap[0]->b_data[0] |= 1;
	s->s_zmap[0]->b_data[0] |= 1;
	free_super(s);
	return s;
}

int sys_umount(char * dev_name)
{
	struct m_inode * inode;
	struct super_block * sb;
	int dev;

	if (!(inode=namei(dev_name)))
		return -ENOENT;
	dev = inode->i_zone[0];
	if (!S_ISBLK(inode->i_mode)) {
		iput(inode);
		return -ENOTBLK;
	}
	iput(inode);
	if (dev==ROOT_DEV)
		return -EBUSY;
	if (!(sb=get_super(dev)) || !(sb->s_imount))
		return -ENOENT;
	if (!sb->s_imount->i_mount)
		printk("Mounted inode has i_mount=0\n");
	for (inode=inode_table+0 ; inode<inode_table+NR_INODE ; inode++)
		if (inode->i_dev==dev && inode->i_count)
				return -EBUSY;
	sb->s_imount->i_mount=0;
	iput(sb->s_imount);
	sb->s_imount = NULL;
	iput(sb->s_isup);
	sb->s_isup = NULL;
	put_super(dev);
	sync_dev(dev);
	return 0;
}

int sys_mount(char * dev_name, char * dir_name, int rw_flag)
{
	struct m_inode * dev_i, * dir_i;
	struct super_block * sb;
	int dev;

	if (!(dev_i=namei(dev_name)))
		return -ENOENT;
	dev = dev_i->i_zone[0];
	if (!S_ISBLK(dev_i->i_mode)) {
		iput(dev_i);
		return -EPERM;
	}
	iput(dev_i);
	if (!(dir_i=namei(dir_name)))
		return -ENOENT;
	if (dir_i->i_count != 1 || dir_i->i_num == ROOT_INO) {
		iput(dir_i);
		return -EBUSY;
	}
	if (!S_ISDIR(dir_i->i_mode)) {
		iput(dir_i);
		return -EPERM;
	}
	if (!(sb=read_super(dev))) {
		iput(dir_i);
		return -EBUSY;
	}
	if (sb->s_imount) {
		iput(dir_i);
		return -EBUSY;
	}
	if (dir_i->i_mount) {
		iput(dir_i);
		return -EPERM;
	}
	sb->s_imount=dir_i;
	dir_i->i_mount=1;
	dir_i->i_dirt=1;		/* NOTE! we don't iput(dir_i) */
	return 0;			/* we do that in umount */
}

void mount_root(void)
{
	int i,free;
	struct super_block * p;
	struct m_inode * mi;

	if (32 != sizeof (struct d_inode))
		panic("bad i-node size");
	for(i=0;i<NR_FILE;i++)
		file_table[i].f_count=0;
	if (MAJOR(ROOT_DEV) == 2) {
		printk("Insert root floppy and press ENTER");
		wait_for_keypress();
	}
	for(p = &super_block[0] ; p < &super_block[NR_SUPER] ; p++) {
		p->s_dev = 0;
		p->s_lock = 0;
		p->s_wait = NULL;
	}
	if (!(p=read_super(ROOT_DEV)))
		panic("Unable to mount root");
	if (!(mi=iget(ROOT_DEV,ROOT_INO)))
		panic("Unable to read root i-node");
	mi->i_count += 3 ;	/* NOTE! it is logically used 4 times, not 1 */
	p->s_isup = p->s_imount = mi;
	current->pwd = mi;
	current->root = mi;
	free=0;
	i=p->s_nzones;
	while (-- i >= 0)
		if (!set_bit(i&8191,p->s_zmap[i>>13]->b_data))
			free++;
	printk("%d/%d free blocks\n\r",free,p->s_nzones);
	free=0;
	i=p->s_ninodes+1;
	while (-- i >= 0)
		if (!set_bit(i&8191,p->s_imap[i>>13]->b_data))
			free++;
	printk("%d/%d free inodes\n\r",free,p->s_ninodes);
}
