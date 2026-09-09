/* kernel/ide.c - Complete IDE PIO driver */
#include "ide.h"

extern void print(const char *str);
extern void putchar(char c);

static inline unsigned char inb(unsigned short port) {
    unsigned char ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outb(unsigned short port, unsigned char data) {
    __asm__ volatile ("outb %0, %1" :: "a"(data), "Nd"(port));
}
static inline unsigned short inw(unsigned short port) {
    unsigned short ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outw(unsigned short port, unsigned short data) {
    __asm__ volatile ("outw %0, %1" :: "a"(data), "Nd"(port));
}

#define IDE_BASE 0x1F0
#define IDE_DATA        (IDE_BASE + 0)
#define IDE_ERROR       (IDE_BASE + 1)
#define IDE_SECTOR_COUNT (IDE_BASE + 2)
#define IDE_LBA_LOW     (IDE_BASE + 3)
#define IDE_LBA_MID     (IDE_BASE + 4)
#define IDE_LBA_HIGH    (IDE_BASE + 5)
#define IDE_DRIVE       (IDE_BASE + 6)
#define IDE_STATUS      (IDE_BASE + 7)
#define IDE_COMMAND     (IDE_BASE + 7)

static void ide_wait_not_busy() {
    while (inb(IDE_STATUS) & 0x80);
}

static void ide_wait_drq() {
    while (!(inb(IDE_STATUS) & 0x08));
}

void ide_init() {
    unsigned short *vga = (unsigned short *)0xB8000;
    vga[110] = 0x0F00 | 'I';
    vga[111] = 0x0F00 | 'D';
    vga[112] = 0x0F00 | 'E';
    vga[113] = 0x0F00 | 'S';
    
    // 等待控制器就绪（加超时）
    int timeout = 100000;
    while (timeout-- > 0) {
        unsigned char status = inb(0x1F7);
        if (!(status & 0x80)) break;
    }
    
    vga[115] = 0x0F00 | 'R';
    vga[116] = 0x0F00 | 'E';
    vga[117] = 0x0F00 | 'A';
    vga[118] = 0x0F00 | 'D';
    vga[119] = 0x0F00 | 'Y';
    
    // 选择主盘
    outb(0x1F6, 0xA0);
    
    vga[121] = 0x0F00 | 'D';
    vga[122] = 0x0F00 | 'O';
    vga[123] = 0x0F00 | 'N';
    vga[124] = 0x0F00 | 'E';
}

int ide_read_sectors(unsigned int lba, unsigned char count, unsigned char *buffer) {
    ide_wait_not_busy();
    outb(IDE_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(IDE_SECTOR_COUNT, count);
    outb(IDE_LBA_LOW, (unsigned char)(lba & 0xFF));
    outb(IDE_LBA_MID, (unsigned char)((lba >> 8) & 0xFF));
    outb(IDE_LBA_HIGH, (unsigned char)((lba >> 16) & 0xFF));
    outb(IDE_COMMAND, 0x20);

    while (count--) {
        ide_wait_not_busy();
        ide_wait_drq();
        unsigned short *buf = (unsigned short *)buffer;
        for (int i = 0; i < 256; i++) {
            buf[i] = inw(IDE_DATA);
        }
        buffer += 512;
    }
    return 0;
}

int ide_write_sectors(unsigned int lba, unsigned char count, const unsigned char *buffer) {
    // 等待驱动器就绪
    while (!(inb(IDE_STATUS) & 0x40));   // DRDY
    ide_wait_not_busy();
    outb(IDE_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(IDE_SECTOR_COUNT, count);
    outb(IDE_LBA_LOW, (unsigned char)(lba & 0xFF));
    outb(IDE_LBA_MID, (unsigned char)((lba >> 8) & 0xFF));
    outb(IDE_LBA_HIGH, (unsigned char)((lba >> 16) & 0xFF));
    outb(IDE_COMMAND, 0x30);

    while (count--) {
        ide_wait_not_busy();
        ide_wait_drq();
        unsigned short *buf = (unsigned short *)buffer;
        for (int i = 0; i < 256; i++) {
            outw(IDE_DATA, buf[i]);
        }
        ide_wait_not_busy();
        buffer += 512;
    }
    // 缓存刷新
    outb(IDE_COMMAND, 0xE7);
    ide_wait_not_busy();
    return 0;
}

void ide_display_info() {
    ide_wait_not_busy();
    outb(IDE_DRIVE, 0xE0);
    outb(IDE_SECTOR_COUNT, 0);
    outb(IDE_LBA_LOW, 0);
    outb(IDE_LBA_MID, 0);
    outb(IDE_LBA_HIGH, 0);
    outb(IDE_COMMAND, 0xEC);

    if (inb(IDE_STATUS) == 0) { print("No IDE device.\n"); return; }
    ide_wait_not_busy();
    if (!(inb(IDE_STATUS) & 0x08)) { print("IDENTIFY failed.\n"); return; }

    unsigned short data[256];
    for (int i=0; i<256; i++) data[i]=inw(IDE_DATA);

    print("HDD Model: ");
    for (int i=27; i<=46; i++) {
        putchar((data[i]>>8)&0xFF);
        putchar(data[i]&0xFF);
    }
    print("\nMax LBA: ");
    unsigned int max_lba = data[60] | (data[61]<<16);
    char buf[20]; int p=0;
    if (max_lba) { while (max_lba) { buf[p++]='0'+max_lba%10; max_lba/=10; } }
    else buf[p++]='0';
    while (p) putchar(buf[--p]);
    print(" sectors\n");
    outb(IDE_DRIVE, 0xE0);
}

void mbr_list_partitions() {
    unsigned char sector[512];
    if (ide_read_sectors(0, 1, sector) != 0) { print("MBR read fail.\n"); return; }
    MBR *mbr = (MBR *)sector;
    if (mbr->signature != 0xAA55) { print("Invalid MBR.\n"); return; }
    print("Partitions:\n");
    for (int i=0; i<4; i++) {
        MBRPartition *p = &mbr->partitions[i];
        if (!p->type) continue;
        print("  ");
        putchar('1'+i);
        print(": type=");
        char hex[3]; hex[0]="0123456789ABCDEF"[p->type>>4]; hex[1]="0123456789ABCDEF"[p->type&0xF]; hex[2]=0;
        print(hex);
        print(" start=");
        unsigned int v = p->lba_start;
        int pos = 0;
        char num[16];
        if (!v) num[pos++] = '0'; else while (v) { num[pos++] = '0' + v % 10; v /= 10; }
        while (pos) putchar(num[--pos]);
        print(" sectors=");
        v = p->sectors;
        pos = 0;
        if (!v) num[pos++] = '0'; else while (v) { num[pos++] = '0' + v % 10; v /= 10; }
        while (pos) putchar(num[--pos]);
        print("\n");
    }
}