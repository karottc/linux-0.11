/*
 *  linux/fs/read_write.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <sys/stat.h>
#include <errno.h>
#include <sys/types.h>

#include <linux/kernel.h>
#include <linux/sched.h>
#include <asm/segment.h>

// 字符设备读写函数。
extern int rw_char(int rw,int dev, char * buf, int count, off_t * pos);
// 读管道操作函数。
extern int read_pipe(struct m_inode * inode, char * buf, int count);
// 写管道操作函数
extern int write_pipe(struct m_inode * inode, char * buf, int count);
// 块设备读操作函数
extern int block_read(int dev, off_t * pos, char * buf, int count);
// 块设备写操作函数
extern int block_write(int dev, off_t * pos, char * buf, int count);
// 读文件操作函数
extern int file_read(struct m_inode * inode, struct file * filp,
		char * buf, int count);
// 写文件操作函数
extern int file_write(struct m_inode * inode, struct file * filp,
		char * buf, int count);

//// 重定位文件读写指针系统调用
// 参数fd是文件句柄，offset是新的文件读写指针偏移值，origin是便宜的起始位置，
// 可有三种选择：SEEK_SET(0, 从文件开始处)、SEEK_CUR（1，从当前读写位置）、
// SEEK_END(2，从文件尾处)。
int sys_lseek(unsigned int fd,off_t offset, int origin)
{
	struct file * file;
	int tmp;

    // 首先判断函数提供的参数有效性。如果文件句柄值大于程序最多打开文件数NR_OPEN(20),
    // 或者该句柄的文件结构指针为空，或者对应文件结构的i节点字段为空，或者指定设备
    // 文件指针是不可定位的，则返回出错码并退出。如果文件对应的i节点是管道节点，则
    // 返回出错码退出。因为管道头尾指针不可随意移动！
	if (fd >= NR_OPEN || !(file=current->filp[fd]) || !(file->f_inode)
	   || !IS_SEEKABLE(MAJOR(file->f_inode->i_dev)))
		return -EBADF;
	if (file->f_inode->i_pipe)
		return -ESPIPE;
    // 然后根据设置的定位标志，分别重新定位文件读写指针。
	switch (origin) {
        // origin = SEEK_SET,要求以文件起始处作为原点设置文件读写指针。若偏移值小于零，
        // 则出错返回错误码。否则设置文件读写指针等于offset。
		case 0:
			if (offset<0) return -EINVAL;
			file->f_pos=offset;
			break;
        // origin = SEEK_CUR, 要求以文件当前读写指针处作为原点重定位读写指针。如果文件
        // 当前指针加上偏移值小于0，则返回出错码退出。否则在当前读写指针上加上偏移值。
		case 1:
			if (file->f_pos+offset<0) return -EINVAL;
			file->f_pos += offset;
			break;
        // origin = SEEK_END,要求以文件末尾作为原点重定位读写指针。此时若文件大小加上
        // 偏移值小于零则返回出错码退出。否则重定位读写指针为文件长度加上偏移值。
		case 2:
			if ((tmp=file->f_inode->i_size+offset) < 0)
				return -EINVAL;
			file->f_pos = tmp;
			break;
        // 若orgin设置无效，返回出错码退出。
		default:
			return -EINVAL;
	}
	return file->f_pos;
}

int sys_read(unsigned int fd,char * buf,int count)
{
	struct file * file;
	struct m_inode * inode;

	if (fd>=NR_OPEN || count<0 || !(file=current->filp[fd]))
		return -EINVAL;
	if (!count)
		return 0;
	verify_area(buf,count);
	inode = file->f_inode;
	if (inode->i_pipe)
		return (file->f_mode&1)?read_pipe(inode,buf,count):-EIO;
	if (S_ISCHR(inode->i_mode))
		return rw_char(READ,inode->i_zone[0],buf,count,&file->f_pos);
	if (S_ISBLK(inode->i_mode))
		return block_read(inode->i_zone[0],&file->f_pos,buf,count);
	if (S_ISDIR(inode->i_mode) || S_ISREG(inode->i_mode)) {
		if (count+file->f_pos > inode->i_size)
			count = inode->i_size - file->f_pos;
		if (count<=0)
			return 0;
		return file_read(inode,file,buf,count);
	}
	printk("(Read)inode->i_mode=%06o\n\r",inode->i_mode);
	return -EINVAL;
}

int sys_write(unsigned int fd,char * buf,int count)
{
	struct file * file;
	struct m_inode * inode;
	
	if (fd>=NR_OPEN || count <0 || !(file=current->filp[fd]))
		return -EINVAL;
	if (!count)
		return 0;
	inode=file->f_inode;
	if (inode->i_pipe)
		return (file->f_mode&2)?write_pipe(inode,buf,count):-EIO;
	if (S_ISCHR(inode->i_mode))
		return rw_char(WRITE,inode->i_zone[0],buf,count,&file->f_pos);
	if (S_ISBLK(inode->i_mode))
		return block_write(inode->i_zone[0],&file->f_pos,buf,count);
	if (S_ISREG(inode->i_mode))
		return file_write(inode,file,buf,count);
	printk("(Write)inode->i_mode=%06o\n\r",inode->i_mode);
	return -EINVAL;
}
