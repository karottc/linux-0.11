#
# Makefile for the FREAX-kernel.
#
# Note! Dependencies are done automagically by 'make dep', which also
# removes any old dependencies. DON'T put your own dependencies here
# unless it's something special (ie not a .c file).
#

# GNU的二进制文件处理程序，用于创建、修改以及从归档文件中抽取文件
AR	=ar
AS	=as 		# GNU的汇编程序
LD	=ld 		# GNU的链接程序
# 链接程序所有的参数，-s 输出文件中省略所有符号信息。-x 删除所有局部符号。
LDFLAGS	=-s -x
CC	=gcc -mcpu=i386  	# GNU C语言编译器
# C 编译程序选项。-Wall 显示所有的警告信息：-0 优化选项，优化代码长度和执行时间；
# -fstrength-reduce 优化循环代码，排除重复变量；-fomit-frame-pointer省略保存不
#  必要的框架指针；-fcombine-regs合并寄存器，减少寄存器类的使用；-finline-function
#  将所有简单短小的函数代码嵌入调用程序中；-mstring-insns Linus自己添加的优化选项，
#  以后不再使用；-nostdinc -L../include不使用默认路径中的包含文件，而使用这里指定
#  目录中的。
CFLAGS	=-Wall -O -fstrength-reduce -fomit-frame-pointer \
	-finline-functions -nostdinc -I../include
# C前处理选项。-E只运行C前处理，对所有指定的C程序进行预处理并将处理结果输出到标准
# 输出设备或指定的输出文件中.
CPP	=gcc -E -nostdinc -I../include

# 下面的规则指示make利用下面的命令将所有的.c文件编译生成.s汇编程序。该规则的命令
# 指使gcc采用CFLAGS所指定的选项对C代码编译后不进行汇编就停止(-S),从而产生与输入的
# 各个C文件对应的汇编代码文件。默认情况下所产生的汇编程序文件名是原C文件名去掉.c而
# 加上.s的后缀。-o表示其后是输出文件的名称。其中$*.s(或$@)是自动目标变量，$<代表第
# 一个先决条件，这里即是符合条件*.c的文件。
# 下面这3个不同规则分别用于不同的操作要求。若目标是.s文件，而源文件是.c文件则会使
# 用第一个规则；若目录是.o,而原文件是.s，则使用第2个规则；若目录是.o文件而原文件
# 是c文件，则可直接使用第3个规则。
.c.s:
	$(CC) $(CFLAGS) \
	-S -o $*.s $<
.s.o:
	$(AS) -o $*.o $<
.c.o:
	$(CC) $(CFLAGS) \
	-c -o $*.o $<

OBJS  = sched.o system_call.o traps.o asm.o fork.o \
	panic.o printk.o vsprintf.o sys.o exit.o \
	signal.o mktime.o

# 在有了先决条件OBJS后使用下面的命令连接成目标kernel.o
# 选项'-r' 用于指示生成可重定位的输出，即产生可以作为链接器ld输入的目标文件。
kernel.o: $(OBJS)
	$(LD) -r -o kernel.o $(OBJS)
	sync

# 下面规则用于清理工作。当执行'make clean'时，就会执行上面的命令,去除所有编译
# 链接生成的文件。'rm'是文件删除命令，选项-f含义是忽略不存在的文件并且不显示删除信息。
clean:
	rm -f core *.o *.a tmp_make keyboard.s
	for i in *.c;do rm -f `basename $$i .c`.s;done
	(cd chr_drv; make clean)
	(cd blk_drv; make clean)
	(cd math; make clean)

# 下面的目标或规则用于检查各文件之间的依赖关系。方法如下：
# 使用字符串编辑程序sed对makefile文件(这里即是自己)进行处理，输出为删除Makefile
# 文件中'### Dependencies'行后面的所有航，并生成tmp_make临时文件。然后对kernel/目录下
# 的每一个C文件执行gcc预处理操作，-M标志告诉预处理程序输出描述符每个目标文件相关性的
# 规则，并且这些规则符合make语法。对于每一个源文件，预处理程序输出一个make规则，其结果
# 形式是相应源程序文件的目标文件名加上其依赖关系——该源文件中包含的所有头文件列表。把
# 预处理结果都添加到临时文件tmp_make中，然后将该临时文件复制成新的Makefile文件。
dep:
	sed '/\#\#\# Dependencies/q' < Makefile > tmp_make
	(for i in *.c;do echo -n `echo $$i | sed 's,\.c,\.s,'`" "; \
		$(CPP) -M $$i;done) >> tmp_make
	cp tmp_make Makefile
	(cd chr_drv; make dep)
	(cd blk_drv; make dep)

### Dependencies:
exit.s exit.o: exit.c ../include/errno.h ../include/signal.h \
  ../include/sys/types.h ../include/sys/wait.h ../include/linux/sched.h \
  ../include/linux/head.h ../include/linux/fs.h ../include/linux/mm.h \
  ../include/linux/kernel.h ../include/linux/tty.h ../include/termios.h \
  ../include/asm/segment.h
fork.s fork.o: fork.c ../include/errno.h ../include/linux/sched.h \
  ../include/linux/head.h ../include/linux/fs.h ../include/sys/types.h \
  ../include/linux/mm.h ../include/signal.h ../include/linux/kernel.h \
  ../include/asm/segment.h ../include/asm/system.h
mktime.s mktime.o: mktime.c ../include/time.h
panic.s panic.o: panic.c ../include/linux/kernel.h ../include/linux/sched.h \
  ../include/linux/head.h ../include/linux/fs.h ../include/sys/types.h \
  ../include/linux/mm.h ../include/signal.h
printk.s printk.o: printk.c ../include/stdarg.h ../include/stddef.h \
  ../include/linux/kernel.h
sched.s sched.o: sched.c ../include/linux/sched.h ../include/linux/head.h \
  ../include/linux/fs.h ../include/sys/types.h ../include/linux/mm.h \
  ../include/signal.h ../include/linux/kernel.h ../include/linux/sys.h \
  ../include/linux/fdreg.h ../include/asm/system.h ../include/asm/io.h \
  ../include/asm/segment.h
signal.s signal.o: signal.c ../include/linux/sched.h ../include/linux/head.h \
  ../include/linux/fs.h ../include/sys/types.h ../include/linux/mm.h \
  ../include/signal.h ../include/linux/kernel.h ../include/asm/segment.h
sys.s sys.o: sys.c ../include/errno.h ../include/linux/sched.h \
  ../include/linux/head.h ../include/linux/fs.h ../include/sys/types.h \
  ../include/linux/mm.h ../include/signal.h ../include/linux/tty.h \
  ../include/termios.h ../include/linux/kernel.h ../include/asm/segment.h \
  ../include/sys/times.h ../include/sys/utsname.h
traps.s traps.o: traps.c ../include/string.h ../include/linux/head.h \
  ../include/linux/sched.h ../include/linux/fs.h ../include/sys/types.h \
  ../include/linux/mm.h ../include/signal.h ../include/linux/kernel.h \
  ../include/asm/system.h ../include/asm/segment.h ../include/asm/io.h
vsprintf.s vsprintf.o: vsprintf.c ../include/stdarg.h ../include/string.h
