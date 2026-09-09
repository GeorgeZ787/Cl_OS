#include "fat32.h"
#include "ide.h"

extern void print(const char *str);
extern void putchar(char c);
extern void print_int(int val);
extern void set_color(unsigned char fg, unsigned char bg);

static unsigned int part_start_lba;
static FAT32_BPB bpb;
static unsigned int fat_start, data_start, root_cluster, sectors_per_cluster, bytes_per_sector;

// 当前目录簇（0 = 根目录）
static unsigned int current_dir_cluster = 0;
static char current_path[256] = "/";

static unsigned int cluster_to_lba(unsigned int cluster) {
    return data_start + (cluster - 2) * sectors_per_cluster;
}

static int read_cluster(unsigned int cluster, unsigned char *buffer) {
    unsigned int lba = cluster_to_lba(cluster);
    return ide_read_sectors(lba, sectors_per_cluster, buffer);
}

static unsigned int read_fat_entry(unsigned int cluster) {
    unsigned int fat_offset = cluster * 4;
    unsigned int fat_sector = fat_start + (fat_offset / bytes_per_sector);
    unsigned int entry_offset = fat_offset % bytes_per_sector;

    unsigned int val = 0;
    unsigned char sector[512];
    if (ide_read_sectors(fat_sector, 1, sector) != 0) return 0x0FFFFFFF;
    
    if (entry_offset > bytes_per_sector - 4) {
        unsigned char next_sector[512];
        if (ide_read_sectors(fat_sector + 1, 1, next_sector) != 0) return 0x0FFFFFFF;
        unsigned char buffer[4];
        int bytes_in_first = bytes_per_sector - entry_offset;
        for (int i = 0; i < bytes_in_first; i++) buffer[i] = sector[entry_offset + i];
        for (int i = 0; i < 4 - bytes_in_first; i++) buffer[bytes_in_first + i] = next_sector[i];
        val = *(unsigned int *)buffer;
    } else {
        val = *(unsigned int *)(sector + entry_offset);
    }
    return val & 0x0FFFFFFF;
}

static int write_fat_entry(unsigned int cluster, unsigned int value) {
    unsigned int fat_offset = cluster * 4;
    unsigned int fat_sector = fat_start + (fat_offset / bytes_per_sector);
    unsigned int entry_offset = fat_offset % bytes_per_sector;
    value &= 0x0FFFFFFF;

    unsigned char sector[512];
    if (ide_read_sectors(fat_sector, 1, sector) != 0) return -1;
    
    if (entry_offset > bytes_per_sector - 4) {
        unsigned char next_sector[512];
        if (ide_read_sectors(fat_sector + 1, 1, next_sector) != 0) return -1;
        int bytes_in_first = bytes_per_sector - entry_offset;
        unsigned char *ptr = (unsigned char *)&value;
        for (int i = 0; i < bytes_in_first; i++) sector[entry_offset + i] = ptr[i];
        for (int i = 0; i < 4 - bytes_in_first; i++) next_sector[i] = ptr[bytes_in_first + i];
        if (ide_write_sectors(fat_sector, 1, sector) != 0) return -1;
        if (ide_write_sectors(fat_sector + 1, 1, next_sector) != 0) return -1;
    } else {
        *(unsigned int *)(sector + entry_offset) = value;
        if (ide_write_sectors(fat_sector, 1, sector) != 0) return -1;
    }
    return 0;
}

static void make_83_name(const char *input, char out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    int p = 0, name_idx = 0;
    while (input[p] && input[p] != '.' && name_idx < 8) {
        char c = input[p];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[name_idx++] = c;
        p++;
    }
    if (input[p] == '.') p++;
    int ext = 8;
    while (input[p] && ext < 11) {
        char c = input[p];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[ext++] = c;
        p++;
    }
}

/* 核心修复：查找文件优先使用 current_dir_cluster */
static int find_file(const char *name, unsigned int *cluster, unsigned int *size) {
    char fat_name[11];
    make_83_name(name, fat_name);

    unsigned char *cluster_buffer = (unsigned char *)0x40000;
    unsigned int cur_cluster = current_dir_cluster ? current_dir_cluster : root_cluster;
    unsigned int entries_per_cluster = (sectors_per_cluster * bytes_per_sector) / sizeof(FAT32_DirEntry);

    while (cur_cluster >= 2 && cur_cluster < 0x0FFFFFF8) {
        if (read_cluster(cur_cluster, cluster_buffer) != 0) return -1;
        FAT32_DirEntry *dir = (FAT32_DirEntry *)cluster_buffer;
        for (unsigned int i = 0; i < entries_per_cluster; i++) {
            if (dir[i].name[0] == 0) return -1;
            if (dir[i].name[0] == 0xE5) continue;
            if (dir[i].attributes == 0x0F) continue;
            if (dir[i].attributes & 0x08) continue;

            int match = 1;
            for (int j = 0; j < 11; j++) {
                if (dir[i].name[j] != fat_name[j]) { match = 0; break; }
            }
            if (match) {
                *cluster = ((unsigned int)dir[i].first_cluster_high << 16) | dir[i].first_cluster_low;
                *size = dir[i].file_size;
                return 0;
            }
        }
        cur_cluster = read_fat_entry(cur_cluster);
    }
    return -1;
}

static int update_file_size(const char *filename, unsigned int new_size) {
    char fat_name[11];
    make_83_name(filename, fat_name);

    unsigned int cluster = current_dir_cluster ? current_dir_cluster : root_cluster;
    unsigned int entries_per_cluster = (sectors_per_cluster * bytes_per_sector) / sizeof(FAT32_DirEntry);
    unsigned char *dir_buf = (unsigned char *)0x44000;

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        if (read_cluster(cluster, dir_buf) != 0) return -4;
        FAT32_DirEntry *entries = (FAT32_DirEntry *)dir_buf;
        for (unsigned int i = 0; i < entries_per_cluster; i++) {
            if (entries[i].name[0] == 0) break;
            int match = 1;
            for (int m = 0; m < 11; m++) {
                if (entries[i].name[m] != fat_name[m]) { match = 0; break; }
            }
            if (match) {
                entries[i].file_size = new_size;
                if (ide_write_sectors(cluster_to_lba(cluster), sectors_per_cluster, dir_buf) != 0)
                    return -5;
                return new_size;
            }
        }
        cluster = read_fat_entry(cluster);
    }
    return -6;
}

int fat32_init(unsigned int partition_start_lba) {
    unsigned char sector[512];
    part_start_lba = partition_start_lba;

    if (ide_read_sectors(partition_start_lba, 1, sector) != 0) return -1;
    if (sector[0] != 0xEB || sector[1] != 0x58 || sector[2] != 0x90) return -4;

    FAT32_BPB *pbpb = (FAT32_BPB *)sector;
    if (pbpb->bytes_per_sector != 512) return -2;
    if (pbpb->sectors_per_cluster == 0) return -3;

    unsigned char *src = sector;
    unsigned char *dst = (unsigned char *)&bpb;
    for (int i = 0; i < sizeof(FAT32_BPB); i++) dst[i] = src[i];

    bytes_per_sector = bpb.bytes_per_sector;
    sectors_per_cluster = bpb.sectors_per_cluster;
    fat_start = partition_start_lba + bpb.reserved_sectors;
    data_start = fat_start + bpb.num_fats * bpb.sectors_per_fat_32;
    root_cluster = bpb.root_cluster;

    current_dir_cluster = 0; // 重置工作目录为根目录
    current_path[0] = '/';
    current_path[1] = '\0';

    return 0;
}

void fat32_list_root() {
    unsigned char *cluster_buffer = (unsigned char *)0x40000;
    unsigned int cluster = root_cluster;
    unsigned int entries_per_cluster = (sectors_per_cluster * bytes_per_sector) / sizeof(FAT32_DirEntry);

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        if (read_cluster(cluster, cluster_buffer) != 0) return;

        FAT32_DirEntry *dir = (FAT32_DirEntry *)cluster_buffer;
        for (unsigned int i = 0; i < entries_per_cluster; i++) {
            unsigned char first = dir[i].name[0];
            if (first == 0x00) break;
            if (first == 0xE5) continue;
            if (dir[i].attributes == 0x0F) continue;
            if (dir[i].attributes & 0x08) continue;

            for (int j = 0; j < 8 && dir[i].name[j] != ' '; j++) putchar(dir[i].name[j]);
            if (dir[i].name[8] != ' ') {
                putchar('.');
                for (int j = 8; j < 11 && dir[i].name[j] != ' '; j++) putchar(dir[i].name[j]);
            }

            print("   ");
            unsigned int sz = dir[i].file_size;
            char buf[12]; int pos = 0;
            if (sz == 0) buf[pos++] = '0';
            else { while (sz) { buf[pos++] = '0' + (sz % 10); sz /= 10; } }
            while (pos) putchar(buf[--pos]);
            print(" bytes\n");
        }
        cluster = read_fat_entry(cluster);
    }
}

int fat32_read_file_content(const char *filename, char *buffer, unsigned int max_size) {
    unsigned int start_cluster, file_size;
    if (find_file(filename, &start_cluster, &file_size) != 0) return -1;

    unsigned int bytes_read = 0;
    unsigned int cur_cluster = start_cluster;
    unsigned char *tmp_buf = (unsigned char *)0x42000;

    while (bytes_read < file_size && cur_cluster >= 2 && cur_cluster < 0x0FFFFFF8) {
        if (read_cluster(cur_cluster, tmp_buf) != 0) return -1;
        unsigned int to_copy = bytes_per_sector * sectors_per_cluster;
        if (bytes_read + to_copy > file_size) to_copy = file_size - bytes_read;
        if (bytes_read + to_copy > max_size) to_copy = max_size - bytes_read;
        for (unsigned int i = 0; i < to_copy; i++) buffer[bytes_read + i] = tmp_buf[i];
        bytes_read += to_copy;
        if (bytes_read >= file_size || bytes_read >= max_size) break;
        cur_cluster = read_fat_entry(cur_cluster);
    }
    return bytes_read;
}

int fat32_overwrite_file(const char *filename, const char *data, unsigned int size) {
    unsigned int start_cluster, old_size;
    if (find_file(filename, &start_cluster, &old_size) != 0) return -1;

    if (size == 0) return update_file_size(filename, 0);

    unsigned int max_bytes_per_cluster = sectors_per_cluster * bytes_per_sector;
    unsigned int bytes_written = 0;
    unsigned char *tmp_buf = (unsigned char *)0x42000;

    unsigned int needed_clusters = (size + max_bytes_per_cluster - 1) / max_bytes_per_cluster;
    unsigned int clusters[1024];
    unsigned int cluster_count = 0;
    unsigned int c = start_cluster;
    while (c >= 2 && c < 0x0FFFFFF8 && cluster_count < 1024) {
        clusters[cluster_count++] = c;
        c = read_fat_entry(c);
    }

    while (cluster_count < needed_clusters) {
        unsigned int new_cluster = 0;
        for (unsigned int cl = 2; cl < 0x0FFFFFF0; cl++) {
            if (read_fat_entry(cl) == 0) { new_cluster = cl; break; }
        }
        if (!new_cluster) return -7;
        unsigned int last_cluster = clusters[cluster_count - 1];
        if (write_fat_entry(last_cluster, new_cluster) != 0) return -8;
        if (write_fat_entry(new_cluster, 0x0FFFFFFF) != 0) return -8;
        clusters[cluster_count++] = new_cluster;
        unsigned char *empty_buf = (unsigned char *)0x46000;
        for (int i = 0; i < max_bytes_per_cluster; i++) empty_buf[i] = 0;
        if (ide_write_sectors(cluster_to_lba(new_cluster), sectors_per_cluster, empty_buf) != 0)
            return -9;
    }

    for (unsigned int i = 0; i < cluster_count && bytes_written < size; i++) {
        unsigned int cur_cluster = clusters[i];
        unsigned int to_copy = max_bytes_per_cluster;
        if (bytes_written + to_copy > size) to_copy = size - bytes_written;
        if (read_cluster(cur_cluster, tmp_buf) != 0) return -2;
        for (unsigned int j = 0; j < to_copy; j++) tmp_buf[j] = data[bytes_written + j];
        for (unsigned int j = to_copy; j < max_bytes_per_cluster; j++) tmp_buf[j] = 0;
        if (ide_write_sectors(cluster_to_lba(cur_cluster), sectors_per_cluster, tmp_buf) != 0)
            return -3;
        bytes_written += to_copy;
    }
    return update_file_size(filename, bytes_written);
}

/* 核心修复：在当前目录中新建文件 */
int fat32_create_file(const char *name) {
    char fat_name[11];
    make_83_name(name, fat_name);

    unsigned int free_cluster = 0;
    for (unsigned int c = 2; c < 0x0FFFFFF0; c++) {
        if (read_fat_entry(c) == 0) { free_cluster = c; break; }
        if (c > 1000) break;
    }
    if (!free_cluster) return -3;

    unsigned char *cluster_buf = (unsigned char *)0x40000;
    unsigned int cluster = current_dir_cluster ? current_dir_cluster : root_cluster;
    unsigned int entries_per_cluster = (sectors_per_cluster * bytes_per_sector) / sizeof(FAT32_DirEntry);
    int found = 0;

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        if (read_cluster(cluster, cluster_buf) != 0) return -1;
        FAT32_DirEntry *dir = (FAT32_DirEntry *)cluster_buf;
        for (unsigned int k = 0; k < entries_per_cluster; k++) {
            if (dir[k].name[0] == 0 || dir[k].name[0] == 0xE5) {
                unsigned char *entry = (unsigned char *)&dir[k];
                for (int m = 0; m < sizeof(FAT32_DirEntry); m++) entry[m] = 0;
                for (int m = 0; m < 11; m++) dir[k].name[m] = fat_name[m];
                dir[k].attributes = 0x20;
                dir[k].file_size = 0;
                dir[k].first_cluster_low = free_cluster & 0xFFFF;
                dir[k].first_cluster_high = (free_cluster >> 16) & 0xFFFF;
                if (ide_write_sectors(cluster_to_lba(cluster), sectors_per_cluster, cluster_buf) != 0)
                    return -2;
                found = 1;
                break;
            }
        }
        if (found) break;
        cluster = read_fat_entry(cluster);
    }
    if (!found) return -1;

    if (write_fat_entry(free_cluster, 0x0FFFFFFF) != 0) return -2;

    unsigned char *empty_buf = (unsigned char *)0x45000;
    for (int i = 0; i < sectors_per_cluster * bytes_per_sector; i++) empty_buf[i] = 0;
    if (ide_write_sectors(cluster_to_lba(free_cluster), sectors_per_cluster, empty_buf) != 0)
        return -2;
    return 0;
}

/* 核心修复：在当前目录中删除文件 */
int fat32_delete_file(const char *filename) {
    unsigned int start_cluster, file_size;
    if (find_file(filename, &start_cluster, &file_size) != 0) return -1;

    char fat_name[11];
    make_83_name(filename, fat_name);

    unsigned int cluster = current_dir_cluster ? current_dir_cluster : root_cluster;
    unsigned int entries_per_cluster = (sectors_per_cluster * bytes_per_sector) / sizeof(FAT32_DirEntry);
    unsigned char *dir_buf = (unsigned char *)0x40000;

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        if (read_cluster(cluster, dir_buf) != 0) return -2;
        FAT32_DirEntry *entries = (FAT32_DirEntry *)dir_buf;
        for (unsigned int i = 0; i < entries_per_cluster; i++) {
            if (entries[i].name[0] == 0) continue;
            int match = 1;
            for (int m = 0; m < 11; m++) {
                if (entries[i].name[m] != fat_name[m]) { match = 0; break; }
            }
            if (match) {
                entries[i].name[0] = 0xE5;
                for (int m = 1; m < 11; m++) entries[i].name[m] = ' ';
                entries[i].file_size = 0;
                entries[i].first_cluster_low = 0;
                entries[i].first_cluster_high = 0;
                if (ide_write_sectors(cluster_to_lba(cluster), sectors_per_cluster, dir_buf) != 0)
                    return -3;
                unsigned int cur_cluster = start_cluster;
                while (cur_cluster >= 2 && cur_cluster < 0x0FFFFFF8) {
                    unsigned int next_cluster = read_fat_entry(cur_cluster);
                    if (write_fat_entry(cur_cluster, 0) != 0) return -4;
                    cur_cluster = next_cluster;
                }
                return 0;
            }
        }
        cluster = read_fat_entry(cluster);
    }
    return -1;
}

static unsigned int get_entry_cluster(FAT32_DirEntry *entry) {
    return ((unsigned int)entry->first_cluster_high << 16) | entry->first_cluster_low;
}

static int find_in_directory(unsigned int dir_cluster, const char *name, FAT32_DirEntry *found_entry) {
    char fat_name[11];
    make_83_name(name, fat_name);

    unsigned char *cluster_buf = (unsigned char *)0x40000;
    unsigned int cluster = dir_cluster ? dir_cluster : root_cluster;
    unsigned int entries_per_cluster = (sectors_per_cluster * bytes_per_sector) / sizeof(FAT32_DirEntry);

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        if (read_cluster(cluster, cluster_buf) != 0) return -1;
        FAT32_DirEntry *dir = (FAT32_DirEntry *)cluster_buf;
        
        for (unsigned int i = 0; i < entries_per_cluster; i++) {
            if (dir[i].name[0] == 0) return -1;
            if (dir[i].name[0] == 0xE5) continue;
            if (dir[i].attributes == ATTR_LONG_NAME) continue;
            if (dir[i].attributes & ATTR_VOLUME_ID) continue;

            int match = 1;
            for (int j = 0; j < 11; j++) {
                if (dir[i].name[j] != fat_name[j]) { match = 0; break; }
            }
            if (match) {
                if (found_entry) *found_entry = dir[i];
                return 0;
            }
        }
        cluster = read_fat_entry(cluster);
    }
    return -1;
}

static unsigned int get_current_cluster() {
    return current_dir_cluster;
}

static unsigned int resolve_path(const char *path) {
    if (!path || path[0] == '\0') return get_current_cluster();

    unsigned int current_cluster = (path[0] == '/') ? 0 : get_current_cluster();
    char temp_path[256];
    
    int i = 0;
    while (path[i] && i < 255) { temp_path[i] = path[i]; i++; }
    temp_path[i] = '\0';
    
    char *token = temp_path;
    if (token[0] == '/') token++;
    
    while (*token) {
        char *next = token;
        while (*next && *next != '/') next++;
        char old_char = *next;
        *next = '\0';
        
        if (token[0] == '.' && token[1] == '.' && token[2] == '\0') {
            if (current_cluster != 0) {
                unsigned char *cluster_buf = (unsigned char *)0x40000;
                if (read_cluster(current_cluster, cluster_buf) != 0) return 0xFFFFFFFF;
                FAT32_DirEntry *dir = (FAT32_DirEntry *)cluster_buf;
                if (dir[1].name[0] != 0xE5 && dir[1].name[0] != 0) {
                    current_cluster = get_entry_cluster(&dir[1]);
                } else {
                    current_cluster = 0;
                }
            }
        } else if (token[0] == '.' && token[1] == '\0') {
            // 当前目录
        } else {
            FAT32_DirEntry entry;
            if (find_in_directory(current_cluster, token, &entry) != 0) return 0xFFFFFFFF;
            if (!(entry.attributes & ATTR_DIRECTORY)) return 0xFFFFFFFF;
            current_cluster = get_entry_cluster(&entry);
        }
        
        if (old_char == '\0') break;
        token = next + 1;
    }
    return current_cluster;
}

void fat32_get_current_path(char *buffer, int size) {
    if (current_dir_cluster == 0) {
        buffer[0] = '/';
        buffer[1] = '\0';
        return;
    }
    int len = 0;
    const char *src = current_path;
    while (*src && len < size - 1) buffer[len++] = *src++;
    buffer[len] = '\0';
}

int fat32_change_directory(const char *path) {
    if (!path || path[0] == '\0') return 0;
    unsigned int target_cluster = resolve_path(path);
    if (target_cluster == 0xFFFFFFFF) return -1;
    
    current_dir_cluster = target_cluster;
    
    if (target_cluster == 0) {
        current_path[0] = '/';
        current_path[1] = '\0';
    } else if (path[0] == '/') {
        int len = 0;
        while (path[len] && len < (int)sizeof(current_path) - 1) {
            current_path[len] = path[len];
            len++;
        }
        current_path[len] = '\0';
    } else if (path[0] == '.' && path[1] == '.' && path[2] == '\0') {
        int len = 0;
        while (current_path[len]) len++;
        while (len > 1 && current_path[len - 1] != '/') len--;
        if (len > 1) {
            current_path[len - 1] = '\0';
        } else {
            current_path[0] = '/';
            current_path[1] = '\0';
        }
    } else {
        int len = 0;
        while (current_path[len]) len++;
        if (len > 1 && current_path[len - 1] != '/' &&
            len < (int)sizeof(current_path) - 1) {
            current_path[len++] = '/';
        }
        const char *src = path;
        while (*src && len < (int)sizeof(current_path) - 1) current_path[len++] = *src++;
        current_path[len] = '\0';
    }
    return 0;
}

void fat32_list_current_directory() {
    unsigned char *cluster_buf = (unsigned char *)0x40000;
    unsigned int cluster = current_dir_cluster ? current_dir_cluster : root_cluster;
    unsigned int entries_per_cluster = (sectors_per_cluster * bytes_per_sector) / sizeof(FAT32_DirEntry);
    int has_files = 0;

    print("Directory: ");
    char path_str[256];
    fat32_get_current_path(path_str, sizeof(path_str));
    print(path_str);
    print("\n----------------------------------------\n");

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        if (read_cluster(cluster, cluster_buf) != 0) {
            print("Error reading directory\n");
            return;
        }
        FAT32_DirEntry *dir = (FAT32_DirEntry *)cluster_buf;
        
        for (unsigned int i = 0; i < entries_per_cluster; i++) {
            unsigned char first = dir[i].name[0];
            if (first == 0x00) break;
            if (first == 0xE5) continue;
            if (dir[i].attributes == ATTR_LONG_NAME) continue;
            if (dir[i].attributes & ATTR_VOLUME_ID) continue;

            has_files = 1;
            if (dir[i].attributes & ATTR_DIRECTORY) {
                set_color(0x0B, 0x00);
                print("[DIR]  ");
            } else {
                set_color(0x0F, 0x00);
                print("[FILE] ");
            }
            
            for (int j = 0; j < 8 && dir[i].name[j] != ' '; j++) putchar(dir[i].name[j]);
            if (dir[i].name[8] != ' ') {
                putchar('.');
                for (int j = 8; j < 11 && dir[i].name[j] != ' '; j++) putchar(dir[i].name[j]);
            }
            
            if (!(dir[i].attributes & ATTR_DIRECTORY)) {
                print(" (");
                unsigned int sz = dir[i].file_size;
                char buf[12]; int pos = 0;
                if (sz == 0) buf[pos++] = '0';
                else { while (sz) { buf[pos++] = '0' + (sz % 10); sz /= 10; } }
                while (pos) putchar(buf[--pos]);
                print(" bytes)");
            }
            set_color(0x07, 0x00);
            print("\n");
        }
        cluster = read_fat_entry(cluster);
    }
    
    if (!has_files) print("(empty)\n");
    print("----------------------------------------\n");
}

int fat32_create_directory(const char *name) {
    char fat_name[11];
    make_83_name(name, fat_name);

    FAT32_DirEntry existing;
    if (find_in_directory(get_current_cluster(), name, &existing) == 0) {
        print("Directory already exists\n");
        return -4;
    }

    unsigned int free_cluster = 0;
    for (unsigned int c = 2; c < 0x0FFFFFF0; c++) {
        if (read_fat_entry(c) == 0) { free_cluster = c; break; }
        if (c > 1000) break;
    }
    if (!free_cluster) return -3;

    unsigned char *cluster_buf = (unsigned char *)0x40000;
    unsigned int cluster = get_current_cluster() ? get_current_cluster() : root_cluster;
    unsigned int entries_per_cluster = (sectors_per_cluster * bytes_per_sector) / sizeof(FAT32_DirEntry);
    int found = 0;

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        if (read_cluster(cluster, cluster_buf) != 0) return -1;
        FAT32_DirEntry *dir = (FAT32_DirEntry *)cluster_buf;
        for (unsigned int k = 0; k < entries_per_cluster; k++) {
            if (dir[k].name[0] == 0 || dir[k].name[0] == 0xE5) {
                unsigned char *entry = (unsigned char *)&dir[k];
                for (int m = 0; m < sizeof(FAT32_DirEntry); m++) entry[m] = 0;
                for (int m = 0; m < 11; m++) dir[k].name[m] = fat_name[m];
                dir[k].attributes = ATTR_DIRECTORY;
                dir[k].file_size = 0;
                dir[k].first_cluster_low = free_cluster & 0xFFFF;
                dir[k].first_cluster_high = (free_cluster >> 16) & 0xFFFF;

                if (ide_write_sectors(cluster_to_lba(cluster), sectors_per_cluster, cluster_buf) != 0)
                    return -2;
                found = 1;
                break;
            }
        }
        if (found) break;
        cluster = read_fat_entry(cluster);
    }
    if (!found) return -1;

    if (write_fat_entry(free_cluster, 0x0FFFFFFF) != 0) return -2;

    unsigned char *new_dir_buf = (unsigned char *)0x45000;
    for (int i = 0; i < sectors_per_cluster * bytes_per_sector; i++) new_dir_buf[i] = 0;
    
    FAT32_DirEntry *new_dir = (FAT32_DirEntry *)new_dir_buf;
    
    for (int i = 0; i < 11; i++) new_dir[0].name[i] = ' ';
    new_dir[0].name[0] = '.';
    new_dir[0].attributes = ATTR_DIRECTORY;
    new_dir[0].first_cluster_low = free_cluster & 0xFFFF;
    new_dir[0].first_cluster_high = (free_cluster >> 16) & 0xFFFF;
    
    for (int i = 0; i < 11; i++) new_dir[1].name[i] = ' ';
    new_dir[1].name[0] = '.';
    new_dir[1].name[1] = '.';
    new_dir[1].attributes = ATTR_DIRECTORY;
    unsigned int parent_cluster = get_current_cluster() ? get_current_cluster() : root_cluster;
    new_dir[1].first_cluster_low = parent_cluster & 0xFFFF;
    new_dir[1].first_cluster_high = (parent_cluster >> 16) & 0xFFFF;

    if (ide_write_sectors(cluster_to_lba(free_cluster), sectors_per_cluster, new_dir_buf) != 0)
        return -2;

    print("Directory created: ");
    print(name);
    print("\n");
    return 0;
}