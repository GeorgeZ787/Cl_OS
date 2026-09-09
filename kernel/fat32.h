#ifndef FAT32_H
#define FAT32_H

typedef struct {
    unsigned char  jump[3];
    char           oem[8];
    unsigned short bytes_per_sector;
    unsigned char  sectors_per_cluster;
    unsigned short reserved_sectors;
    unsigned char  num_fats;
    unsigned short root_entries;
    unsigned short total_sectors_16;
    unsigned char  media;
    unsigned short sectors_per_fat_16;
    unsigned short sectors_per_track;
    unsigned short num_heads;
    unsigned int   hidden_sectors;
    unsigned int   total_sectors_32;
    unsigned int   sectors_per_fat_32;
    unsigned short flags;
    unsigned short version;
    unsigned int   root_cluster;
    unsigned short fs_info_sector;
    unsigned short backup_boot_sector;
    unsigned char  reserved1[12];
    unsigned char  drive_number;
    unsigned char  reserved2;
    unsigned char  boot_signature;
    unsigned int   volume_id;
    char           volume_label[11];
    char           fs_type[8];
} __attribute__((packed)) FAT32_BPB;

typedef struct {
    char           name[11];
    unsigned char  attributes;
    unsigned char  reserved;
    unsigned char  creation_tenths;
    unsigned short creation_time;
    unsigned short creation_date;
    unsigned short last_access_date;
    unsigned short first_cluster_high;
    unsigned short last_write_time;
    unsigned short last_write_date;
    unsigned short first_cluster_low;
    unsigned int   file_size;
} __attribute__((packed)) FAT32_DirEntry;

// 属性定义
#define ATTR_READ_ONLY  0x01
#define ATTR_HIDDEN     0x02
#define ATTR_SYSTEM     0x04
#define ATTR_VOLUME_ID  0x08
#define ATTR_DIRECTORY  0x10
#define ATTR_ARCHIVE    0x20
#define ATTR_LONG_NAME  0x0F

int fat32_init(unsigned int partition_start_lba);
void fat32_list_root();
int fat32_read_file_content(const char *filename, char *buffer, unsigned int max_size);
int fat32_overwrite_file(const char *filename, const char *data, unsigned int size);
int fat32_create_file(const char *name);
int fat32_delete_file(const char *name);

// 新增目录操作函数
int fat32_create_directory(const char *name);
int fat32_change_directory(const char *path);
void fat32_list_current_directory();
void fat32_get_current_path(char *buffer, int size);

#endif