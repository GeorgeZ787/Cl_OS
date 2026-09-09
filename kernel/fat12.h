#ifndef FAT12_H
#define FAT12_H

int fat12_init(unsigned char *bpb_addr, unsigned char *fat_addr, unsigned char *root_dir_addr);
int fat12_list_directory(void);

#endif