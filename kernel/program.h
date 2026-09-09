#ifndef PROGRAM_H
#define PROGRAM_H

// 程序头 - 放在每个 .bin 文件开头
#define PROG_MAGIC 0x43414D43  // "CMAC" (Cl_OS Magic)

typedef struct {
    unsigned int magic;         // 魔数: 0x43414D43
    unsigned int version;       // 版本号
    unsigned int entry;         // 入口点偏移（从文件开头算）
    unsigned int text_size;     // 代码段大小
    unsigned int data_size;     // 数据段大小
    unsigned int bss_size;      // BSS 段大小（未初始化数据）
    unsigned int stack_size;    // 栈大小
    char name[32];              // 程序名
    unsigned int checksum;      // 校验和（简单 XOR）
} ProgramHeader;

#endif