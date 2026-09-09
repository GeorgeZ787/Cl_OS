#include "syscall.h" // 确保 syscall.h 里的函数是 static inline 的！

void _start() {
    // 魔法：将绝对字符串转换为栈上的局部数组
    char msg_start[] = "=== Test Process Started ===\n";
    char msg_pid[] = "My PID: ";
    char msg_nl[] = "\n";
    char msg_count[] = "Count: ";
    char msg_done[] = "=== Test Process Done ===\n";

    print(msg_start);
    print(msg_pid);
    print_int(getpid());
    print(msg_nl);
    
    for (int i = 0; i < 5; i++) {
        print(msg_count);
        print_int(i);
        print(msg_nl);
        yield();
    }
    
    print(msg_done);
    exit(0);
}