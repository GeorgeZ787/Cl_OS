/* kernel/fat12.c - Minimal FAT12 stub */
#include "fat12.h"

int fat12_init(unsigned char *bpb_addr, unsigned char *fat_addr, unsigned char *root_dir_addr) {
    // 软盘文件系统暂未启用，直接返回成功（或可忽略）
    return 0;
}

int fat12_list_directory(void) {
    // 软盘目录不可用，返回 -1 表示无数据
    return -1;
}