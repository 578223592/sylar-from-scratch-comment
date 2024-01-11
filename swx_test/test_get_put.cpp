#include <ucontext.h>
#include <stdio.h>

void func1(void * arg)
{
    puts("1");
    puts("11");
    puts("111");
    puts("1111");

}
/**
 * 运行这个案例会发生的打印：
A
B
1
11
111
1111
A
B
1
11
。。。
 */
void context_test1()
{
    char stack[1024*128];
    ucontext_t child,main;
    getcontext(&main);//获取当前上下文)
    puts("A");
    getcontext(&child); //获取当前上下文
    puts("B");
    child.uc_stack.ss_sp = stack;//指定栈空间
    child.uc_stack.ss_size = sizeof(stack);//指定栈空间大小
    child.uc_stack.ss_flags = 0;
    child.uc_link = &main;//设置后继上下文

    makecontext(&child,(void (*)(void))func1,0);//修改上下文指向func1函数
    setcontext(&child);//设置当前上下文
    //swapcontext(&main,&child);//切换到child上下文，保存当前上下文到main，如果不保存，那么切换回来的时候就没东西了，
    puts("main");//如果设置了后继上下文，func1函数指向完后会返回此处
}


/**
 * 运行这个案例会发生的打印：
A
B
1
11
111
1111
main
 */
void context_test2()
{
    char stack[1024*128];
    ucontext_t child,main;
    getcontext(&main);//获取当前上下文)
    puts("A");
    getcontext(&child); //获取当前上下文
    puts("B");
    child.uc_stack.ss_sp = stack;//指定栈空间
    child.uc_stack.ss_size = sizeof(stack);//指定栈空间大小
    child.uc_stack.ss_flags = 0;
    child.uc_link = &main;//设置后继上下文

    makecontext(&child,(void (*)(void))func1,0);//修改上下文指向func1函数
    // setcontext(&child);//设置当前上下文
    swapcontext(&main,&child);//切换到child上下文，保存当前上下文到main，如果不保存，那么切换回来的时候就没东西了，
    puts("main");//如果设置了后继上下文，func1函数指向完后会返回此处
}
int main()
{
    context_test1();
    context_test2();
    return 0;
}