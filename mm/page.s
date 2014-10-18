/*
 *  linux/mm/page.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * page.s contains the low-level page-exception code.
 * the real work is done in mm.c
 */

.globl page_fault       # 声明为全局变量。将在traps.c中用于设置页异常描述符。

page_fault:
	xchgl %eax,(%esp)       # 取出错码到eax
	pushl %ecx
	pushl %edx
	push %ds
	push %es
	push %fs
	movl $0x10,%edx         # 置内核数据段选择符
	mov %dx,%ds
	mov %dx,%es
	mov %dx,%fs
	movl %cr2,%edx          # 取引起页面异常的线性地址
	pushl %edx              # 将该线性地址和出错码压入栈中，作为将调用函数的参数
	pushl %eax
	testl $1,%eax           # 测试页存在标志P（为0），如果不是缺页引起的异常则跳转
	jne 1f
	call do_no_page         # 调用缺页处理函数
	jmp 2f
1:	call do_wp_page         # 调用写保护处理函数
2:	addl $8,%esp            # 丢弃压入栈的两个参数，弹出栈中寄存器并退出中断。
	pop %fs
	pop %es
	pop %ds
	popl %edx
	popl %ecx
	popl %eax
	iret
