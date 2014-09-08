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

	mode &= 0777 & ~current->umask;
	for(fd=0 ; fd<NR_OPEN ; fd++)
		if (!current->filp[fd])
			break;
	if (fd>=NR_OPEN)
		return -EINVAL;
	current->close_on_exec &= ~(1<<fd);
	f=0+file_table;
	for (i=0 ; i<NR_FILE ; i++,f++)
		if (!f->f_count) break;
	if (i>=NR_FILE)
		return -EINVAL;
	(current->filp[fd]=f)->f_count++;
	if ((i=open_namei(filename,flag,mode,&inode))<0) {
		current->filp[fd]=NULL;
		f->f_count=0;
		return i;
	}
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
	if (S_ISBLK(inode->i_mode))
		check_disk_change(inode->i_zone[0]);
	f->f_mode = inode->i_mode;
	f->f_flags = flag;
	f->f_count = 1;
	f->f_inode = inode;
	f->f_pos = 0;
	return (fd);
}

int sys_creat(const char * pathname, int mode)
{
	return sys_open(pathname, O_CREAT | O_TRUNC, mode);
}

int sys_close(unsigned int fd)
{	
	struct file * filp;

	if (fd >= NR_OPEN)
		return -EINVAL;
	current->close_on_exec &= ~(1<<fd);
	if (!(filp = current->filp[fd]))
		return -EINVAL;
	current->filp[fd] = NULL;
	if (filp->f_count == 0)
		panic("Close: file count is 0");
	if (--filp->f_count)
		return (0);
	iput(filp->f_inode);
	return (0);
}
