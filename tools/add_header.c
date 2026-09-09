// tools/add_header.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct {
    unsigned int magic;
    unsigned int version;
    unsigned int entry;
    unsigned int text_size;
    unsigned int data_size;
    unsigned int bss_size;
    unsigned int stack_size;
    char name[32];
    unsigned int checksum;
} ProgramHeader;

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: %s <input.bin> <output.bin>\n", argv[0]);
        return 1;
    }

    FILE *fp = fopen(argv[1], "rb");
    if (!fp) {
        printf("Cannot open file: %s\n", argv[1]);
        return 1;
    }

    // 获取文件大小
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    // 读取整个文件
    unsigned char *data = malloc(file_size);
    fread(data, 1, file_size, fp);

    // 创建程序头
    ProgramHeader header;
    memset(&header, 0, sizeof(header));
    header.magic = 0x43414D43;
    header.version = 1;
    header.entry = sizeof(ProgramHeader);  // 入口点在头部之后
    header.text_size = file_size;
    header.data_size = 0;
    header.bss_size = 0;
    header.stack_size = 0x1000;  // 4KB 栈
    strncpy(header.name, argv[1], sizeof(header.name) - 1);

    // 简单校验和
    uint32_t checksum = 0;
    for (int i = 0; i < file_size; i++) {
        checksum ^= data[i];
    }
    header.checksum = checksum;

    FILE *out = fopen(argv[2], "wb");
    if (!out) {
        printf("Cannot create file: %s\n", argv[2]);
        free(data);
        fclose(fp);
        return 1;
    }
    fwrite(&header, 1, sizeof(header), out);
    fwrite(data, 1, file_size, out);

    free(data);
    fclose(fp);
    fclose(out);

    printf("Added header to %s (size: %ld bytes)\n", argv[2], file_size + sizeof(header));
    return 0;
}