/*
 *  linux/fs/block_dev.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <errno.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>
#include <asm/system.h>

//// 数据块写函数 - 向指定设备从给定偏移出写入制定长度数据。
// 参数：dev - 设备号； pos - 设备文件中偏移量指针；buf - 用户空间中缓冲区地址；
// count - 要传送的字节数
// 返回已写入字节数。若没有写入任何字节或出错，则返回出错号。
// 对于内核来说，写操作是向高速缓冲区中写入数据。什么时候数据最终写入设备是由高
// 速缓冲管理程序决定并处理的。另外，因为块设备是以块为单位进行读写，因此对于写
// 开始位置不处于块起始处时，需要先将开始字节所在的整个块读出，然后将需要写的数
// 据从写开始处填写满该块，再将完整的一块数据写盘（即交由高速缓冲程序去处理）。
int block_write(int dev, long * pos, char * buf, int count)
{
    // 首先由文件中位置pos换算成开始读写盘快的块序号block，并求出需写第1字节在该
    // 块中的偏移位置offset.
	int block = *pos >> BLOCK_SIZE_BITS;        // pos所在文件数据块号
	int offset = *pos & (BLOCK_SIZE-1);         // pos在数据块中偏移值
	int chars;
	int written = 0;
	struct buffer_head * bh;
	register char * p;                          // 局部寄存器变量，被存放在寄存器中

    // 然后针对要写入的字节数count，循环执行以下操作，知道数据全部写入。在循环执行
    // 过程中，先计算在当前处理的数据块中可写入的字节数。如果写入的字节数填不满一块，
    // 那么就只需写count字节。如果正要写1块数据内容，则直接申请1块高速缓冲块，并把
    // 用户数据放入即可。否则就需要读入将被写入部分数据的数据块，并预读下两块数据。
    // 然后将块号递增1，为下次操作做好准备。如果缓冲块操作失败，则返回已写字节数，
    // 如果没有写入任何字节，则返回出错号(负数).
	while (count>0) {
		chars = BLOCK_SIZE - offset;
		if (chars > count)
			chars=count;
		if (chars == BLOCK_SIZE)
			bh = getblk(dev,block);
		else
			bh = breada(dev,block,block+1,block+2,-1);
		block++;
		if (!bh)
			return written?written:-EIO;
        // 接着先把指针p指向读出数据的缓冲块中开始写入数据的位置处。若最后一次循环写入
        // 的数据不足一块，则需从块开始处填写（修改）所需的字节，因此这里需预先设置offset
        // 为零。此后将文件中偏移指针pos前移此次将要写的字节数chars，并累加这些要写的
        // 字节数到统计值written中，再把还需要写的计数值count减去此次要写的字节数chars.
        // 然后我们从用户缓冲区复制chars个字节到p指向的高速缓冲中开始写入的位置处。复制
        // 完后就设置该缓冲区块已修改标志，并释放该缓冲区(也即该缓冲区引用计数递减1)。
		p = offset + bh->b_data;
		offset = 0;
		*pos += chars;
		written += chars;           // 累计写入字节数
		count -= chars;
		while (chars-->0)
			*(p++) = get_fs_byte(buf++);
		bh->b_dirt = 1;
		brelse(bh);
	}
	return written;
}

//// 数据块读函数 - 从指定设备和位置处读入指定长度数据到用户缓冲区中。
// 参数：dev - 设备号；pos - 设备文件中偏移量指针；buf - 用户空间缓冲区地址；
// count - 要传送的字节数。
// 返回已读入字节数。若没有读入任何字节或出错，则返回出错号。
int block_read(int dev, unsigned long * pos, char * buf, int count)
{
    // 首先由文件中位置pos换算成开始读写盘块的块序号block，并求出需读第1个字节在块中
    // 的偏移位置offset.
	int block = *pos >> BLOCK_SIZE_BITS;
	int offset = *pos & (BLOCK_SIZE-1);
	int chars;
	int read = 0;
	struct buffer_head * bh;
	register char * p;

    // 然后针对要读入的字节数count，循环执行以下操作，直到数据全部读入。在循环执行
    // 过程中，先计算在当前处理的数据块中需读入的字节数。如果需要读入的字节数还不满
    // 一块，那么就只需要读count字节。然后调用读块函数breada()读如需要的数据块，并
    // 预读下两块数据，如果读操作出错，则返回已读字节数，如果没有读入任何字节，则
    // 返回出错号。然后将块号递增1.为下次操作做好准备。如果缓冲块操作失败，则返回已
    // 写字节数，如果没有读入任何字节，则返回出错号（负数）。
	while (count>0) {
		chars = BLOCK_SIZE-offset;
		if (chars > count)
			chars = count;
		if (!(bh = breada(dev,block,block+1,block+2,-1)))
			return read?read:-EIO;
		block++;
        // 接着先把指针p指向读出盘块的缓冲中开始读入数据的位置处。若最后一次循环读
        // 操作的数据不足一块，则需从块起始处读取所需字节，因此这里需预先设置offset
        // 为零。此后将文件中偏移指针pos前移此次将要读的字节数chars,并且累加这些要读
        // 的字节数到统计值read中。再把还需要读的计数值count减去此次要读的字节数chars。
        // 然后我们从高速缓冲块中p指向的开始读的位置处复制chars个字节到用户缓冲区中，
        // 同时把用户缓冲区指针前移。本次复制完后就释放该缓冲块。
		p = offset + bh->b_data;
		offset = 0;
		*pos += chars;
		read += chars;                      // 读入累计字节数
		count -= chars;
		while (chars-->0)
			put_fs_byte(*(p++),buf++);
		brelse(bh);
	}
	return read;
}
