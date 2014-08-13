/*
 *  linux/fs/file_dev.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <errno.h>
#include <fcntl.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

//// 文件读函数 - 根据i节点和文件结构，读取文件中数据。
// 由i节点我们可以知道设备号，由filp结构可以知道文件中当前读写指针位置。buf指定
// 用户空间中缓冲区位置，count是需要读取字节数。返回值是实际读取的字节数，或出错号(小于0).
int file_read(struct m_inode * inode, struct file * filp, char * buf, int count)
{
	int left,chars,nr;
	struct buffer_head * bh;

    // 首先判断参数的有效性。若需要读取的字节数count小于等于0，则返回0.若还需要读
    // 取的字节数不等于0，就循环执行下面操作，直到数据全部读出或遇到问题。在读循环
    // 操作过程中，我们根据i节点和文件表结构信息，并利用bmap()得到包含文件当前读写
    // 位置的数据块在设备上对应的逻辑块号nr。若nr不为0，则从i节点指定的设备上读取该
    // 逻辑块。如果读操作是吧则退出循环。若nr为0，表示指定的数据块不存在，置缓冲块
    // 指针为NULL。(filp->f_pos)/BLOCK_SIZE用于计算出文件当前指针所在的数据块号。
	if ((left=count)<=0)
		return 0;
	while (left) {
		if ((nr = bmap(inode,(filp->f_pos)/BLOCK_SIZE))) {
			if (!(bh=bread(inode->i_dev,nr)))
				break;
		} else
			bh = NULL;
        // 接着我们计算文件读写指针在数据块中的偏移值nr，则在该数据块中我们希望读取的
        // 字节数为(BLOCK_SIZE-nr)。然后和现在还需读取的字节数left做比较。其中小值
        // 即为本次操作需读取的字节数chars。如果(BLOCK_SIZE-nr) > left，则说明该块
        // 是需要读取的最后一块数据。反之还需要读取下一块数据。之后调整读写文件指针。
        // 指针前移此次将读取的字节数chars，剩余字节计数left相应减去chars。
		nr = filp->f_pos % BLOCK_SIZE;
		chars = MIN( BLOCK_SIZE-nr , left );
		filp->f_pos += chars;
		left -= chars;
        // 若上面从设备上读到了数据，则将p指向缓冲块中开始读取数据的位置，并且复制chars
        // 字节到用户缓冲区buf中。否则往用户缓冲区中填入chars个0值字节。
		if (bh) {
			char * p = nr + bh->b_data;
			while (chars-->0)
				put_fs_byte(*(p++),buf++);
			brelse(bh);
		} else {
			while (chars-->0)
				put_fs_byte(0,buf++);
		}
	}
    // 修改该i节点的访问时间为当前时间。返回读取的字节数，若读取字节数为0，则返回
    // 出错号。CURRENT_TIME是定义在include/linux/sched.h中的宏，用于计算UNIX时间。
    // 即从1970年1月1日0时0分0秒开始，到当前的时间，单位是秒。
	inode->i_atime = CURRENT_TIME;
	return (count-left)?(count-left):-ERROR;
}

int file_write(struct m_inode * inode, struct file * filp, char * buf, int count)
{
	off_t pos;
	int block,c;
	struct buffer_head * bh;
	char * p;
	int i=0;

/*
 * ok, append may not work when many processes are writing at the same time
 * but so what. That way leads to madness anyway.
 */
	if (filp->f_flags & O_APPEND)
		pos = inode->i_size;
	else
		pos = filp->f_pos;
	while (i<count) {
		if (!(block = create_block(inode,pos/BLOCK_SIZE)))
			break;
		if (!(bh=bread(inode->i_dev,block)))
			break;
		c = pos % BLOCK_SIZE;
		p = c + bh->b_data;
		bh->b_dirt = 1;
		c = BLOCK_SIZE-c;
		if (c > count-i) c = count-i;
		pos += c;
		if (pos > inode->i_size) {
			inode->i_size = pos;
			inode->i_dirt = 1;
		}
		i += c;
		while (c-->0)
			*(p++) = get_fs_byte(buf++);
		brelse(bh);
	}
	inode->i_mtime = CURRENT_TIME;
	if (!(filp->f_flags & O_APPEND)) {
		filp->f_pos = pos;
		inode->i_ctime = CURRENT_TIME;
	}
	return (i?i:-1);
}
