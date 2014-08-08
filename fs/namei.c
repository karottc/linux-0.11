/*
 *  linux/fs/namei.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * Some corrections by tytso.
 */

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

#include <string.h> 
#include <fcntl.h>
#include <errno.h>
#include <const.h>
#include <sys/stat.h>

// 下面宏中右侧表达式是访问数组的一种特殊使用方法。它基于这样的一个事实，
// 即用数组名和数组下标所表示的数组项（例如a[b]）的值等同于使用数组首指针
// 加上该项偏移地址的形式的值*(a+b),同时可知项a[b]也可以表示成b[a]的形式。
// 因此对于字符数组项形式为“loveYou”[2](或者2["loveYou"])就等同于*("loveYou")+2
// 另外，字符串“loveYou”在内存中被存储的位置就是其地址，因此数组项“LoveYou”[2]
// 的值就是该字符串中索引值为2的字符'v'所对应的ASCII码值0x76，或用八进制表示
// 就是0166.在C语言中，字符也可以用ASCII码值来表示，方法是在字符的ASCII码值
// 前加一个反斜杠。例如字符“v”可以表示成“\x76”或者“\166”,对于不可显示的字符
// 就可用其ASCII来表示。
// 
// 下面是访问模式宏。x是头文件fentl.h中定义的访问标志。这个宏根据文件访问
// 标志x的值来索引双引号中对应的数值。双引号中有4个八进制数值（实际表示4个
// 控制字符），分别表示读、写、执行的权限为：r,w,rw。并且分别对应x的索引值
// 0-3.例如，如果x为2，则该宏返回把禁止呢006，表示可读写。另外，
// O_ACCMODE＝ 00003，是索引值x的屏蔽码。
#define ACC_MODE(x) ("\004\002\006\377"[(x)&O_ACCMODE])

/*
 * comment out this line if you want names > NAME_LEN chars to be
 * truncated. Else they will be disallowed.
 */
/* #define NO_TRUNCATE */

#define MAY_EXEC 1
#define MAY_WRITE 2
#define MAY_READ 4

/*
 *	permission()
 *
 * is used to check for read/write/execute permissions on a file.
 * I don't know if we should look at just the euid or both euid and
 * uid, but that should be easily changed.
 */
//// 检测文件访问权限
// 参数：inode - 文件的i节点指针；mask - 访问属性屏蔽码。
// 返回：访问许可返回1，否则返回0.
static int permission(struct m_inode * inode,int mask)
{
	int mode = inode->i_mode;

/* special case: not even root can read/write a deleted file */
    // 如果i节点有对应的设备，但该i节点的连接计数值等于0，表示该文件
    // 已被删除，则返回。否则，如果进程的有效用户ID(euid)与i节点的
    // 用户id相同，则取文件宿主的访问权限。否则如果与组id相同，
    // 则取租用户的访问权限。
	if (inode->i_dev && !inode->i_nlinks)
		return 0;
	else if (current->euid==inode->i_uid)
		mode >>= 6;
	else if (current->egid==inode->i_gid)
		mode >>= 3;
	if (((mode & mask & 0007) == mask) || suser())
		return 1;
	return 0;
}

/*
 * ok, we cannot use strncmp, as the name is not in our data space.
 * Thus we'll have to use match. No big problem. Match also makes
 * some sanity tests.
 *
 * NOTE! unlike strncmp, match returns 1 for success, 0 for failure.
 */
//// 指定长度字符串比较函数
// 参数：len - 比较的字符串长度；name - 文件名指针；de - 目录项结构
// 返回：相同返回1，不同返回0. 
// 下面函数中的寄存器变了same被保存在eax寄存器中，以便高效访问。
static int match(int len,const char * name,struct dir_entry * de)
{
	register int same ;

    // 首先判断函数参数的有效性。如果目录项指针空，或者目录项i节点等于0，或者
    // 要比较的字符串长度超过文件名长度，则返回0.如果要比较的长度len小于NAME_LEN，
    // 但是目录项中文件名长度超过len，也返回0.
	if (!de || !de->inode || len > NAME_LEN)
		return 0;
	if (len < NAME_LEN && de->name[len])
		return 0;
    // 然后使用嵌套汇编语句进行快速比较操作。他会在用户数据空间(fs段)执行字符串的比较
    // 操作。%0 - eax（比较结果same）；%1 - eax (eax初值0)；%2 - esi(名字指针)；
    // %3 - edi(目录项名指针)；%4 - ecx(比较的字节长度值len).
	__asm__("cld\n\t"
		"fs ; repe ; cmpsb\n\t"
		"setz %%al"
		:"=a" (same)
		:"0" (0),"S" ((long) name),"D" ((long) de->name),"c" (len)
		);
	return same;
}

/*
 *	find_entry()
 *
 * finds an entry in the specified directory with the wanted name. It
 * returns the cache buffer in which the entry was found, and the entry
 * itself (as a parameter - res_dir). It does NOT read the inode of the
 * entry - you'll have to do that yourself if you want to.
 *
 * This also takes care of the few special cases due to '..'-traversal
 * over a pseudo-root and a mount point.
 */
//// 查找指定目录和文件名的目录项。
// 参数：*dir - 指定目录i节点的指针；name - 文件名；namelen - 文件名长度；
// 该函数在指定目录的数据（文件）中搜索指定文件名的目录项。并对指定文件名
// 是'..'的情况根据当前进行的相关设置进行特殊处理。关于函数参数传递指针的指针
// 作用，请参见seched.c中的注释。
// 返回：成功则函数高速缓冲区指针，并在*res_dir处返回的目录项结构指针。失败则
// 返回空指针NULL。
static struct buffer_head * find_entry(struct m_inode ** dir,
	const char * name, int namelen, struct dir_entry ** res_dir)
{
	int entries;
	int block,i;
	struct buffer_head * bh;
	struct dir_entry * de;
	struct super_block * sb;

    // 同样，本函数一上来也需要对函数参数的有效性进行判断和验证。如果我们在前面
    // 定义了符号常数NO_TRUNCATE,那么如果文件名长度超过最大长度NAME_LEN，则不予
    // 处理。如果没有定义过NO_TRUNCATE，那么在文件名长度超过最大长度NAME_LEN时截短之。
#ifdef NO_TRUNCATE
	if (namelen > NAME_LEN)
		return NULL;
#else
	if (namelen > NAME_LEN)
		namelen = NAME_LEN;
#endif
    // 首先计算本目录中目录项项数entries。目录i节点i_size字段中含有本目录包含的数据
    // 长度，因此其除以一个目录项的长度（16字节）即课得到该目录中目录项数。然后置空
    // 返回目录项结构指针。如果长度等于0，则返回NULL，退出。
	entries = (*dir)->i_size / (sizeof (struct dir_entry));
	*res_dir = NULL;
	if (!namelen)
		return NULL;
    // 接下来我们对目录项文件名是'..'的情况进行特殊处理。如果当前进程指定的根i节点就是
    // 函数参数指定的目录，则说明对于本进程来说，这个目录就是它伪根目录，即进程只能访问
    // 该目录中的项而不能后退到其父目录中去。也即对于该进程本目录就如同是文件系统的根目录，
    // 因此我们需要将文件名修改为‘.’。
    // 否则，如果该目录的i节点号等于ROOT_INO（1号）的话，说明确实是文件系统的根i节点。
    // 则取文件系统的超级块。如果被安装到的i节点存在，则先放回原i节点，然后对被安装到
    // 的i节点进行处理。于是我们让*dir指向该被安装到的i节点；并且该i节点的引用数加1.
    // 即针对这种情况，我们悄悄的进行了“偷梁换柱”工程。:-)
/* check for '..', as we might have to do some "magic" for it */
	if (namelen==2 && get_fs_byte(name)=='.' && get_fs_byte(name+1)=='.') {
/* '..' in a pseudo-root results in a faked '.' (just change namelen) */
		if ((*dir) == current->root)
			namelen=1;
		else if ((*dir)->i_num == ROOT_INO) {
/* '..' over a mount-point results in 'dir' being exchanged for the mounted
   directory-inode. NOTE! We set mounted, so that we can iput the new dir */
			sb=get_super((*dir)->i_dev);
			if (sb->s_imount) {
				iput(*dir);
				(*dir)=sb->s_imount;
				(*dir)->i_count++;
			}
		}
	}
    // 现在我们开始正常操作，查找指定文件名的目录项在什么地方。因此我们需要读取目录的
    // 数据，即取出目录i节点对应块设备数据区中的数据块（逻辑块）信息。这些逻辑块的块号
    // 保存在i节点结构的i_zone[9]数组中。我们先取其中第一个块号。如果目录i节点指向的
    // 第一个直接磁盘块好为0，则说明该目录竟然不含数据，这不正常。于是返回NULL退出，
    // 否则我们就从节点所在设备读取指定的目录项数据块。当然，如果不成功，则也返回NULL 退出。
	if (!(block = (*dir)->i_zone[0]))
		return NULL;
	if (!(bh = bread((*dir)->i_dev,block)))
		return NULL;
    // 此时我们就在这个读取的目录i节点数据块中搜索匹配指定文件名的目录项。首先让de指向
    // 缓冲块中的数据块部分。并在不超过目录中目录项数的条件下，循环执行搜索。其中i是目录中
    // 的目录项索引号。在循环开始时初始化为0.
	i = 0;
	de = (struct dir_entry *) bh->b_data;
	while (i < entries) {
        // 如果当前目录项数据块已经搜索完，还没有找到匹配的目录项，则释放当前目录项数据块。
        // 再读入目录的下一个逻辑块。若这块为空。则只要还没有搜索完目录中的所有目录项，就
        // 跳过该块，继续读目录的下一逻辑块。若该块不空，就让de指向该数据块，然后在其中继续
        // 搜索。其中DIR_ENTRIES_PER_BLOCK可得到当前搜索的目录项所在目录文件中的块号，而bmap()
        // 函数则课计算出在设备上对应的逻辑块号.
		if ((char *)de >= BLOCK_SIZE+bh->b_data) {
			brelse(bh);
			bh = NULL;
			if (!(block = bmap(*dir,i/DIR_ENTRIES_PER_BLOCK)) ||
			    !(bh = bread((*dir)->i_dev,block))) {
				i += DIR_ENTRIES_PER_BLOCK;
				continue;
			}
			de = (struct dir_entry *) bh->b_data;
		}
        // 如果找到匹配的目录项的话，则返回该目录项结构指针de和该目录项i节点指针*dir以及该目录项
        // 数据块指针bh，并退出函数。否则继续在目录项数据块中比较下一个目录项。
		if (match(namelen,name,de)) {
			*res_dir = de;
			return bh;
		}
		de++;
		i++;
	}
    // 如果指定目录中的所有目录项都搜索完后，还没有找到相应的目录项，则释放目录的数据块，
    // 最后返回NULL（失败）。
	brelse(bh);
	return NULL;
}

/*
 *	add_entry()
 *
 * adds a file entry to the specified directory, using the same
 * semantics as find_entry(). It returns NULL if it failed.
 *
 * NOTE!! The inode part of 'de' is left at 0 - which means you
 * may not sleep between calling this and putting something into
 * the entry, as someone else might have used it while you slept.
 */
//// 根据指定的目录和文件名添加目录项
// 参数：dir - 指定目录的i节点；name - 文件名；namelen - 文件名长度；
// 返回：高速缓冲区指针；res_dir - 返回的目录项结构指针。
static struct buffer_head * add_entry(struct m_inode * dir,
	const char * name, int namelen, struct dir_entry ** res_dir)
{
	int block,i;
	struct buffer_head * bh;
	struct dir_entry * de;

    // 同样，本函数一上来也需要对函数参数的有效性进行判断和验证。
    // 如果我们在前面定义了符号常数NO_TRUNCATE，那么如果文件名长
    // 度超过最大长度NAME_LEN，则不予处理。如果没有定义过NO_TRUNCATE，
    // 那么在文件名长度超过最大长度NAME_LEN时截短之。
	*res_dir = NULL;
#ifdef NO_TRUNCATE
	if (namelen > NAME_LEN)
		return NULL;
#else
	if (namelen > NAME_LEN)
		namelen = NAME_LEN;
#endif
    // 现在我们开始操作，向指定目录中添加一个指定文件名的目录项。因此
    // 我们需要先读取目录的数据，即取出目录i节点对应块数据区中的数据块
    // 信息。这些逻辑块的块号保存在i节点结构i_zone[9]数组中。我们先取
    // 其中第1个块号，如果目录i节点指向的第一个直接磁盘块号为0，则说明
    // 该目录竟然不含数据，这不正常。于是返回NULL退出。否则我们就从节点
    // 所在设备读取指定目录项数据块。当然，如果不成功，则也返回NULL退出。
    // 如果参数提供的文件名长度等于0，则也返回NULL退出。
	if (!namelen)
		return NULL;
	if (!(block = dir->i_zone[0]))
		return NULL;
	if (!(bh = bread(dir->i_dev,block)))
		return NULL;
    // 此时我们就在这个目录i节点数据块中循环查找最后未使用的空目录项。
    // 首先让目录项结构指针de指向缓冲块中的数据块部分，即第一个目录项处。
    // 其中i是目录中的目录项索引号，在循环开始时初始化为0.
	i = 0;
	de = (struct dir_entry *) bh->b_data;
	while (1) {
        // 如果当前目录项数据块已经搜索完毕，但还没有找到需要的空目录项，
        // 则释放当前目录项数据块，再读入目录的下一个逻辑块。如果对应的逻辑块。
        // 如果对应的逻辑块不存在就创建一块。如果读取或创建操作失败则返回空。
        // 如果此次读取的磁盘逻辑块数据返回的缓冲块数据为空，说明这块逻辑块
        // 可能是因为不存在而新创建的空块，则把目录项索引值加上一块逻辑块所
        // 能容纳的目录项数DIR_ENTRIES_PER_BLOCK，用以跳过该块并继续搜索。
        // 否则说明新读入的块上有目录项数据，于是让目录项结构指针de指向该块
        // 的缓冲块数据部分，然后在其中继续搜索。其中i/DIR_ENTRIES_PER_BLOCK可
        // 计算得到当前搜索的目录项i所在目录文件中的块号，而create_block函数则可
        // 读取或创建出在设备上对应的逻辑块。
		if ((char *)de >= BLOCK_SIZE+bh->b_data) {
			brelse(bh);
			bh = NULL;
			block = create_block(dir,i/DIR_ENTRIES_PER_BLOCK);
			if (!block)
				return NULL;
			if (!(bh = bread(dir->i_dev,block))) {
				i += DIR_ENTRIES_PER_BLOCK;
				continue;
			}
			de = (struct dir_entry *) bh->b_data;
		}
        // 如果当前所操作的目录项序号i乘上目录结构大小所在长度值已经超过了该目录
        // i节点信息所指出的目录数据长度值i_size，则说明整个目录文件数据中没有
        // 由于删除文件留下的空目录项，因此我们只能把需要添加的新目录项附加到
        // 目录文件数据的末端处。于是对该处目录项进行设置（置该目录项的i节点指针
        // 为空），并更新该目录文件的长度值（加上一个目录项的长度），然后设置目录
        // 的i节点已修改标志，再更新该目录的改变时间为当前时间。
		if (i*sizeof(struct dir_entry) >= dir->i_size) {
			de->inode=0;
			dir->i_size = (i+1)*sizeof(struct dir_entry);
			dir->i_dirt = 1;
			dir->i_ctime = CURRENT_TIME;
		}
        // 若当前搜索的目录项de的i节点为空，则表示找到一个还未使用的空闲目录项
        // 或是添加的新目录项。于是更新目录的修改时间为当前时间，并从用户数据区
        // 复制文件名到该目录项的文件名字段，置含有本目录项的相应高速缓冲块已修改
        // 标志。返回该目录项的指针以及该高速缓冲块的指针，退出。
		if (!de->inode) {
			dir->i_mtime = CURRENT_TIME;
			for (i=0; i < NAME_LEN ; i++)
				de->name[i]=(i<namelen)?get_fs_byte(name+i):0;
			bh->b_dirt = 1;
			*res_dir = de;
			return bh;
		}
		de++;
		i++;
	}
    // 本函数执行不到这里。这也许是Linus在写这段代码时，先复制了上面的find_entry()
    // 函数的代码，而后修改成本函数的。:-)
	brelse(bh);
	return NULL;
}

/*
 *	get_dir()
 *
 * Getdir traverses the pathname until it hits the topmost directory.
 * It returns NULL on failure.
 */
//// 搜寻指定路径的目录（或文件名）的i节点。
// 参数：pathname - 路径名
// 返回：目录或文件的i节点指针。
static struct m_inode * get_dir(const char * pathname)
{
	char c;
	const char * thisname;
	struct m_inode * inode;
	struct buffer_head * bh;
	int namelen,inr,idev;
	struct dir_entry * de;

    // 搜索操作会从当前任务结构中设置的根（或伪根）i节点或当前工作目录i节点
    // 开始，因此首先需要判断进程的根i节点指针和当前工作目录i节点指针是否有效。
    // 如果当前进程没有设定根i节点，或者该进程根i节点指向是一个空闲i节点（引用为0），
    // 则系统出错停机。如果进程的当前工作目录i节点指针为空，或者该当前工作目录
    // 指向的i节点是一个空闲i节点，这也是系统有问题，停机。
	if (!current->root || !current->root->i_count)
		panic("No root inode");
	if (!current->pwd || !current->pwd->i_count)
		panic("No cwd inode");
    // 如果用户指定的路径名的第1个字符是'/'，则说明路径名是绝对路径名。则从
    // 根i节点开始操作，否则第一个字符是其他字符，则表示给定的相对路径名。
    // 应从进程的当前工作目录开始操作。则取进程当前工作目录的i节点。如果路径
    // 名为空，则出错返回NULL退出。此时变量inode指向了正确的i节点 -- 进程的
    // 根i节点或当前工作目录i节点之一。
	if ((c=get_fs_byte(pathname))=='/') {
		inode = current->root;
		pathname++;
	} else if (c)
		inode = current->pwd;
	else
		return NULL;	/* empty name is bad */
    // 然后针对路径名中的各个目录名部分和文件名进行循环出路，首先把得到的i节点
    // 引用计数增1，表示我们正在使用。在循环处理过程中，我们先要对当前正在处理
    // 的目录名部分（或文件名）的i节点进行有效性判断，并且把变量thisname指向
    // 当前正在处理的目录名部分（或文件名）。如果该i节点不是目录类型的i节点，
    // 或者没有可进入该目录的访问许可，则放回该i节点，并返回NULL退出。当然，刚
    // 进入循环时，当前的i节点就是进程根i节点或者是当前工作目录的i节点。
	inode->i_count++;
	while (1) {
		thisname = pathname;
		if (!S_ISDIR(inode->i_mode) || !permission(inode,MAY_EXEC)) {
			iput(inode);
			return NULL;
		}
        // 每次循环我们处理路径名中一个目录名（或文件名）部分。因此在每次循环中
        // 我们都要从路径名字符串中分离出一个目录名（或文件名）。方法是从当前路径名
        // 指针pathname开始处搜索检测字符，知道字符是一个结尾符（NULL）或者是一
        // 个'/'字符。此时变量namelen正好是当前处理目录名部分的长度，而变量thisname
        // 正指向该目录名部分的开始处。此时如果字符是结尾符NULL，则表明以及你敢搜索
        // 到路径名末尾，并已到达最后指定目录名或文件名，则返回该i节点指针退出。
        // 注意！如果路径名中最后一个名称也是一个目录名，但其后面没有加上'/'字符，
        // 则函数不会返回该最后目录的i节点！例如：对于路径名/usr/src/linux，该函数
        // 将只返回src/目录名的i节点。
		for(namelen=0;(c=get_fs_byte(pathname++))&&(c!='/');namelen++)
			/* nothing */ ;
		if (!c)
			return inode;
        // 在得到当前目录名部分（或文件名）后，我们调用查找目录项函数find_entry()在
        // 当前处理的目录中寻找指定名称的目录项。如果没有找到，则返回该i节点，并返回
        // NULL退出。然后在找到的目录项中取出其i节点号inr和设备号idev，释放包含该目录
        // 项的高速缓冲块并放回该i节点。然后去节点号inr的i节点inode，并以该目录项为
        // 当前目录继续循环处理路径名中的下一目录名部分（或文件名）。
		if (!(bh = find_entry(&inode,thisname,namelen,&de))) {
			iput(inode);
			return NULL;
		}
		inr = de->inode;                        // 当前目录名部分的i节点号
		idev = inode->i_dev;
		brelse(bh);
		iput(inode);
		if (!(inode = iget(idev,inr)))          // 取i节点内容。
			return NULL;
	}
}

/*
 *	dir_namei()
 *
 * dir_namei() returns the inode of the directory of the
 * specified name, and the name within that directory.
 */
// 参数：pathname - 目录路径名；namelen - 路径名长度；name - 返回的最顶层目录名。
// 返回：指定目录名最顶层目录的i节点指针和最顶层目录名称及长度。出错时返回NULL。
// 注意！！这里"最顶层目录"是指路径名中最靠近末端的目录。
static struct m_inode * dir_namei(const char * pathname,
	int * namelen, const char ** name)
{
	char c;
	const char * basename;
	struct m_inode * dir;

    // 首先取得指定路径名最顶层目录的i节点。然后对路径名Pathname 进行搜索检测，查出
    // 最后一个'/'字符后面的名字字符串，计算其长度，并且返回最顶层目录的i节点指针。
    // 注意！如果路径名最后一个字符是斜杠字符'/'，那么返回的目录名为空，并且长度为0.
    // 但返回的i节点指针仍然指向最后一个'/'字符钱目录名的i节点。
	if (!(dir = get_dir(pathname)))
		return NULL;
	basename = pathname;
	while ((c=get_fs_byte(pathname++)))
		if (c=='/')
			basename=pathname;
	*namelen = pathname-basename-1;
	*name = basename;
	return dir;
}

/*
 *	namei()
 *
 * is used by most simple commands to get the inode of a specified name.
 * Open, link etc use their own routines, but this is enough for things
 * like 'chmod' etc.
 */
//// 取指定路径名的i节点。
// 参数：pathname - 路径名。
// 返回：对应的i节点。
struct m_inode * namei(const char * pathname)
{
	const char * basename;
	int inr,dev,namelen;
	struct m_inode * dir;
	struct buffer_head * bh;
	struct dir_entry * de;

    // 首先查找指定路径的最顶层目录的目录名并得到其i节点，若不存在，则返回NULL退出。
    // 如果返回的最顶层名字长度是0，则表示该路径名以一个目录名为最后一项。因此我们
    // 已经找到对应目录的i节点，可以直接返回该i节点退出。
	if (!(dir = dir_namei(pathname,&namelen,&basename)))
		return NULL;
	if (!namelen)			/* special case: '/usr/' etc */
		return dir;
    // 然后在返回的顶层目录中寻找指定文件名目录项的i节点。注意！因为如果最后也是一个
    // 目录名，但其后没有加'/',则不会返回该目录的i节点！例如：/usr/src/linux,将只返回
    // src/目录名的i节点。因为函数dir_namei()把不以'/'结束的最后一个名字当作一个文件名
    // 来看待，所以这里需要单独对这种情况使用寻找目录项i节点函数find_entry()进行处理。
    // 此时de中含有寻找到的目录项指针，而dir是包含该目录项的目录的i节点指针。
	bh = find_entry(&dir,basename,namelen,&de);
	if (!bh) {
		iput(dir);
		return NULL;
	}
    // 接着取该目录项的i节点号和设备号，并释放包含该目录项的高速缓冲块并返回目录i节点。
    // 然后取对应节点号的i节点，修改其被访问时间为当前时间，并置已修改标志。最后返回
    // 该i节点指针。
	inr = de->inode;
	dev = dir->i_dev;
	brelse(bh);
	iput(dir);
	dir=iget(dev,inr);
	if (dir) {
		dir->i_atime=CURRENT_TIME;
		dir->i_dirt=1;
	}
	return dir;
}

/*
 *	open_namei()
 *
 * namei for open - this is in fact almost the whole open-routine.
 */
//// 文件打开namei函数。
// 参数filename是文件名，flag是打开文件标志，他可取值：O_RDONLY(只读)、O_WRONLY(只写)
// 或O_RDWR(读写)，以及O_CREAT(创建)、O_EXCL(被创建文件必须不存在)、O_APPEND(在文件尾
// 添加数据)等其他一些标志的组合。如果本调用创建了一个新文件，则mode就用于指定文件的
// 许可属性。这些属性有S_IRWXU(文件宿主具有读、写和执行权限)、S_IRUSR(用户具有读文件
// 权限)、S_IRWXG(组成员具有读、写和执行权限)等等。对于新创建的文件，这些属性只应用于
// 将来对文件的访问，创建了只读文件的打开调用也将返回一个可读写的文件句柄。
// 返回：成功返回0，否则返回出错码；res_inode - 返回对应文件路径名的i节点指针。
int open_namei(const char * pathname, int flag, int mode,
	struct m_inode ** res_inode)
{
	const char * basename;
	int inr,dev,namelen;
	struct m_inode * dir, *inode;
	struct buffer_head * bh;
	struct dir_entry * de;

    // 首先对函数参数进行合理的处理。如果文件访问模式标志是只读(0)，但是文件截零标志
    // O_TRUNC却置位了，则在文件打开标志中添加只写O_WRONLY。这样做的原因是由于截零标志
    // O_TRUNC必须在文件可写情况下才有效。然后使用当前进程的文件访问许可屏蔽码，屏蔽掉
    // 给定模式中的相应位，并添上对普通文件标志I_REGULAR。该标志将用于打开的文件不存在
    // 而需要创建文件时，作为新文件的默认属性。
	if ((flag & O_TRUNC) && !(flag & O_ACCMODE))
		flag |= O_WRONLY;
	mode &= 0777 & ~current->umask;
	mode |= I_REGULAR;
    // 然后根据指定的路径名寻找对应的i节点，以及最顶端目录名及其长度。此时如果最顶端目录
    // 名长度为0（例如'/usr/'这种路径名的情况），那么若操作不是读写、创建和文件长度截0，
    // 则表示是在打开一个目录名文件操作。于是直接返回该目录的i节点并返回0退出。否则说明
    // 进程操作非法，于是放回该i节点，返回出错码。
	if (!(dir = dir_namei(pathname,&namelen,&basename)))
		return -ENOENT;
	if (!namelen) {			/* special case: '/usr/' etc */
		if (!(flag & (O_ACCMODE|O_CREAT|O_TRUNC))) {
			*res_inode=dir;
			return 0;
		}
		iput(dir);
		return -EISDIR;
	}
    // 接着根据上面得到的最顶层目录名的i节点dir，在其中查找取得路径名字符串中最后的文件名
    // 对应的目录项结构de，并同时得到该目录项所在的高速缓冲区指针。如果该高速缓冲指针为NULL，
    // 则表示没有找到对应文件名的目录项，因此只可能是创建文件操作。此时如果不是创建文件，则
    // 放回该目录的i节点，返回出错号退出。如果用户在该目录没有写的权力，则放回该目录的i节点，
    // 返回出错号退出。
	bh = find_entry(&dir,basename,namelen,&de);
	if (!bh) {
		if (!(flag & O_CREAT)) {
			iput(dir);
			return -ENOENT;
		}
		if (!permission(dir,MAY_WRITE)) {
			iput(dir);
			return -EACCES;
		}
        // 现在我们确定了是创建操作并且有写操作许可。因此我们就在目录i节点对设备上申请一个
        // 新的i节点给路径名上指定的文件使用。若失败则放回目录的i节点，并返回没有空间出错码。
        // 否则使用该新i节点，对其进行初始设置：置节点的用户id；对应节点访问模式；置已修改
        // 标志。然后并在指定目录dir中添加一个新目录项。
		inode = new_inode(dir->i_dev);
		if (!inode) {
			iput(dir);
			return -ENOSPC;
		}
		inode->i_uid = current->euid;
		inode->i_mode = mode;
		inode->i_dirt = 1;
		bh = add_entry(dir,basename,namelen,&de);
        // 如果返回的应该含有新目录项的高速缓冲区指针为NULL，则表示添加目录项操作失败。于是
        // 将该新i节点的引用计数减1，放回该i节点与目录的i节点并返回出错码退出。否则说明添加
        // 目录项操作成功。于是我们来设置该新目录的一些初始值：置i节点号为新申请的i节点的号
        // 码；并置高速缓冲区已修改标志。然后释放该高速缓冲区，放回目录的i节点。返回新目录
        // 项的i节点指针，并成功退出。
		if (!bh) {
			inode->i_nlinks--;
			iput(inode);
			iput(dir);
			return -ENOSPC;
		}
		de->inode = inode->i_num;
		bh->b_dirt = 1;
		brelse(bh);
		iput(dir);
		*res_inode = inode;
		return 0;
	}
    // 若上面在目录中取文件名对应目录项结构的操作成功（即bh不为NULL），则说明指定打开的文件已
    // 经存在。于是取出该目录项的i节点号和其所在设备号，并释放该高速缓冲区以及放回目录的i节点
    // 如果此时堵在操作标志O_EXCL置位，但现在文件已经存在，则返回文件已存在出错码退出。
	inr = de->inode;
	dev = dir->i_dev;
	brelse(bh);
	iput(dir);
	if (flag & O_EXCL)
		return -EEXIST;
    // 然后我们读取该目录项的i节点内容。若该i节点是一个目录i节点并且访问模式是只写或读写，或者
    // 没有访问的许可权限，则放回该i节点，返回访问权限出错码退出。
	if (!(inode=iget(dev,inr)))
		return -EACCES;
	if ((S_ISDIR(inode->i_mode) && (flag & O_ACCMODE)) ||
	    !permission(inode,ACC_MODE(flag))) {
		iput(inode);
		return -EPERM;
	}
    // 接着我们更新该i节点的访问时间字段值为当前时间。如果设立了截0标志，则将该i节点的文件长度
    // 截0.最后返回该目录项i节点的指针，并返回0（成功）。
	inode->i_atime = CURRENT_TIME;
	if (flag & O_TRUNC)
		truncate(inode);
	*res_inode = inode;
	return 0;
}

//// 创建一个设备特殊文件或普通文件节点(node)
// 该函数创建名称为filename,由mode和dev指定的文件系统节点（普通文件、设备特殊文件或命名管道）
// 参数：filename - 路径名；mode - 指定使用许可以及所创建节点的类型；dev - 设备号。
// 返回：成功则返回0，否则返回出错码。
int sys_mknod(const char * filename, int mode, int dev)
{
	const char * basename;
	int namelen;
	struct m_inode * dir, * inode;
	struct buffer_head * bh;
	struct dir_entry * de;

    // 首先检查操作许可和参数的有效性并取路径名中顶层目录的i节点。如果不是超级用户，则返回
    // 访问许可出错码。如果找不到对应路径名中顶层目录的i节点，则返回出错码。如果最顶端的
    // 文件名长度为0，则说明给出的路径名最后没有指定文件名，放回该目录i节点，返回出错码退出。
    // 如果在该目录中没有写的权限，则放回该目录的i节点，返回访问许可出错码退出。如果不是超级
    // 用户，则返回访问许可出错码。
	if (!suser())
		return -EPERM;
	if (!(dir = dir_namei(filename,&namelen,&basename)))
		return -ENOENT;
	if (!namelen) {
		iput(dir);
		return -ENOENT;
	}
	if (!permission(dir,MAY_WRITE)) {
		iput(dir);
		return -EPERM;
	}
    // 然后我们搜索一下路径名指定的文件是否已经存在。若已经存在则不能创建同名文件节点。
    // 如果对应路径名上最后的文件名的目录项已经存在，则释放包含该目录项的缓冲区块并放回
    // 目录的i节点，放回文件已存在的出错码退出。
	bh = find_entry(&dir,basename,namelen,&de);
	if (bh) {
		brelse(bh);
		iput(dir);
		return -EEXIST;
	}
    // 否则我们就申请一个新的i节点，并设置该i节点的属性模式。如果要创建的是块设备文件或者是
    // 字符设备文件，则令i节点的直接逻辑块指针0等于设备号。即对于设备文件来说，其i节点的
    // i_zone[0]中存放的是该设备文件所定义设备的设备号。然后设置该i节点的修改时间、访问
    // 时间为当前时间，并设置i节点已修改标志。
	inode = new_inode(dir->i_dev);
	if (!inode) {
		iput(dir);
		return -ENOSPC;
	}
	inode->i_mode = mode;
	if (S_ISBLK(mode) || S_ISCHR(mode))
		inode->i_zone[0] = dev;
	inode->i_mtime = inode->i_atime = CURRENT_TIME;
	inode->i_dirt = 1;
    // 接着为这个新的i节点在目录中新添加一个目录项。如果失败（包含该目录项的高速缓冲块指针为
    // NULL），则放回目录的i节点，吧所申请的i节点引用连接计数复位，并放回该i节点，返回出错码退出。
	bh = add_entry(dir,basename,namelen,&de);
	if (!bh) {
		iput(dir);
		inode->i_nlinks=0;
		iput(inode);
		return -ENOSPC;
	}
    // 现在添加目录项操作也成功了，于是我们来设置这个目录项内容。令该目录项的i节点字段于新i节点
    // 号，并置高速缓冲区已修改标志，放回目录和新的i节点，释放高速缓冲区，最后返回0（成功）。
	de->inode = inode->i_num;
	bh->b_dirt = 1;
	iput(dir);
	iput(inode);
	brelse(bh);
	return 0;
}

//// 创建一个目录
// 参数：pathname - 路径名；mode - 目录使用的权限属性。
// 返回：成功则返回0，否则返回出错码。
int sys_mkdir(const char * pathname, int mode)
{
	const char * basename;
	int namelen;
	struct m_inode * dir, * inode;
	struct buffer_head * bh, *dir_block;
	struct dir_entry * de;

    // 首先检查操作许可和参数的有效性并取路径名中顶层目录的i节点。如果不是超级用户，则
    // 放回访问许可出错码。如果找不到对应路径名中顶层目录的i节点，则返回出错码。如果最
    // 顶端的文件名长度为0，则说明给出的路径名最后没有指定文件名，放回该目录i节点，返回
    // 出错码退出。如果在该目录中没有写权限，则放回该目录的i节点，返回访问许可出错码退出。
    // 如果不是超级用户，则返回访问许可出错码。
	if (!suser())
		return -EPERM;
	if (!(dir = dir_namei(pathname,&namelen,&basename)))
		return -ENOENT;
	if (!namelen) {
		iput(dir);
		return -ENOENT;
	}
	if (!permission(dir,MAY_WRITE)) {
		iput(dir);
		return -EPERM;
	}
    // 然后我们搜索一下路径名指定的目录名是否已经存在。若已经存在则不能创建同名目录节点。
    // 如果对应路径名上最后的目录名的目录项已经存在，则释放包含该目录项的缓冲区块并放回
    // 目录的i节点，返回文件已经存在的出错码退出。否则我们就申请一个新的i节点，并设置该i
    // 节点的属性模式：置该新i节点对应的文件长度为32字节（2个目录项的大小），置节点已修改
    // 标志，以及节点的修改时间和访问时间，2个目录项分别用于‘.’和'..'目录。
	bh = find_entry(&dir,basename,namelen,&de);
	if (bh) {
		brelse(bh);
		iput(dir);
		return -EEXIST;
	}
	inode = new_inode(dir->i_dev);
	if (!inode) {
		iput(dir);
		return -ENOSPC;
	}
	inode->i_size = 32;
	inode->i_dirt = 1;
	inode->i_mtime = inode->i_atime = CURRENT_TIME;
    // 接着为该新i节点申请一用于保存目录项数据的磁盘块，用于保存目录项结构信息。并令i节
    // 点的第一个直接块指针等于该块号。如果申请失败则放回对应目录的i节点；复位新申请的i
    // 节点连接计数；放回该新的i节点，返回没有空间出错码退出。否则置该新的i节点已修改标志。
	if (!(inode->i_zone[0]=new_block(inode->i_dev))) {
		iput(dir);
		inode->i_nlinks--;
		iput(inode);
		return -ENOSPC;
	}
	inode->i_dirt = 1;
    // 从设备上读取新申请的磁盘块（目的是吧对应块放到高速缓冲区中）。若出错，则放回对应
    // 目录的i节点；释放申请的磁盘块；复位新申请的i节点连接计数；放回该新的i节点，返回没有
    // 空间出错码退出。
	if (!(dir_block=bread(inode->i_dev,inode->i_zone[0]))) {
		iput(dir);
		free_block(inode->i_dev,inode->i_zone[0]);
		inode->i_nlinks--;
		iput(inode);
		return -ERROR;
	}
    // 然后我们在缓冲块中建立起所创建目录文件中的2个默认的新目录项('.'和'..')结构数据。
    // 首先令de指向存放目录项的数据块，然后置该目录项的i节点号字段等于新申请的i节点号，
    // 名字字段等于'.'。然后de指向下一个目录项结构，并在该结构中存放上级目录的i节点号
    // 和名字'..'。然后设置该高速缓冲块 已修改标志，并释放该缓冲块。再初始化设置新i节点
    // 的模式字段，并置该i节点已修改标志。
	de = (struct dir_entry *) dir_block->b_data;
	de->inode=inode->i_num;
	strcpy(de->name,".");
	de++;
	de->inode = dir->i_num;
	strcpy(de->name,"..");
	inode->i_nlinks = 2;
	dir_block->b_dirt = 1;
	brelse(dir_block);
	inode->i_mode = I_DIRECTORY | (mode & 0777 & ~current->umask);
	inode->i_dirt = 1;
    // 现在我们在指定目录中新添加一个目录项，用于存放新建目录的i节点号和目录名。如果
    // 失败(包含该目录项的高速缓冲区指针为NULL)，则放回目录的i节点；所申请的i节点引用
    // 连接计数复位，并放回该i节点。返回出错码退出。
	bh = add_entry(dir,basename,namelen,&de);
	if (!bh) {
		iput(dir);
		free_block(inode->i_dev,inode->i_zone[0]);
		inode->i_nlinks=0;
		iput(inode);
		return -ENOSPC;
	}
    // 最后令该新目录项的i节点字段等于新i节点号，并置高速缓冲块已修改标志，放回目录和
    // 新的i节点，是否高速缓冲块，最后返回0(成功).
	de->inode = inode->i_num;
	bh->b_dirt = 1;
	dir->i_nlinks++;
	dir->i_dirt = 1;
	iput(dir);
	iput(inode);
	brelse(bh);
	return 0;
}

/*
 * routine to check that the specified directory is empty (for rmdir)
 */
//// 检查指定目录是否空。
// 参数：inode - 指定目录的i节点指针。
// 返回：1 - 目录中是空的；0 - 不空。
static int empty_dir(struct m_inode * inode)
{
	int nr,block;
	int len;
	struct buffer_head * bh;
	struct dir_entry * de;

    // 首先计算指定目录中现有目录项个数并检查开始两个特定目录项中信息是否正确。
    // 一个目录中应该起码有2个目录项：即'.'和'..'。如果目录项个数少于2个或该目录
    // i节点的第1个直接块没有指向任何磁盘块号，或者该直接块读不出，则显示警告信
    // 息“设备dev上目录错”，返回0.
	len = inode->i_size / sizeof (struct dir_entry);
	if (len<2 || !inode->i_zone[0] ||
	    !(bh=bread(inode->i_dev,inode->i_zone[0]))) {
	    	printk("warning - bad directory on dev %04x\n",inode->i_dev);
		return 0;
	}
    // 此时bh所指缓冲块中含有目录项数据。我们让目录项指针de指向缓冲块中第1个目录项。
    // 对于第1个目录项（‘.’），它的i节点号字段inode应该等于当前目录的i节点号。对于
    // 第2个目录项（‘..’），它的i节点号字段inode应该等于上一层目录的i节点号，不会
    // 为0.因此如果第1个目录项的i节点号字段不等于该目录的i节点号，或者第2个目录项的
    // i节点号字段为零，或者两个目录项的名字字段不分别等于'.'和'..'，则显示出错警告
    // 信息“设备dev上目录错”，返回0.
	de = (struct dir_entry *) bh->b_data;
	if (de[0].inode != inode->i_num || !de[1].inode || 
	    strcmp(".",de[0].name) || strcmp("..",de[1].name)) {
	    	printk("warning - bad directory on dev %04x\n",inode->i_dev);
		return 0;
	}
    // 然后我们令nr等于目录项序号（从0开始）；de指向第三个目录项。并循环检测该目录项
    // 中其余所有的（len-2）个目录项，看有没有目录项的i节点号字段不为0（被使用）。
	nr = 2;
	de += 2;
	while (nr<len) {
        // 如果该块磁盘块中目录项已经全部检测完毕，则释放该磁盘块的缓冲块，并读取目录
        // 数据文件中下一块含有目录项的磁盘块。读取的方法是根据当前检测的目录项序号nr
        // 计算出对应目录项在目录数据文件中的数据块号（nr/DIR_ENTRIES_PER_BLOCK）,然后
        // 使用bmap()函数取得对应的盘块号block，再使用读设备盘块函数bread()把相应盘块
        // 读入缓冲块中，并返回该缓冲块指针。若所读取的相应盘块没有使用（或已经不用，如
        // 文件已经删除等），则继续读下一块，若读不出，则出错返回0，否则让de指向读出块
        // 的首个目录项。
		if ((void *) de >= (void *) (bh->b_data+BLOCK_SIZE)) {
			brelse(bh);
			block=bmap(inode,nr/DIR_ENTRIES_PER_BLOCK);
			if (!block) {
				nr += DIR_ENTRIES_PER_BLOCK;
				continue;
			}
			if (!(bh=bread(inode->i_dev,block)))
				return 0;
			de = (struct dir_entry *) bh->b_data;
		}
        // 对于de指向的当前目录项，如果该目录项的i节点号字段不等于0，则表示该目录项目前正
        // 被使用，则释放该高速缓冲区，返回0退出。否则，若还没有查询完该目录中的所有目录项，
        // 则把目录项序号nr增1，de指向下一个目录项，继续检测。
		if (de->inode) {
			brelse(bh);
			return 0;
		}
		de++;
		nr++;
	}
	brelse(bh);
	return 1;
}

//// 删除目录
// 参数：name - 目录名（路径名）
// 返回：返回0表示成功，否则返回出错号。
int sys_rmdir(const char * name)
{
	const char * basename;
	int namelen;
	struct m_inode * dir, * inode;
	struct buffer_head * bh;
	struct dir_entry * de;

    // 首先检查操作许可和参数的有效性并取路径名中顶层目录的i节点。如果不是
    // 超级用户，则返回访问许可出错码。如果找到对应路径名中顶层目录的i节点，
    // 则返回出错码。如果最顶端的文件名长度为0，则说明给出的路径名最后没有
    // 指定目录名，放回该目录i节点，返回出错码退出。如果在该目录中哦没有写
    // 权限，则放回该目录的i节点，返回访问许可出错码退出。如果不是超级用户，
    // 则返回访问许可出错码。
	if (!suser())
		return -EPERM;
	if (!(dir = dir_namei(name,&namelen,&basename)))
		return -ENOENT;
	if (!namelen) {
		iput(dir);
		return -ENOENT;
	}
	if (!permission(dir,MAY_WRITE)) {
		iput(dir);
		return -EPERM;
	}
    // 然后根据指定目录的i节点和目录名利用函数find_entry()寻找对应目录项，并返回
    // 包含该目录项的缓冲块指针bh、包含该目录项的目录的i节点指针dir和该目录项指针
    // de。再根据该目录项de中的i节点号利用iget()函数得到对应的i节点inode。如果对应
    // 路径名上最后目录名的目录项不存在，则释放包含该目录项的高速缓冲区，放回目录的
    // i节点，返回文件已经存在出错码，并退出。如果取目录项的i节点出错，则放回目录的
    // i节点，并释放含有目录项的高速缓冲区，返回出错号。
	bh = find_entry(&dir,basename,namelen,&de);
	if (!bh) {
		iput(dir);
		return -ENOENT;
	}
	if (!(inode = iget(dir->i_dev, de->inode))) {
		iput(dir);
		brelse(bh);
		return -EPERM;
	}
    // 此时我们已有包含要被删除目录项的目录i节点dir、要被删除目录项的i节点inode和要被
    // 删除目录项指针de。下面我们通过对3个对象中信息的检查来验证删除操作的可行性。
    // 若该目录设置了受限删除标志并且进程的有效用户ID（euid）不是root,并且进程的有效
    // 用户id（euid）不等于该i节点的用户id，则表示当前进程没有权限删除该目录，于是放回
    // 包含要删除目录名的目录i节点和该要删除目录的i节点，然后释放高速缓冲区，放回出错码。
	if ((dir->i_mode & S_ISVTX) && current->euid &&
	    inode->i_uid != current->euid) {
		iput(dir);
		iput(inode);
		brelse(bh);
		return -EPERM;
	}
    // 如果要被删除的目录项i节点的设备号不等于包含该目录项的目录的设备号，或者该被删除目录
    // 的引用连接计数1(表示有符号链接等)，则不能删除该目录。于是释放包含要删除目录名的目录
    // i节点和该要删除目录的i节点，释放高速缓冲块，返回出错码。
	if (inode->i_dev != dir->i_dev || inode->i_count>1) {
		iput(dir);
		iput(inode);
		brelse(bh);
		return -EPERM;
	}
    // 如果要被删除目录的目录项i节点就等于包含该需删除目录的目录i节点，则表示试图删除'.'目录，
    // 这是不允许的。于是放回包含要删除目录名的目录i节点和要删除目录的i节点。释放高速缓冲块，
    // 放回出错码。
	if (inode == dir) {	/* we may not delete ".", but "../dir" is ok */
		iput(inode);
		iput(dir);
		brelse(bh);
		return -EPERM;
	}
    // 若要被删除目录i节点的属性表明这不是一个目录，则本删除操作的前提完全不存在。于是放回
    // 包含删除目录名的目录i节点和该要删除目录的i节点，释放高速缓冲块，放回出错码。
	if (!S_ISDIR(inode->i_mode)) {
		iput(inode);
		iput(dir);
		brelse(bh);
		return -ENOTDIR;
	}
    // 若该需要被删除的目录不空，则也不能删除。于是放回包含要删除目录名的目录i节点和该要删除
    // 目录的i节点，释放高速缓冲块，返回出错码。
	if (!empty_dir(inode)) {
		iput(inode);
		iput(dir);
		brelse(bh);
		return -ENOTEMPTY;
	}
    // 对于一个空目录，其目录项链接数应该为2(连接到上层目录和本目录)。若该需被删除目录的i节点和
    // 链接数不等于2，则显示警告信息。但删除操作仍然继续执行。于是置该需被删除目录的目录项的i节点
    // 号字段为0，表示该目录项不再使用，并置含有该目录项的高速缓冲块已修改标志，并是否该缓冲块。
    // 然后在置被删除目录i节点的连接 数为0(表示空闲)，并置i节点已修改标志。
	if (inode->i_nlinks != 2)
		printk("empty directory has nlink!=2 (%d)",inode->i_nlinks);
	de->inode = 0;
	bh->b_dirt = 1;
	brelse(bh);
	inode->i_nlinks=0;
	inode->i_dirt=1;
    // 再将包含被删除目录名的目录的i节点连接计数减一，修改其改变时间和修改时间为当前时间，并置该
    // 节点已修改标志。最后放回包含要删除目录名的目录i节点和该要修改目录的i节点，返回0（表示删除成功）。
	dir->i_nlinks--;
	dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	dir->i_dirt=1;
	iput(dir);
	iput(inode);
	return 0;
}

//// 删除（释放）文件名对应的目录项
// 从文件系统删除一个名字。如果是文件的最后一个连接，并且没有进程正打开该文件，则该文件也将被
// 删除，并释放所占用的设备空间。
// 参数：name - 文件名（路径名）
// 返回：成功则返回0，否则返回出错号。
int sys_unlink(const char * name)
{
	const char * basename;
	int namelen;
	struct m_inode * dir, * inode;
	struct buffer_head * bh;
	struct dir_entry * de;

    // 首先检查参数的有效性并取路径名中顶层目录的i节点。如果找不到对应路径名中顶层
    // 目录的i节点，则返回出错码.如果最顶端的文件名长度为0，则说明给出的路径名最后
    // 没有指定文件名，放回该目录i节点，返回出错码退出。如果在该目录中没有写权限，则
    // 放回该目录的i节点，返回访问许可出错码退出。如果找不到对应路径名顶层目录的i节点，
    // 则返回出错码。
	if (!(dir = dir_namei(name,&namelen,&basename)))
		return -ENOENT;
	if (!namelen) {
		iput(dir);
		return -ENOENT;
	}
	if (!permission(dir,MAY_WRITE)) {
		iput(dir);
		return -EPERM;
	}
    // 然后根据指定目录的i节点和目录名利用函数find_entry()寻找对应目录项，
    // 并返回包含该目录项的缓冲块指针bh、包含该目录项的目录的i节点指针dir和该目录项
    // 指针de。再根据该目录项de中的i节点号利用iget()函数得到对应的i节点inode。如果对
    // 应路径名上最后目录名的目录项不存在，则释放包含该目录项的高速缓冲区，放回目录的
    // i节点，返回文件已经存在出错码，并退出。如果取自目录项的i节点出错，则放回目录的
    // i节点，并释放含有目录项的高速缓冲区，返回出错号。
	bh = find_entry(&dir,basename,namelen,&de);
	if (!bh) {
		iput(dir);
		return -ENOENT;
	}
	if (!(inode = iget(dir->i_dev, de->inode))) {
		iput(dir);
		brelse(bh);
		return -ENOENT;
	}
    // 此时我们已有包含要被删除目录项的目录i节点dir、要被删除目录项的i节点inode和要被
    // 删除目录项指针de。下面我们通过对这3个对象中信息的检查来验证删除操作的可行性。
    // 若该目录设置了受限删除标志并且进程的有效用户id(euid)不是root，并且进程的euid
    // 不等于该i节点的用户id，并且进程的euid也不等于目录i节点的用户id，则表示当前进程
    // 没有权限删除该目录，于是放回包含要删除目录名的目录i节点和该要删除目录的i节点。
    // 然后释放高速缓冲块，返回出错码。
	if ((dir->i_mode & S_ISVTX) && !suser() &&
	    current->euid != inode->i_uid &&
	    current->euid != dir->i_uid) {
		iput(dir);
		iput(inode);
		brelse(bh);
		return -EPERM;
	}
    // 如果该指定文件名是一个目录，则也不能删除。放回该目录i节点和该文件名目录项的i节
    // 点，释放包含该目录项的缓冲块，返回出错号。
	if (S_ISDIR(inode->i_mode)) {
		iput(inode);
		iput(dir);
		brelse(bh);
		return -EPERM;
	}
    // 如果该i节点的连接计数值已经为0，则显示警告信息，并修正为1.
	if (!inode->i_nlinks) {
		printk("Deleting nonexistent file (%04x:%d), %d\n",
			inode->i_dev,inode->i_num,inode->i_nlinks);
		inode->i_nlinks=1;
	}
    // 现在我们可以删除文件名对应的目录项了，于是将该文件名目录项中的i节点号字段置为0，
    // 表示释放该目录项，并设置包含该目录项的缓冲块已修改标志，释放该高速缓冲块。
	de->inode = 0;
	bh->b_dirt = 1;
	brelse(bh);
    // 然后把文件名对应i节点的链接数减1，置已修改标志，更新改变时间为当前时间。最后放回
    // 该i节点和目录的i节点，返回0(成功)。如果是文件的最后一个链接，即i节点链接数减1后等
    // 于0，并且此时没有进程正打开该文件，那么在调用iput()放回i节点时，该文件也将被删除，
    // 并释放所占用的设备空间。
	inode->i_nlinks--;
	inode->i_dirt = 1;
	inode->i_ctime = CURRENT_TIME;
	iput(inode);
	iput(dir);
	return 0;
}

//// 为文件建立一个文件名目录项
// 为一个已存在的文件创建一个新链接(也称为硬链接 - hard link)
// 参数：oldname - 原路径名；newname - 新的路径名
// 返回：若成功则返回0，否则返回出错号。
int sys_link(const char * oldname, const char * newname)
{
	struct dir_entry * de;
	struct m_inode * oldinode, * dir;
	struct buffer_head * bh;
	const char * basename;
	int namelen;

    // 首先对原文件名进行有效性验证，它应该存在并且不是一个目录名。所以我们先取得原文件
    // 路径名对应的i节点oldname.若果为0，则表示出错，返回出错号。若果原路径名对应的是
    // 一个目录名，则放回该i节点，也返回出错号。
	oldinode=namei(oldname);
	if (!oldinode)
		return -ENOENT;
	if (S_ISDIR(oldinode->i_mode)) {
		iput(oldinode);
		return -EPERM;
	}
    // 然后查找新路径名的最顶层目录的i节点dir，并返回最后的文件名及其长度。如果目录的
    // i节点没有找到，则放回原路径名的i节点，返回出错号。如果新路径名中不包括文件名，
    // 则放回原路径名i节点和新路径名目录的i节点，返回出错号。
	dir = dir_namei(newname,&namelen,&basename);
	if (!dir) {
		iput(oldinode);
		return -EACCES;
	}
	if (!namelen) {
		iput(oldinode);
		iput(dir);
		return -EPERM;
	}
    // 我们不能跨设备建立硬链接。因此如果新路径名顶层目录的设备号与原路径名的设备号不
    // 一样，则放回新路径名目录的i节点和原路径名的i节点，返回出错号。另外，如果用户没
    // 有在新目录中写的权限，则也不能建立连接，于是放回新路径名目录的i节点和原路径名
    // 的i节点，返回出错号。
	if (dir->i_dev != oldinode->i_dev) {
		iput(dir);
		iput(oldinode);
		return -EXDEV;
	}
	if (!permission(dir,MAY_WRITE)) {
		iput(dir);
		iput(oldinode);
		return -EACCES;
	}
    // 现在查询该新路径名是否已经存在，如果存在则也不能建立链接。于是释放包含该已存在
    // 目录项的高速缓冲块，放回新路径名目录的i节点和原路径名的i节点，返回出错号。
	bh = find_entry(&dir,basename,namelen,&de);
	if (bh) {
		brelse(bh);
		iput(dir);
		iput(oldinode);
		return -EEXIST;
	}
    // 现在所有条件都满足了，于是我们在新目录中添加一个目录项。若失败则放回该目录的
    // i节点和原路径名的i节点，返回出错号。否则初始设置该目录项的i节点号等于原路径名的
    // i节点号，并置包含该新添加目录项的缓冲块已修改标志，释放该缓冲块，放回目录的i节点。
	bh = add_entry(dir,basename,namelen,&de);
	if (!bh) {
		iput(dir);
		iput(oldinode);
		return -ENOSPC;
	}
	de->inode = oldinode->i_num;
	bh->b_dirt = 1;
	brelse(bh);
	iput(dir);
    // 再将原节点的链接计数加1，修改其改变时间为当前时间，并设置i节点已修改标志。最后
    // 放回原路径名的i节点，并返回0（成功）。
	oldinode->i_nlinks++;
	oldinode->i_ctime = CURRENT_TIME;
	oldinode->i_dirt = 1;
	iput(oldinode);
	return 0;
}
