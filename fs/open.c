/*
 *  linux/fs/open.c
 *
 *  (C) 1991  Linus Torvalds
 */

/* #include <string.h> */
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <utime.h>
#include <sys/stat.h>

#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/kernel.h>
#include <asm/segment.h>

//// 取文件系统信息。
// 参数dev是含有已安装文件系统的设备号。ubuf是一个结构缓冲区指针，用户存放系统
// 返回的文件系统信息。该系统调用用于返回已安装文件系统的统计信息。成功时返回
// 0，并且ubuf指向的ustate结构被添入文件系统总空闲块数和空闲i节点数。ustat结构
// 定义在types.h中。
int sys_ustat(int dev, struct ustat * ubuf)
{
	return -ENOSYS;
}

//// 设置文件访问和修改时间
// 参数filename是文件名，times是访问和修改时间结构指针。
// 如果times指针不为NULL，则取utimebuf结构中的时间信息来设置文件的访问和修改时
// 间。如果times指针是NULL,则取系统当前时间来设置指定文件的访问和修改时间域。
int sys_utime(char * filename, struct utimbuf * times)
{
	struct m_inode * inode;
	long actime,modtime;

    // 文件的时间信息保存在其i节点中。因此我们首先根据文件名取得对应的i节点。
    // 如果没有找到，则返回出错码。
	if (!(inode=namei(filename)))
		return -ENOENT;
    // 如果提供的访问和修改时间结构指针times不为NULL，则从结构中读取用户设置的
    // 时间值。否则就用系统当前时间来设置文件的访问和修改时间。
	if (times) {
		actime = get_fs_long((unsigned long *) &times->actime);
		modtime = get_fs_long((unsigned long *) &times->modtime);
	} else
		actime = modtime = CURRENT_TIME;
    // 然后修改i节点中的访问时间字段和修改时间字段。再设置i节点已修改标志，放回
    // 该i节点并返回0.
	inode->i_atime = actime;
	inode->i_mtime = modtime;
	inode->i_dirt = 1;
	iput(inode);
	return 0;
}

/*
 * XXX should we use the real or effective uid?  BSD uses the real uid,
 * so as to make this call useful to setuid programs.
 */
//// 检查文件的访问权限。
// 参数filename是文件名，mode是检查的访问属性，它有3个有效bit位组成：R_OK(值4)、
// W_OK(2)、X_OK(1)和F_OK(0)组成，分别表示检测文件是否可读、可写、可执行和文件
// 是否存在。如果访问允许的话，则返回0，否则返回出错码。
int sys_access(const char * filename,int mode)
{
	struct m_inode * inode;
	int res, i_mode;

    // 文件的访问权限信息也同样保存在文件的i节点结构中，因此我们要先取得对应文
    // 件名的i节点。检测的访问属性mode由低3位组成，因此需要与上八进制007来清除
    // 所有高bit位。如果文件名对应的i节点不存在，则返回没有许可权限出错码。若i
    // 节点存在，则取i节点中文件属性码，并放回该i节点。
	mode &= 0007;
	if (!(inode=namei(filename)))
		return -EACCES;
	i_mode = res = inode->i_mode & 0777;
	iput(inode);  // 这句应该房子下面else if之后。
    // 如果当前进程用户是该文件的宿主，则取文件宿主属性。否则如果当前进程用户与
    // 该文件宿主同属一组，则取文件组属性。否则，此时res最低3 bit是其他人访问
    // 的许可属性。
    // [[?? 这里应 res >> 3 ??]]
	if (current->uid == inode->i_uid)
		res >>= 6;
	else if (current->gid == inode->i_gid)
		res >>= 6;
    // 此时res的最低3 bit是根据当前进程用户与文件的关系选择出来的访问属性位。
    // 现在我们来判断这3 bit.如果文件属性具有参数所查询的属性位mode，则访问许
    // 可，返回0.
	if ((res & 0007 & mode) == mode)
		return 0;
	/*
	 * XXX we are doing this test last because we really should be
	 * swapping the effective with the real user id (temporarily),
	 * and then calling suser() routine.  If we do call the
	 * suser() routine, it needs to be called last. 
	 */
    // 如果当前用户ID为0(超级用户)并且屏蔽码执行位是0或者文件可以被任何人执行、
    // 搜索，则返回0，否则返回出错码。
	if ((!current->uid) &&
	    (!(mode & 1) || (i_mode & 0111)))
		return 0;
	return -EACCES;
}

//// 改变当前工作目录系统调用
// 参数filename是目录名
// 操作成功则返回0，否则返回出错码
int sys_chdir(const char * filename)
{
	struct m_inode * inode;

    // 改变当前工作目录就是要求把 进程任务结构的当前工作目录字段指向给定目录名
    // 的i节点。因此我们首先取目录名的i节点。如果目录名对应的i节点不存在，则返
    // 回出错码。如果该i节点不是一个目录i节点，则放回该i节点，并返回出错码。
	if (!(inode = namei(filename)))
		return -ENOENT;
	if (!S_ISDIR(inode->i_mode)) {
		iput(inode);
		return -ENOTDIR;
	}
    // 然后释放进程原工作目录i节点，并使其指向新设置的工作目录i节点，返回0.
	iput(current->pwd);
	current->pwd = inode;
	return (0);
}

//// 改变根目录系统调用
// 把指定的目录名设置成当前进程的根目录'/'.
// 如果操作成功则返回0，否则返回出错码。
int sys_chroot(const char * filename)
{
	struct m_inode * inode;

    // 该调用用于改变当前进程任务结构中的根目录字段root，让其执行参数给定目录名
    // 的i节点。如果目录名对应的i节点不存在，则返回出错码。如果该i节点不是目录i
    // 节点，则放回该i节点，也返回出错码。
	if (!(inode=namei(filename)))
		return -ENOENT;
	if (!S_ISDIR(inode->i_mode)) {
		iput(inode);
		return -ENOTDIR;
	}
    // 然后释放当前进程的根目录i节点，并重新设置为指定目录名的i节点，返回0。
	iput(current->root);
	current->root = inode;
	return (0);
}

//// 修改文件属性系统调用
// 参数filename是文件名，mode是新的文件属性
// 若操作成功则返回0，否则返回出错码。
int sys_chmod(const char * filename,int mode)
{
	struct m_inode * inode;

    // 该调用为指定文件设置新的访问属性Mode。文件的访问属性在文件名对应的i节点
    // 中，因此我们首先取文件名对应的i节点。如果i节点不存在，则返回出错码(文件或
    // 目录不存在)。如果当前进程是有效用户id与文件i节点的用户id不同，并且也不是
    // 超级用户，则放回该文件i节点，返回出错码(没有访问权限)。
	if (!(inode=namei(filename)))
		return -ENOENT;
	if ((current->euid != inode->i_uid) && !suser()) {
		iput(inode);
		return -EACCES;
	}
    // 否则就重新设置该i节点的文件属性，并置该i节点的已修改标志。放回该i节点，返回0.
	inode->i_mode = (mode & 07777) | (inode->i_mode & ~07777);
	inode->i_dirt = 1;
	iput(inode);
	return 0;
}

///// 修改文件宿主系统调用
// 参数filename是文件名，uid是用户标识符(用户ID)，gid是组ID。
// 若操作成功则返回0，否则返回出错码。
int sys_chown(const char * filename,int uid,int gid)
{
	struct m_inode * inode;

    // 该调用用于设置文件i节点中的用户和组ID，因此首先要取得给定文件名的i节点。
    // 如果文件名的i节点不存在，则返回出错码(文件或目录不存在)。如果当前进程不
    // 是超级用户，则放回该i节点，并返回出错码(没有访问权限)。
	if (!(inode=namei(filename)))
		return -ENOENT;
	if (!suser()) {
		iput(inode);
		return -EACCES;
	}
    // 否则我们就用参数提供的值来设置文件i节点的用户ID和组ID，并置i节点已修改
    // 标志，放回该节点，返回0.
	inode->i_uid=uid;
	inode->i_gid=gid;
	inode->i_dirt=1;
	iput(inode);
	return 0;
}

//// 打开（或创建）文件系统调用。
// 参数filename是文件名，flag是打开文件标志，它可取值：O_RDONLY（只读）、O_WRONLY
// （只写）或O_RDWR(读写)，以及O_EXCL（被创建文件必须不存在）、O_APPEND（在文件
// 尾添加数据）等其他一些标志的组合。如果本调用创建了一个新文件，则mode就用于指
// 定文件的许可属性。这些属性有S_IRWXU（文件宿主具有读、写和执行权限）、S_IRUSR
// （用户具有读文件权限）、S_IRWXG（组成员具有读、写和执行权限）等等。对于新创
// 建的文件，这些属性只应用与将来对文件的访问，创建了只读文件的打开调用也将返回
// 一个可读写的文件句柄。如果调用操作成功，则返回文件句柄(文件描述符)，否则返回出错码。
int sys_open(const char * filename,int flag,int mode)
{
	struct m_inode * inode;
	struct file * f;
	int i,fd;

    // 首先对参数进行处理。将用户设置的文件模式和屏蔽码相与，产生许可的文件模式。
    // 为了为打开文件建立一个文件句柄，需要搜索进程结构中文件结构指针数组，以查
    // 找一个空闲项。空闲项的索引号fd即是文件句柄值。若已经没有空闲项，则返回出错码。
	mode &= 0777 & ~current->umask;
	for(fd=0 ; fd<NR_OPEN ; fd++)
		if (!current->filp[fd])
			break;
	if (fd>=NR_OPEN)
		return -EINVAL;
    // 然后我们设置当前进程的执行时关闭文件句柄(close_on_exec)位图，复位对应的
    // bit位。close_on_exec是一个进程所有文件句柄的bit标志。每个bit位代表一个打
    // 开着的文件描述符，用于确定在调用系统调用execve()时需要关闭的文件句柄。当
    // 程序使用fork()函数创建了一个子进程时，通常会在该子进程中调用execve()函数
    // 加载执行另一个新程序。此时子进程中开始执行新程序。若一个文件句柄在close_on_exec
    // 中的对应bit位被置位，那么在执行execve()时应对应文件句柄将被关闭，否则该
    // 文件句柄将始终处于打开状态。当打开一个文件时，默认情况下文件句柄在子进程
    // 中也处于打开状态。因此这里要复位对应bit位。
	current->close_on_exec &= ~(1<<fd);
    // 然后为打开文件在文件表中寻找一个空闲结构项。我们令f指向文件表数组开始处。
    // 搜索空闲文件结构项(引用计数为0的项)，若已经没有空闲文件表结构项，则返回
    // 出错码。
	f=0+file_table;
	for (i=0 ; i<NR_FILE ; i++,f++)
		if (!f->f_count) break;
	if (i>=NR_FILE)
		return -EINVAL;
    // 此时我们让进程对应文件句柄fd的文件结构指针指向搜索到的文件结构，并令文件
    // 引用计数递增1。然后调用函数open_namei()执行打开操作，若返回值小于0，则说
    // 明出错，于是释放刚申请到的文件结构，返回出错码i。若文件打开操作成功，则
    // inode是已打开文件的i节点指针。
	(current->filp[fd]=f)->f_count++;
	if ((i=open_namei(filename,flag,mode,&inode))<0) {
		current->filp[fd]=NULL;
		f->f_count=0;
		return i;
	}
    // 根据已打开文件的i节点的属性字段，我们可以知道文件的具体类型。对于不同类
    // 型的文件，我们需要操作一些特别的处理。如果打开的是字符设备文件，那么对于
    // 主设备号是4的字符文件(例如/dev/tty0)，如果当前进程是组首领并且当前进程的
    // tty字段小于0(没有终端)，则设置当前进程的tty号为该i节点的子设备号，并设置
    // 当前进程tty对应的tty表项的父进程组号等于当前进程的进程组号。表示为该进程
    // 组（会话期）分配控制终端。对于主设备号是5的字符文件(/dev/tty)，若当前进
    // 程没有tty，则说明出错，于是放回i节点和申请到的文件结构，返回出错码(无许可)。
/* ttys are somewhat special (ttyxx major==4, tty major==5) */
	if (S_ISCHR(inode->i_mode)) {
		if (MAJOR(inode->i_zone[0])==4) {
			if (current->leader && current->tty<0) {
				current->tty = MINOR(inode->i_zone[0]);
				tty_table[current->tty].pgrp = current->pgrp;
			}
		} else if (MAJOR(inode->i_zone[0])==5)
			if (current->tty<0) {
				iput(inode);
				current->filp[fd]=NULL;
				f->f_count=0;
				return -EPERM;
			}
	}
/* Likewise with block-devices: check for floppy_change */
    // 如果打开的是块设备文件，则检查盘片是否更换过。若更换过则需要让高速缓冲区
    // 中该设备的所有缓冲块失败。
	if (S_ISBLK(inode->i_mode))
		check_disk_change(inode->i_zone[0]);
    // 现在我们初始化打开文件的文件结构。设置文件结构属性和标志，置句柄引用计数
    // 为1，并设置i节点字段为打开文件的i节点，初始化文件读写指针为0.最后返回文
    // 件句柄号。
	f->f_mode = inode->i_mode;
	f->f_flags = flag;
	f->f_count = 1;
	f->f_inode = inode;
	f->f_pos = 0;
	return (fd);
}

//// 创建文件系统调用
// 参数pathname是路径名，mode与上面的sys_open()函数相同。
// 成功则返回文件句柄，否则返回出错码。
int sys_creat(const char * pathname, int mode)
{
	return sys_open(pathname, O_CREAT | O_TRUNC, mode);
}

//// 关闭文件系统调用
// 参数fd是文件句柄。
// 成功则返回0，否则返回出错码。
int sys_close(unsigned int fd)
{	
	struct file * filp;

    // 首先检查参数有效性。若给出的文件句柄值大于程序同时能打开的文件数NR_OPEN，
    // 则返回出错码。然后复位进程的执行关闭文件句柄位图对应位。若该文件句柄对应
    // 的文件结构指针是NULL，则返回出错码。
	if (fd >= NR_OPEN)
		return -EINVAL;
	current->close_on_exec &= ~(1<<fd);
	if (!(filp = current->filp[fd]))
		return -EINVAL;
    // 现在置该文件句柄的文件结构指针为NULL。若在关闭文件之前，对应文件结构中的
    // 句柄引用计数已经为0，则说明内核出错，停机。否则将对应的文件结构引用计数
    // 减1.此时如果它还不为0，则说明有其他进程正在使用该文件，于是返回0（成功）。
    // 如果引用计数等于0，说明该文件已经没有进程引用，该文件结构已变为空闲。则
    // 释放该文件i节点，返回0.
	current->filp[fd] = NULL;
	if (filp->f_count == 0)
		panic("Close: file count is 0");
	if (--filp->f_count)
		return (0);
	iput(filp->f_inode);
	return (0);
}
