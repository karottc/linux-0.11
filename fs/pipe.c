/*
 *  linux/fs/pipe.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <signal.h>

#include <linux/sched.h>
#include <linux/mm.h>	/* for get_free_page */
#include <asm/segment.h>

//// 管道读操作函数
// 参数inode是管道对应的i节点，buf是用户数据缓冲区指针，count是读取的字节数。
int read_pipe(struct m_inode * inode, char * buf, int count)
{
	int chars, size, read = 0;

    // 如果需要读取的字节计数count大于0，我们就循环执行以下操作。在循环读操作
    // 过程中，若当前管道中没有数据（size=0），则唤醒等待该节点的进程，这通常
    // 是写管道进程。如果已没有写管道者，即i节点引用计数值小于2，则返回已读字
    // 节数退出。否则在该i节点上睡眠，等待信息。宏PIPE_SIZE定义在fs.h中。
	while (count>0) {
		while (!(size=PIPE_SIZE(*inode))) {
			wake_up(&inode->i_wait);
			if (inode->i_count != 2) /* are there any writers? */
				return read;
			sleep_on(&inode->i_wait);
		}
        // 此时说明管道(缓冲区)中有数据。于是我们取管道尾指针到缓冲区末端的字
        // 节数chars。如果其大于还需要读取的字节数count，则令其等于count。如果
        // chars大于当前管道中含有数据的长度size，则令其等于size。然后把需读字
        // 节数count减去此次可读的字节数chars，并累加已读字节数read.
		chars = PAGE_SIZE-PIPE_TAIL(*inode);
		if (chars > count)
			chars = count;
		if (chars > size)
			chars = size;
		count -= chars;
		read += chars;
        // 再令size指向管道尾指针处，并调整当前管道尾指针(前移chars字节)。若尾
        // 指针超过管道末端则绕回。然后将管道中的数据复制到用户缓冲区中。对于
        // 管道i节点，其i_size字段中是管道缓冲块指针。
		size = PIPE_TAIL(*inode);
		PIPE_TAIL(*inode) += chars;
		PIPE_TAIL(*inode) &= (PAGE_SIZE-1);
		while (chars-->0)
			put_fs_byte(((char *)inode->i_size)[size++],buf++);
	}
    // 当此次读管道操作结束，则唤醒等待该管道的进程，并返回读取的字节数。
	wake_up(&inode->i_wait);
	return read;
}

//// 管道写操作函数。
// 参数inode是管道对应的i节点，buf是数据缓冲区指针，count是将写入管道的字节数。
int write_pipe(struct m_inode * inode, char * buf, int count)
{
	int chars, size, written = 0;

    // 如果要写入的字节数count大于0，那么我们就循环执行以下操作。在循环操作过程
    // 中，若当前管道中没有已经满了(空闲空间size = 0),则唤醒等待该节点的进程，
    // 通常唤醒的是读管道进程。如果已没有读管道者，即i节点引用计数值小于2，则
    // 向当前进程发送SIGPIPE信号，并返回已写入的字节数退出；若写入0字节，则返回
    // -1.否则让当前进程在该i节点睡眠，以等待读管道进程读取数据，从而让管道腾出
    // 空间。宏PIPE_SIZE()、PIPE_HEAD()等定义在文件fs.h中。
	while (count>0) {
		while (!(size=(PAGE_SIZE-1)-PIPE_SIZE(*inode))) {
			wake_up(&inode->i_wait);
			if (inode->i_count != 2) { /* no readers */
				current->signal |= (1<<(SIGPIPE-1));
				return written?written:-1;
			}
			sleep_on(&inode->i_wait);
		}
        // 程序执行到这里表示管道缓冲区中有可写空间size.于是我们管道头指针到缓冲区
        // 末端空间字节数chars。写管道操作是从管道头指针处开始写的。如果chars大于还
        // 需要写入的字节数count，则令其等于count。如果chars大于当前管道中空闲空间
        // 长度size，则令其等于size，然后把需要写入字节数count减去此次可写入的字节数
        // chars，并把写入字节数累驾到witten中。
		chars = PAGE_SIZE-PIPE_HEAD(*inode);
		if (chars > count)
			chars = count;
		if (chars > size)
			chars = size;
		count -= chars;
		written += chars;
        // 再令size指向管道数据头指针处，并调整当前管道数据头部指针(前移chars字节)。
        // 若头指针超过管道末端则绕回。然后从用户缓冲区复制chars个字节到管道头指针
        // 开始处。对于管道i节点，其i_size字段中是管道缓冲块指针。
		size = PIPE_HEAD(*inode);
		PIPE_HEAD(*inode) += chars;
		PIPE_HEAD(*inode) &= (PAGE_SIZE-1);
		while (chars-->0)
			((char *)inode->i_size)[size++]=get_fs_byte(buf++);
	}
    // 当此次写管道操作结束，则唤醒等待管道的进程，返回已写入的字节数，退出。
	wake_up(&inode->i_wait);
	return written;
}

//// 创建管道系统调用。
// 在fildes所指的数组中创建一对文件句柄(描述符)。这对句柄指向一管道i节点。
// 参数：filedes - 文件句柄数组。fildes[0]用于读管道数据，fildes[1]向管道写入数据。
// 成功时返回0，出错时返回-1.
int sys_pipe(unsigned long * fildes)
{
	struct m_inode * inode;
	struct file * f[2];
	int fd[2];
	int i,j;

    // 首先从系统文件表中取两个空闲项(引用计数字段为0的项)，并分别设置引用计数为1。
    // 若只有1个空闲项，则释放该项(引用计数复位).若没有找到两个空闲项，则返回-1.
	j=0;
	for(i=0;j<2 && i<NR_FILE;i++)
		if (!file_table[i].f_count)
			(f[j++]=i+file_table)->f_count++;
	if (j==1)
		f[0]->f_count=0;
	if (j<2)
		return -1;
    // 针对上面取得的两个文件表结构项，分别分配一文件句柄号，并使用进程文件结构指针
    // 数组的两项分别指向这两个文件结构。而文件句柄即是该数组的索引号。类似的，如果
    // 只有一个空闲文件句柄，则释放该句柄(置空相应数组项)。如果没有找到两个空闲句柄，
    // 则释放上面获取的两个文件结构项(复位引用计数值)，并返回-1.
	j=0;
	for(i=0;j<2 && i<NR_OPEN;i++)
		if (!current->filp[i]) {
			current->filp[ fd[j]=i ] = f[j];
			j++;
		}
	if (j==1)
		current->filp[fd[0]]=NULL;
	if (j<2) {
		f[0]->f_count=f[1]->f_count=0;
		return -1;
	}
    // 然后利用函数get_pipe_inode()申请一个管道使用的i节点，并为管道分配一页内存作为
    // 缓冲区。如果不成功，则相应释放两个文件句柄和文件结构项，并返回-1.
	if (!(inode=get_pipe_inode())) {
		current->filp[fd[0]] =
			current->filp[fd[1]] = NULL;
		f[0]->f_count = f[1]->f_count = 0;
		return -1;
	}
    // 如果管道i节点申请成功，则对两个文件结构进行初始化操作，让他们都指向同一个管道
    // i节点，并把读写指针都置零。第1个文件结构的文件模式置为读，第2个文件结构的文件
    // 模式置为写。最后将文件句柄数组复制到对应的用户空间数组中，成功返回0，退出。
	f[0]->f_inode = f[1]->f_inode = inode;
	f[0]->f_pos = f[1]->f_pos = 0;
	f[0]->f_mode = 1;		/* read */
	f[1]->f_mode = 2;		/* write */
	put_fs_long(fd[0],0+fildes);
	put_fs_long(fd[1],1+fildes);
	return 0;
}
