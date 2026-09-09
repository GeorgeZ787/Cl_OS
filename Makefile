ASM = nasm
CC = gcc
LD = ld

IMG = cl_os.img
HDD = hdd.img
RESERVED = 33

BOOT_DIR = boot
KERNEL_DIR = kernel
BUILD_DIR = build
PROGRAMS_DIR = programs
TOOLS_DIR = tools

CFLAGS = -m32 -ffreestanding -fno-pic -Ikernel -I.
LDFLAGS = -m elf_i386 -nostdlib -T kernel/link.ld

BOOT_BIN = $(BUILD_DIR)/boot.bin
LOADER_BIN = $(BUILD_DIR)/loader.bin
KERNEL_BIN = $(BUILD_DIR)/kernel.bin

KERNEL_OBJS = $(BUILD_DIR)/entry.o $(BUILD_DIR)/kernel.o \
              $(BUILD_DIR)/fat12.o $(BUILD_DIR)/fat32.o \
              $(BUILD_DIR)/ide.o $(BUILD_DIR)/process.o

PROGRAMS = $(BUILD_DIR)/test.bin

all: $(IMG)

# ========== 引导程序 ==========
$(BOOT_BIN): $(BOOT_DIR)/boot.asm
	$(ASM) -f bin -Iinclude $< -o $@

$(LOADER_BIN): $(BOOT_DIR)/loader.asm
	$(ASM) -f bin -Iinclude $< -o $@

# ========== 内核 ==========
$(BUILD_DIR)/entry.o: $(KERNEL_DIR)/entry.asm
	$(ASM) -f elf32 $< -o $@

$(BUILD_DIR)/kernel.o: $(KERNEL_DIR)/kernel.c $(KERNEL_DIR)/process.h $(KERNEL_DIR)/program.h
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/fat12.o: $(KERNEL_DIR)/fat12.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/fat32.o: $(KERNEL_DIR)/fat32.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/ide.o: $(KERNEL_DIR)/ide.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/process.o: $(KERNEL_DIR)/process.c $(KERNEL_DIR)/process.h $(KERNEL_DIR)/program.h
	$(CC) $(CFLAGS) -c $< -o $@

# 直接生成二进制（跳过 objcopy）
$(KERNEL_BIN): $(KERNEL_OBJS)
	$(LD) -m elf_i386 -nostdlib -T kernel/link.ld -o $@ --oformat binary $^

# ========== 用户程序 ==========
$(BUILD_DIR)/%.o: $(PROGRAMS_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# ========== 工具 ==========
$(BUILD_DIR)/add_header: $(TOOLS_DIR)/add_header.c
	gcc -o $@ $<

$(BUILD_DIR)/%.raw.bin: $(BUILD_DIR)/%.o
	$(LD) -m elf_i386 -Ttext 0x10000 -o $@ --oformat binary $<

$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.raw.bin $(BUILD_DIR)/add_header
	$(BUILD_DIR)/add_header $< $@

# ========== 镜像 ==========
$(IMG): $(BOOT_BIN) $(LOADER_BIN) $(KERNEL_BIN)
	@echo "=> Creating FAT12 image"
	dd if=/dev/zero of=$(IMG) bs=512 count=2880 > /dev/null 2>&1
	mkdosfs -F 12 -R $(RESERVED) -S 512 -s 1 -M 0xF8 $(IMG) > /dev/null 2>&1
	dd if=$(BOOT_BIN) of=$(IMG) conv=notrunc > /dev/null 2>&1
	dd if=$(LOADER_BIN) of=$(IMG) conv=notrunc seek=1 > /dev/null 2>&1
	dd if=$(KERNEL_BIN) of=$(IMG) conv=notrunc seek=2 > /dev/null 2>&1
	@echo "=> Image built: $(IMG)"

# ========== 运行 ==========
run: $(IMG)
	qemu-system-i386 -fda $(IMG) -hda $(HDD) -boot a -m 64 -vnc :1

run-nographic: $(IMG)
	qemu-system-i386 -fda $(IMG) -hda $(HDD) -boot a -m 64 -nographic

# ========== 清理 ==========
clean:
	rm -f $(BUILD_DIR)/*.bin $(BUILD_DIR)/*.o
	rm -f $(IMG)

# ========== 创建硬盘 ==========
hdd:
	dd if=/dev/zero of=$(HDD) bs=1M count=32
	mkfs.fat -F 32 $(HDD)

# ========== 程序 ==========
programs: $(PROGRAMS)

.PHONY: all run run-nographic clean hdd programs