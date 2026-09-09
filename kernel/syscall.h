#ifndef SYSCALL_H
#define SYSCALL_H

/* 
 * ChlorineOS 用户态系统调用接口
 * 注意：必须使用 static inline，确保代码直接嵌入用户程序，避免产生外部链接符号。
 * 寄存器约定：
 * EAX = 系统调用号
 * EBX = 第一个参数
 * 返回值存放在 EAX 中
 */

// SYS_PRINT (0): 打印字符串
static inline void print(const char *str) {
    // "a"(0) 表示将 0 放入 eax，"b"(str) 表示将 str 的指针放入 ebx
    asm volatile(
        "int $0x80" 
        : 
        : "a"(0), "b"(str) 
        : "memory"
    );
}

// SYS_EXIT (1): 退出当前进程
static inline void exit(int code) {
    asm volatile(
        "int $0x80" 
        : 
        : "a"(1), "b"(code) 
        : "memory"
    );
    // 确保编译器知道这个函数不会返回
    while (1) {} 
}

// SYS_YIELD (2): 主动让出 CPU 供调度器切换任务
static inline void yield(void) {
    asm volatile(
        "int $0x80" 
        : 
        : "a"(2) 
        : "memory"
    );
}

// SYS_GETPID (3): 获取当前进程 PID
static inline int getpid(void) {
    int pid;
    // "=a"(pid) 表示将中断返回后 eax 的值赋给变量 pid
    asm volatile(
        "int $0x80" 
        : "=a"(pid) 
        : "a"(3) 
        : "memory"
    );
    return pid;
}

// SYS_PRINT_INT (4): 打印整数
static inline void print_int(int val) {
    asm volatile(
        "int $0x80" 
        : 
        : "a"(4), "b"(val) 
        : "memory"
    );
}

#endif // SYSCALL_H