#include "process.h"
#include "fat32.h"
#include "program.h"

extern void putchar(char c);
extern void print(const char *str);
extern void print_int(int val);
extern void *kmalloc(unsigned int size);
extern void kfree(void *ptr);
extern int fat32_read_file_content(const char *filename, char *buffer, unsigned int max_size);

#define MAX_PROCESSES 16
#define PROC_NAME_LEN 32

static PCB process_table[MAX_PROCESSES];
static int current_pid = 0;
static int next_pid = 1;
static int process_count = 1;

static int find_free_slot() {
    for (int i = 1; i < MAX_PROCESSES; i++) { // 0留给 Shell
        if (process_table[i].state == PROC_TERMINATED || process_table[i].id == 0) {
            return i;
        }
    }
    return -1;
}

static void print_hex(unsigned int val) {
    char hex[] = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) {
        char c = hex[(val >> (i * 4)) & 0xF];
        putchar(c);
    }
}

// 自动安全退出函数：当进程的函数 return 时执行
void process_exit_wrapper() {
    if (current_pid > 0) {
        process_terminate(current_pid);
    }
    scheduler();
    while (1) { asm volatile("hlt"); }
}

int process_create(const char *filename, const char *argv) {
    print("Loading program: ");
    print(filename);
    print("\n");

    unsigned char *temp_buf = (unsigned char *)0x50000;
    int bytes = fat32_read_file_content(filename, (char *)temp_buf, 0x10000);
    if (bytes < sizeof(ProgramHeader)) {
        print("File too small or not found\n");
        return -1;
    }

    ProgramHeader *header = (ProgramHeader *)temp_buf;
    if (header->magic != PROG_MAGIC) {
        print("Invalid program header\n");
        return -2;
    }
    if (header->entry < sizeof(ProgramHeader) ||
        header->entry > bytes ||
        header->text_size > (unsigned int)(bytes - header->entry)) {
        print("Invalid program layout\n");
        return -2;
    }

    int slot = find_free_slot();
    if (slot == -1) {
        print("Process table full\n");
        return -3;
    }

    unsigned int payload_size = header->text_size + header->data_size + header->bss_size;
    unsigned int total_size = payload_size + header->stack_size;
    unsigned int base = (unsigned int)kmalloc(total_size);
    if (!base) {
        print("Out of memory\n");
        return -4;
    }

    unsigned char *dst = (unsigned char *)base;
    unsigned int payload_offset = header->entry;
    unsigned int available = (unsigned int)bytes - payload_offset;
    if (available < header->text_size + header->data_size) {
        print("Program data truncated\n");
        kfree((void *)base);
        return -2;
    }
    for (unsigned int i = 0; i < header->text_size + header->data_size; i++) {
        dst[i] = temp_buf[payload_offset + i];
    }

    unsigned char *bss_start = dst + header->text_size + header->data_size;
    for (unsigned int i = 0; i < header->bss_size; i++) {
        bss_start[i] = 0;
    }

    unsigned int stack_top = base + header->text_size + header->data_size + header->bss_size + header->stack_size;
    unsigned int *stack_ptr = (unsigned int *)stack_top;
    unsigned int entry = base;
    
    // 构造栈帧：先压入 process_exit_wrapper 作为 main 函数 return 后的跳转地址
    stack_ptr--;
    *stack_ptr = (unsigned int)process_exit_wrapper;
    stack_ptr--;
    *stack_ptr = entry;
    stack_ptr--; *stack_ptr = 0; // ebp
    stack_ptr--; *stack_ptr = 0; // ebx
    stack_ptr--; *stack_ptr = 0; // esi
    stack_ptr--; *stack_ptr = 0; // edi

    process_table[slot].id = next_pid++;
    process_table[slot].state = PROC_READY;
    process_table[slot].entry = entry;
    process_table[slot].eip = entry;
    process_table[slot].base = base;
    process_table[slot].code_size = header->text_size;
    process_table[slot].data_size = header->data_size + header->bss_size;
    process_table[slot].stack_base = stack_top - header->stack_size;
    process_table[slot].stack_size = header->stack_size;
    process_table[slot].esp = (unsigned int)stack_ptr;
    process_table[slot].ebp = (unsigned int)stack_ptr;
    process_table[slot].priority = 1;
    process_table[slot].time_slice = 10;
    process_table[slot].parent_id = current_pid;
    process_table[slot].exit_code = 0;

    int i;
    for (i = 0; i < PROC_NAME_LEN - 1 && header->name[i]; i++) {
        process_table[slot].name[i] = header->name[i];
    }
    process_table[slot].name[i] = '\0';

    process_count++;
    print("Process created: PID=");
    print_int(process_table[slot].id);
    print("\n");

    return process_table[slot].id;
}

__attribute__((naked)) void switch_context(unsigned int *old_esp_ptr, unsigned int new_esp) {
    asm volatile(
        "push %%ebp\n"
        "push %%ebx\n"
        "push %%esi\n"
        "push %%edi\n"
        "mov 20(%%esp), %%eax\n"
        "mov %%esp, (%%eax)\n"
        "mov 24(%%esp), %%esp\n"
        "pop %%edi\n"
        "pop %%esi\n"
        "pop %%ebx\n"
        "pop %%ebp\n"
        "ret\n"
        : : : "memory", "eax"
    );
}

void scheduler() {
    int prev_pid = current_pid;
    
    int found = -1;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        int next_idx = (current_pid + 1 + i) % MAX_PROCESSES;
        if (process_table[next_idx].state == PROC_READY) {
            found = next_idx;
            break;
        }
    }
    
    if (found == -1 || found == current_pid) {
        return;
    }
    
    current_pid = found;
    process_table[current_pid].state = PROC_RUNNING;
    
    if (prev_pid != -1 && process_table[prev_pid].state == PROC_RUNNING) {
        process_table[prev_pid].state = PROC_READY;
        switch_context(&process_table[prev_pid].esp, process_table[current_pid].esp);
    } else {
        unsigned int dummy_esp;
        switch_context(&dummy_esp, process_table[current_pid].esp);
    }
}

int process_terminate(unsigned int pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].id == pid && process_table[i].state != PROC_TERMINATED) {
            process_table[i].state = PROC_TERMINATED;
            if (process_table[i].base) {
                kfree((void *)process_table[i].base);
            }
            process_count--;
            print("Process terminated: PID=");
            print_int(pid);
            print("\n");
            if (current_pid == i) {
                current_pid = 0; // 回到 Shell
            }
            return 0;
        }
    }
    return -1;
}

int process_kill(unsigned int pid) {
    return process_terminate(pid);
}

void process_list() {
    print("PID  NAME      STATE    MEM\n");
    print("---  --------  -------- ----\n");
    
    int count = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].state == PROC_TERMINATED) continue;
        count++;
        print_int(process_table[i].id);
        print("   ");
        print(process_table[i].name);
        
        int len = 0;
        while (process_table[i].name[len]) len++;
        for (int j = len; j < 10; j++) putchar(' ');
        
        switch(process_table[i].state) {
            case PROC_READY:   print("READY    "); break;
            case PROC_RUNNING: print("RUNNING  "); break;
            case PROC_WAITING: print("WAITING  "); break;
            default:           print("UNKNOWN  "); break;
        }
        
        unsigned int total_mem = process_table[i].code_size + process_table[i].data_size + process_table[i].stack_size;
        print_int(total_mem / 1024);
        print("KB\n");
    }
}

void process_yield() {
    scheduler();
}

/* 核心修复：纯裸函数系统调用门，避免栈崩溃 (int 0x80) */
__attribute__((naked)) void syscall_handler() {
    asm volatile(
        "pusha\n"             // 压入所有通用寄存器
        
        "cmp $0, %%eax\n"     // SYS_PRINT
        "jne 1f\n"
        "push %%ebx\n"
        "call print\n"
        "add $4, %%esp\n"
        "jmp 9f\n"

        "1:\n"
        "cmp $1, %%eax\n"     // SYS_EXIT
        "jne 2f\n"
        "call process_exit_wrapper\n"
        "jmp 9f\n"

        "2:\n"
        "cmp $2, %%eax\n"     // SYS_YIELD
        "jne 3f\n"
        "call scheduler\n"
        "jmp 9f\n"

        "3:\n"
        "cmp $3, %%eax\n"     // SYS_GETPID (假设编号为 3)
        "jne 4f\n"
        "call get_current_pid\n"
        "mov %%eax, 28(%%esp)\n" // 核心：覆盖栈中保存的 EAX，以便 popa 时能将返回值带回用户态
        "jmp 9f\n"

        "4:\n"
        "cmp $4, %%eax\n"     // SYS_PRINT_INT (假设编号为 4)
        "jne 9f\n"
        "push %%ebx\n"
        "call print_int\n"
        "add $4, %%esp\n"

        "9:\n"
        "popa\n"              // 恢复寄存器 (如果执行了 getpid，EAX 会被替换为 pid)
        "iret\n"              // 返回用户态
        : : : "memory"
    );
}

void process_init() {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_table[i].id = 0;
        process_table[i].state = PROC_TERMINATED;
        process_table[i].name[0] = '\0';
        process_table[i].esp = 0;
        process_table[i].ebp = 0;
        process_table[i].eip = 0;
        process_table[i].base = 0;
        process_table[i].code_size = 0;
        process_table[i].data_size = 0;
        process_table[i].stack_base = 0;
        process_table[i].stack_size = 0;
    }
    
    // 将当前的 Shell 注册为 PID 0 进程
    process_table[0].id = 0;
    process_table[0].state = PROC_RUNNING;
    process_table[0].name[0] = 'S';
    process_table[0].name[1] = 'h';
    process_table[0].name[2] = 'e';
    process_table[0].name[3] = 'l';
    process_table[0].name[4] = 'l';
    process_table[0].name[5] = '\0';

    current_pid = 0;
    next_pid = 1;
    process_count = 1;
    print("Process manager initialized (Shell set as PID 0).\n");
}

int get_current_pid() {
    return current_pid;
}

int get_process_count() {
    return process_count;
}