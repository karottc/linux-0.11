/*
 *  linux/kernel/sys.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <errno.h>

#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/kernel.h>
#include <asm/segment.h>
#include <sys/times.h>
#include <sys/utsname.h>

// 返回日期和时间
// 以下返回值是-ENOSYS的系统调用函数均表示在本版本内核中还未实现。
int sys_ftime()
{
	return -ENOSYS;
}

int sys_break()
{
	return -ENOSYS;
}

// 用于当前进程对子进程进行调试(debugging)
int sys_ptrace()
{
	return -ENOSYS;
}

// 改变并打印终端行设置
int sys_stty()
{
	return -ENOSYS;
}

// 取终端设置信息
int sys_gtty()
{
	return -ENOSYS;
}

// 修改文件名
int sys_rename()
{
	return -ENOSYS;
}

int sys_prof()
{
	return -ENOSYS;
}

// 设置当前任务的实际以及/或者有效组ID(gid)。如果任务没有超级用户权限，
// 那么只能互相换其实际组ID和有效组ID。如果任务具有超级用户权限，就能
// 任意设置有效和实际的组ID。保留的gid(saved gid)被设置成与有效gid同值。
int sys_setregid(int rgid, int egid)
{
	if (rgid>0) {
		if ((current->gid == rgid) || 
		    suser())
			current->gid = rgid;
		else
			return(-EPERM);
	}
	if (egid>0) {
		if ((current->gid == egid) ||
		    (current->egid == egid) ||
		    (current->sgid == egid) ||
		    suser())
			current->egid = egid;
		else
			return(-EPERM);
	}
	return 0;
}

// 设置进程组号(gid).如果任务没有超级用户特权，它可以使用setgid()将其有效
// gid(effective gid)设置为成其保留gid(saved gid)或其实际gid（real gid）.
// 如果任务有超级用户权限，则实际gid、有效gid和保留gid都被设置成参数指定的gid.
int sys_setgid(int gid)
{
	return(sys_setregid(gid, gid));
}

// 打开或关闭进程计帐功能
int sys_acct()
{
	return -ENOSYS;
}

// 映射任务物理内存到进程的虚拟地址空间
int sys_phys()
{
	return -ENOSYS;
}

int sys_lock()
{
	return -ENOSYS;
}

int sys_mpx()
{
	return -ENOSYS;
}

int sys_ulimit()
{
	return -ENOSYS;
}

// 返回从1970年1月1日00:00:00 GMT开始计时的时间值(秒)。如果tloc不为null,
// 则时间值也存储在那里。
// 由于参数是一个指针，而其所指位置在用户空间，因此需要使用函数put_fs_long
// 来访问该值。在进入内核中运行时，段寄存器fs被默认地指向当前用户数据空间。
// 因此该函数就可利用fs来访问用户空间中的值。
int sys_time(long * tloc)
{
	int i;

	i = CURRENT_TIME;
	if (tloc) {
		verify_area(tloc,4);                    // 验证内存容量是否足够
		put_fs_long(i,(unsigned long *)tloc);   // 也放入用户数据段tloc处
	}
	return i;
}

/*
 * Unprivileged users may change the real user id to the effective uid
 * or vice versa.
 */
// 设置任务的实际以及/或者有效的用户ID(uid).如果任务没有超级用户特权，那么
// 只能互换其实际的uid和有效的Uid。如果任务具有超级用户特权，就能任意设置
// 有效的和实际的用户ID。保留的uid(saved uid)被设置成与有效uid同值。
int sys_setreuid(int ruid, int euid)
{
	int old_ruid = current->uid;
	
	if (ruid>0) {
		if ((current->euid==ruid) ||
                    (old_ruid == ruid) ||
		    suser())
			current->uid = ruid;
		else
			return(-EPERM);
	}
	if (euid>0) {
		if ((old_ruid == euid) ||
                    (current->euid == euid) ||
		    suser())
			current->euid = euid;
		else {
			current->uid = old_ruid;
			return(-EPERM);
		}
	}
	return 0;
}

// 设置任务用户ID(uid).如果任务没有超级用户特权，它可以使用setuid()将其有效
// 的uid(effective uid)设置成其保存的uid（saved uid）或其实际的uid(real uid)。
// 如果任务有超级用户特权，则实际的uid、有效的uid和保存的uid都会被设置成参数
// 指定的uid。
int sys_setuid(int uid)
{
	return(sys_setreuid(uid, uid));
}

// 设置系统开机时间。参数tptr是从1970年1月1日00:00:00 GMT 开始计时的时间值(秒)。
// 调用进程必须具有超级用户权限。其中HZ=100,是内核系统运行频率。
// 由于参数是一个指针，而其所指位置在用户空间，因此需要使用函数get_fs_long来访问
// 该值。在进入内核中运行时，段寄存器fs被默认地指向用户数据空间。因此该函数
// 就可利用fs来访问用户空间中的值。
// 函数参数提供的当前时间值减去系统已经运行的时间秒值(jeffies/HZ)即是开机时间秒值。
int sys_stime(long * tptr)
{
	if (!suser())               // 如果不是超级用户则出错返回(许可)
		return -EPERM;
	startup_time = get_fs_long((unsigned long *)tptr) - jiffies/HZ;
	return 0;
}

// 获取当前任务运行时间统计值。tms结构中包括进程用户时间、内核(系统)时间、子进程
// 用户运行时间、子进程系统运行时间。函数返回值是系统运行到当前的滴答数。
int sys_times(struct tms * tbuf)
{
	if (tbuf) {
		verify_area(tbuf,sizeof *tbuf);
		put_fs_long(current->utime,(unsigned long *)&tbuf->tms_utime);
		put_fs_long(current->stime,(unsigned long *)&tbuf->tms_stime);
		put_fs_long(current->cutime,(unsigned long *)&tbuf->tms_cutime);
		put_fs_long(current->cstime,(unsigned long *)&tbuf->tms_cstime);
	}
	return jiffies;
}

// 当参数end_data_seg数值合理，并且系统确实有足够的内存，而且进程没有超越其最大
// 数据段大小时，该函数设置数据段末尾为end_data_seg指定的值。该值必须大于代码结尾
// 并且要小于堆栈结尾16KB.返回值是数据段的新结尾值(如果返回值与要求值不同，则表
// 明有错误发生)。该函数并不被用户直接调用，而由libc库函数进行包装，并且返回值也不一样。
int sys_brk(unsigned long end_data_seg)
{
    // 如果参数值大于代码结尾，并且小于(堆栈 - 16KB)，则设置新数据段结尾值
	if (end_data_seg >= current->end_code &&
	    end_data_seg < current->start_stack - 16384)
		current->brk = end_data_seg;
	return current->brk;                // 返回进程当前的数据段结尾值
}

/*
 * This needs some heave checking ...
 * I just haven't get the stomach for it. I also don't fully
 * understand sessions/pgrp etc. Let somebody who does explain it.
 */
// 设置指定进程pid的进程组号为pgid。
// 参数pid是指定进程的进程号。如果它为0，则让pid等于当前进程的进程号。参数
// pgid是指定的进程组号。如果它为0，则让它等于进程pid的进程组号。如果该函数用
// 于将进程从一个进程组移到另一个进程组，则这两个进程组必须属于同一个会话(session)。
// 在这种情况下，参数pgid指定了要加入的现有进程组ID，此时该组的会话ID必须与将要
// 加入进程的相同。
int sys_setpgid(int pid, int pgid)
{
	int i;

    // 如果参数pid=0,则使用当前进程号。如果pgid＝0，则使用当前进程Pid作为pgid。
    // 【？？这里与POSIX标准的描述有出入】
	if (!pid)
		pid = current->pid;
	if (!pgid)
		pgid = current->pid;
    // 扫描任务数组，查找指定进程号pid的任务。如果找到了进程号是pid的进程，
    // 那么若该任务已经是回话首领，则出错返回。若该任务的会话ID与当前进程
    // 的不同，则也出错返回。否则设置进程的pgrp = pgid,并返回0.若没有找到
    // 指定pid的进程，则返回进程不存在出错码。
	for (i=0 ; i<NR_TASKS ; i++)
		if (task[i] && task[i]->pid==pid) {
			if (task[i]->leader)
				return -EPERM;
			if (task[i]->session != current->session)
				return -EPERM;
			task[i]->pgrp = pgid;
			return 0;
		}
	return -ESRCH;
}

// 返回当前进程的进程组号。与getpgid(0)等同。
int sys_getpgrp(void)
{
	return current->pgrp;
}

// 创建一个会话(session)(即设置其leader=1),并且设置其会话＝其组号＝其进程号。
// setsid - SET Session ID.
int sys_setsid(void)
{
    // 如果当前进程已是会话首领并且不是超级用户，则出错返回。否则设置当前进程为
    // 新会话首领(leader = 1),并且设置当前进程会话号session和组号pgrp都等于
    // 进程号pid，而且设置当前进程没有控制终端。最后系统调用返回会话号。
	if (current->leader && !suser())
		return -EPERM;
	current->leader = 1;
	current->session = current->pgrp = current->pid;
	current->tty = -1;              // 表示当前进程没有控制终端。
	return current->pgrp;
}

// 获取系统名称等信息。其中utsname结构包含5个字段，分别是：当前运行系统的名称，
// 网络节点名称(主机名)、当前操作系统发行级别、操作系统版本号以及系统运行的
// 硬件类型名称，该结构在include/sys/utsname.h中定义。
int sys_uname(struct utsname * name)
{
	static struct utsname thisname = {      // 这里给出了结构中的信息，这种编码肯定会改变。
		"linux .0","nodename","release ","version ","machine "
	};
	int i;

    // 首先判断参数的有效性。如果存放信息的缓冲区指针为空，则出错返回。在验证缓
    // 冲区大小是否超限(若超出内核自动扩展)。然后将utsname中的信息逐字节复制到
    // 用户缓冲区中。
	if (!name) return -ERROR;
	verify_area(name,sizeof *name);
	for(i=0;i<sizeof *name;i++)
		put_fs_byte(((char *) &thisname)[i],i+(char *) name);
	return 0;
}

// 设置当前进程创建文件属性屏蔽码为mask & 0777。并返回原屏蔽码。
int sys_umask(int mask)
{
	int old = current->umask;

	current->umask = mask & 0777;
	return (old);
}
