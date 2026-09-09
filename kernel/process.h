#ifndef PROCESS_H
#define PROCESS_H

#define MAX_PROCESSES 16
#define PROC_NAME_LEN 32

typedef enum {
    PROC_TERMINATED = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_WAITING
} ProcessState;

typedef struct {
    int id;
    char name[PROC_NAME_LEN];
    ProcessState state;
    unsigned int esp;
    unsigned int ebp;
    unsigned int eip;
    unsigned int entry;
    unsigned int base;
    unsigned int code_size;
    unsigned int data_size;
    unsigned int stack_base;
    unsigned int stack_size;
    unsigned int priority;
    unsigned int time_slice;
    int parent_id;
    int exit_code;
} PCB;

void process_init();
int process_create(const char *filename, const char *argv);
int process_terminate(unsigned int pid);
int process_kill(unsigned int pid);
void process_list();
void scheduler();
void process_yield();
int get_current_pid();
int get_process_count();

#endif