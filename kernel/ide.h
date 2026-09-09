#ifndef IDE_H
#define IDE_H

/* MBR 分区表结构 */
#pragma pack(1)
typedef struct {
    unsigned char  status;
    unsigned char  chs_start[3];
    unsigned char  type;
    unsigned char  chs_end[3];
    unsigned int   lba_start;
    unsigned int   sectors;
} MBRPartition;

typedef struct {
    unsigned char  bootstrap[446];
    MBRPartition   partitions[4];
    unsigned short signature;
} MBR;
#pragma pack()

void ide_init();
int ide_read_sectors(unsigned int lba, unsigned char count, unsigned char *buffer);
int ide_write_sectors(unsigned int lba, unsigned char count, const unsigned char *buffer);
void ide_display_info();
void mbr_list_partitions();

#endif