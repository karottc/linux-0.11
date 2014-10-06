/*
 *  linux/fs/fcntl.c
 *
 *  (C) 1991  Linus Torvalds
 */

/* #include <string.h> */
#include <errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

#include <fcntl.h>
#include <sys/stat.h>

extern int sys_close(int fd);   // 关闭文件系统调用

//// 复制文件句柄
// 参数fd是欲复制的文件句柄，arg指定新文件句柄的最小值。
// 返回新文件句柄或出错。
static int dupfd(unsigned int fd, unsigned int arg)
{
    // 首先检查函数的有效性。如果文件句柄值大于一个程序最多打开文件数NR_OPEN，或
    // 者该句柄的文件结构不存在，则返回出错码并退出。如果指定的新句柄值arg大于最
    // 多打开文件数，也返回出错码并退出。注意，实际上文件句柄就是进程文件结构指
    // 针数组项索引号。
	if (fd >= NR_OPEN || !current->filp[fd])
		return -EBADF;
	if (arg >= NR_OPEN)
		return -EINVAL;
    // 然后在当前进程的文件结构指针数组中寻找索引号等于或大于arg，但还没有使用
    // 的项。若找到的新句柄值arg大于最多打开文件数(即没有空闲项)，则返回出错码并退出。
	while (arg < NR_OPEN)
		if (current->filp[arg])
			arg++;
		else
			break;
	if (arg >= NR_OPEN)
		return -EMFILE;
    // 否则针对找到的空闲项(句柄)，在执行时关闭标志位图close_on_exec中复位该句
    // 柄位。即在运行exec()类函数时，不会关闭用dup()创建的句柄。并令该文件结构
    // 指针等于原句柄fd的指针，并且将文件引用计数增1，最后返回新的文件句柄arg.
	current->close_on_exec &= ~(1<<arg);
	(current->filp[arg] = current->filp[fd])->f_count++;
	return arg;
}

//// 复制文件句柄系统调用
// 复制指定文件句柄oldfd，新文件句柄值等于newfd，如果newfd已打开，则首先关闭之。
// 参数：oldfd - 原文件句柄；newfd - 新文件句柄。
// 返回新文件句柄值。
int sys_dup2(unsigned int oldfd, unsigned int newfd)
{
	sys_close(newfd);
	return dupfd(oldfd,newfd);
}

//// 复制文件句柄系统调用
// 复制指定文件句柄oldfd，新句柄的值是当前最小的未用句柄值。
// 参数：fileds - 被复制的文件句柄。
// 返回新文件句柄值。
int sys_dup(unsigned int fildes)
{
	return dupfd(fildes,0);
}

//// 文件控制系统调用函数。
// 参数fd是文件句柄：cmd是控制命令; arg则针对不同的命令有不同的含义。对于复制句
// 柄命令F_DUPFD，arg是新文件句柄可取的最小值；对于设置文件操作和访问标志命令
// F_SETFL，arg是新的文件操作和访问模式。对于文件上锁命令F_GETLK、F_SETLK和
// F_SETLKW，arg是指向flock结构的指针。但本内核中没有实现文件上锁功能。
// 返回：若出错，则所有操作都返回-1。若成功，那么F_DUPFD返回新文件句柄；F_GETFD
// 返回文件句柄的当前执行是关闭标志close_on_exec;F_GETFL返回文件操作和访问标志。
int sys_fcntl(unsigned int fd, unsigned int cmd, unsigned long arg)
{	
	struct file * filp;

    // 首先检查给出的文件句柄的有效性。然后根据不同命令cmd进行分别处理。如果文件
    // 句柄值大于一个进程最多打开文件数NR_OPEN，或者该句柄的文件结构指针为空，则
    // 返回出错码并退出。
	if (fd >= NR_OPEN || !(filp = current->filp[fd]))
		return -EBADF;
	switch (cmd) {
		case F_DUPFD:
			return dupfd(fd,arg);
		case F_GETFD:
			return (current->close_on_exec>>fd)&1;
		case F_SETFD:
			if (arg&1)
				current->close_on_exec |= (1<<fd);
			else
				current->close_on_exec &= ~(1<<fd);
			return 0;
		case F_GETFL:
			return filp->f_flags;
		case F_SETFL:
			filp->f_flags &= ~(O_APPEND | O_NONBLOCK);
			filp->f_flags |= arg & (O_APPEND | O_NONBLOCK);
			return 0;
		case F_GETLK:	case F_SETLK:	case F_SETLKW:
			return -1;
		default:
			return -1;
	}
}
