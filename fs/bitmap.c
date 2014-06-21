/*
 *  linux/fs/bitmap.c
 *
 *  (C) 1991  Linus Torvalds
 */

/* bitmap.c contains the code that handles the inode and block bitmaps */
#include <string.h>

#include <linux/sched.h>
#include <linux/kernel.h>

//// 将指定地址（addr）处的一块1024字节内存清零
// 输入：eax = 0; ecx = 以字节为单位的数据块长度（BLOCK_SIZE/4）；edi ＝ 指定
// 起始地址addr。
#define clear_block(addr) \
__asm__ __volatile__ ("cld\n\t" \       // 清方向位
	"rep\n\t" \                         // 重复执行存储数据(0).
	"stosl" \
	::"a" (0),"c" (BLOCK_SIZE/4),"D" ((long) (addr)))

//// 把指定地址开始的第nr个位偏移处的bit位置位(nr可大于321).返回原bit位值。
// 输入：%0-eax(返回值)：%1 -eax(0)；%2-nr，位偏移值；%3-(addr)，addr的内容。
// res是一个局部寄存器变量。该变量将被保存在指定的eax寄存器中，以便于高效
// 访问和操作。这种定义变量的方法主要用于内嵌汇编程序中。详细说明可以参考
// gcc手册”在指定寄存器中的变量“。整个宏是一个语句表达式(即圆括号括住的组合句)，
// 其值是组合语句中最后一条表达式语句res的值。
// btsl指令用于测试并设置bit位。把基地址(%3)和bit位偏移值(%2)所指定的bit位值
// 先保存到进位标志CF中，然后设置该bit位为1.指令setb用于根据进位标志CF设置
// 操作数(%al)。如果CF=1则%al = 1,否则%al = 0。
#define set_bit(nr,addr) ({\
register int res ; \
__asm__ __volatile__("btsl %2,%3\n\tsetb %%al": \
"=a" (res):"0" (0),"r" (nr),"m" (*(addr))); \
res;})

//// 复位指定地址开始的第nr位偏移处的bit位。返回原bit位值的反码。
// 输入：%0-eax(返回值)；%1-eax(0)；%2-nr,位偏移值；%3-(addr)，addr的内容。
// btrl指令用于测试并复位bit位。其作用与上面的btsl类似，但是复位指定bit位。
// 指令setnb用于根据进位标志CF设置操作数(%al).如果CF=1则%al=0,否则%al=1.
#define clear_bit(nr,addr) ({\
register int res ; \
__asm__ __volatile__("btrl %2,%3\n\tsetnb %%al": \
"=a" (res):"0" (0),"r" (nr),"m" (*(addr))); \
res;})

//// 从addr开始寻找第1个0值bit位。
// 输入：%0-ecx(返回值)；%1-ecx(0); %2-esi(addr).
// 在addr指定地址开始的位图中寻找第1个是0的bit位，并将其距离addr的bit位偏移
// 值返回。addr是缓冲块数据区的地址，扫描寻找的范围是1024字节（8192bit位）。
#define find_first_zero(addr) ({ \
int __res; \
__asm__ __volatile__ ("cld\n" \         // 清方向位
	"1:\tlodsl\n\t" \                   // 取[esi]→eax.
	"notl %%eax\n\t" \                  // eax中每位取反。
	"bsfl %%eax,%%edx\n\t" \            // 从位0扫描eax中是1的第1个位，其偏移值→edx
	"je 2f\n\t" \                       // 如果eax中全是0，则向前跳转到标号2处。
	"addl %%edx,%%ecx\n\t" \            // 偏移值加入ecx(ecx是位图首个0值位的偏移值)
	"jmp 3f\n" \                        // 向前跳转到标号3处
	"2:\taddl $32,%%ecx\n\t" \          // 未找到0值位，则将ecx加1个字长的位偏移量32
	"cmpl $8192,%%ecx\n\t" \            // 已经扫描了8192bit位(1024字节)
	"jl 1b\n" \                         // 若还没有扫描完1块数据，则向前跳转到标号1处
	"3:" \                              // 结束。此时ecx中是位偏移量。
	:"=c" (__res):"c" (0),"S" (addr)); \
__res;})

void free_block(int dev, int block)
{
	struct super_block * sb;
	struct buffer_head * bh;

	if (!(sb = get_super(dev)))
		panic("trying to free block on nonexistent device");
	if (block < sb->s_firstdatazone || block >= sb->s_nzones)
		panic("trying to free block not in datazone");
	bh = get_hash_table(dev,block);
	if (bh) {
		if (bh->b_count != 1) {
			printk("trying to free block (%04x:%d), count=%d\n",
				dev,block,bh->b_count);
			return;
		}
		bh->b_dirt=0;
		bh->b_uptodate=0;
		brelse(bh);
	}
	block -= sb->s_firstdatazone - 1 ;
	if (clear_bit(block&8191,sb->s_zmap[block/8192]->b_data)) {
		printk("block (%04x:%d) ",dev,block+sb->s_firstdatazone-1);
		panic("free_block: bit already cleared");
	}
	sb->s_zmap[block/8192]->b_dirt = 1;
}

int new_block(int dev)
{
	struct buffer_head * bh;
	struct super_block * sb;
	int i,j;

	if (!(sb = get_super(dev)))
		panic("trying to get new block from nonexistant device");
	j = 8192;
	for (i=0 ; i<8 ; i++)
		if ((bh=sb->s_zmap[i]))
			if ((j=find_first_zero(bh->b_data))<8192)
				break;
	if (i>=8 || !bh || j>=8192)
		return 0;
	if (set_bit(j,bh->b_data))
		panic("new_block: bit already set");
	bh->b_dirt = 1;
	j += i*8192 + sb->s_firstdatazone-1;
	if (j >= sb->s_nzones)
		return 0;
	if (!(bh=getblk(dev,j)))
		panic("new_block: cannot get block");
	if (bh->b_count != 1)
		panic("new block: count is != 1");
	clear_block(bh->b_data);
	bh->b_uptodate = 1;
	bh->b_dirt = 1;
	brelse(bh);
	return j;
}

void free_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;

	if (!inode)
		return;
	if (!inode->i_dev) {
		memset(inode,0,sizeof(*inode));
		return;
	}
	if (inode->i_count>1) {
		printk("trying to free inode with count=%d\n",inode->i_count);
		panic("free_inode");
	}
	if (inode->i_nlinks)
		panic("trying to free inode with links");
	if (!(sb = get_super(inode->i_dev)))
		panic("trying to free inode on nonexistent device");
	if (inode->i_num < 1 || inode->i_num > sb->s_ninodes)
		panic("trying to free inode 0 or nonexistant inode");
	if (!(bh=sb->s_imap[inode->i_num>>13]))
		panic("nonexistent imap in superblock");
	if (clear_bit(inode->i_num&8191,bh->b_data))
		printk("free_inode: bit already cleared.\n\r");
	bh->b_dirt = 1;
	memset(inode,0,sizeof(*inode));
}

struct m_inode * new_inode(int dev)
{
	struct m_inode * inode;
	struct super_block * sb;
	struct buffer_head * bh;
	int i,j;

	if (!(inode=get_empty_inode()))
		return NULL;
	if (!(sb = get_super(dev)))
		panic("new_inode with unknown device");
	j = 8192;
	for (i=0 ; i<8 ; i++)
		if ((bh=sb->s_imap[i]))
			if ((j=find_first_zero(bh->b_data))<8192)
				break;
	if (!bh || j >= 8192 || j+i*8192 > sb->s_ninodes) {
		iput(inode);
		return NULL;
	}
	if (set_bit(j,bh->b_data))
		panic("new_inode: bit already set");
	bh->b_dirt = 1;
	inode->i_count=1;
	inode->i_nlinks=1;
	inode->i_dev=dev;
	inode->i_uid=current->euid;
	inode->i_gid=current->egid;
	inode->i_dirt=1;
	inode->i_num = j + i*8192;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	return inode;
}
