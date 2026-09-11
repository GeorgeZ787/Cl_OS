/* kernel/kernel.c - Cl_OS Final GUI & Advanced Shell Integrated */
#include "fat12.h"
#include "ide.h"
#include "fat32.h"
#include "process.h"
#include "program.h"

#define NULL ((void*)0)
#define PROC_NAME_LEN 32
#define MAX_PROCESSES 16

static unsigned char vga_attr = 0x07;
#define HEAP_START 0x100000
static unsigned int heap_ptr = HEAP_START;
static unsigned int first_part_lba = 0;
static int fat32_mounted = 0;       /* 挂载标志 */
extern void syscall_handler();

/* 内联 I/O 函数 */
static inline unsigned char inb(unsigned short port) {
    unsigned char ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outb(unsigned short port, unsigned char data) {
    __asm__ volatile ("outb %0, %1" :: "a"(data), "Nd"(port));
}
static inline void outw(unsigned short port, unsigned short data) {
    __asm__ volatile ("outw %0, %1" :: "a"(data), "Nd"(port));
}

static void uppercase(char *str) {
    for (; *str; str++) {
        if (*str >= 'a' && *str <= 'z') *str -= 32;
    }
}

static int hex_to_int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* ================= 进程与内存管理 ================= */
struct mem_header {
    unsigned int size;      
    int is_free;            
    struct mem_header *next;
};

struct idt_entry {
    unsigned short offset_low;
    unsigned short selector;
    unsigned char zero;
    unsigned char type_attr;
    unsigned short offset_high;
} __attribute__((packed));

struct idt_ptr {
    unsigned short limit;
    unsigned int base;
} __attribute__((packed));

struct idt_entry idt[256];
struct idt_ptr idtp;

static struct mem_header *heap_head = NULL;

void init_heap() {
    heap_head = (struct mem_header *)HEAP_START;
    heap_head->size = 0x100000; 
    heap_head->is_free = 1;
    heap_head->next = NULL;
}

/* =============== VGA 文本驱动 =============== */
#define VGA_BASE 0xB8000
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define vga_buf ((unsigned char *)VGA_BASE)
static int vga_col = 0, vga_row = 0;

/* Keep a bounded line history so the CLI can move its viewport without
 * changing the behavior of programs that draw directly to VGA memory. */
#define TERMINAL_HISTORY_LINES 256
static unsigned short terminal_history[TERMINAL_HISTORY_LINES][VGA_WIDTH];
static int terminal_line = 0;
static int terminal_view = 0;
static int clock_process_pid = -1;
static unsigned char clock_last_second = 0xFF;

static unsigned char rtc_read(unsigned char reg);
static unsigned char rtc_bcd_to_binary(unsigned char value);
static void clock_update(void);

static void move_cursor() {
    unsigned short pos = vga_row * VGA_WIDTH + vga_col;
    outb(0x3D4, 0x0F); outb(0x3D5, (unsigned char)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (unsigned char)((pos >> 8) & 0xFF));
}

static void terminal_fill_line(int line) {
    for (int x = 0; x < VGA_WIDTH; x++)
        terminal_history[line][x] = (unsigned short)(vga_attr << 8) | ' ';
}

static void terminal_render(void) {
    unsigned short *screen = (unsigned short *)VGA_BASE;
    for (int y = 0; y < VGA_HEIGHT; y++) {
        int line = terminal_view + y;
        for (int x = 0; x < VGA_WIDTH; x++) {
            screen[y * VGA_WIDTH + x] =
                (line < TERMINAL_HISTORY_LINES) ? terminal_history[line][x] :
                (unsigned short)(vga_attr << 8) | ' ';
        }
    }
    clock_update();
    vga_row = terminal_line - terminal_view;
    move_cursor();
}

static void terminal_follow_bottom(void) {
    int bottom = terminal_line - VGA_HEIGHT + 1;
    if (bottom < 0) bottom = 0;
    terminal_view = bottom;
}

static void terminal_newline(void) {
    terminal_line++;
    if (terminal_line >= TERMINAL_HISTORY_LINES) {
        for (int y = 1; y < TERMINAL_HISTORY_LINES; y++)
            for (int x = 0; x < VGA_WIDTH; x++)
                terminal_history[y - 1][x] = terminal_history[y][x];
        terminal_line = TERMINAL_HISTORY_LINES - 1;
    }
    terminal_fill_line(terminal_line);
}

static void terminal_scroll(int direction) {
    int max_view = terminal_line - VGA_HEIGHT + 1;
    if (max_view < 0) max_view = 0;
    if (direction < 0 && terminal_view > 0) terminal_view--;
    if (direction > 0 && terminal_view < max_view) terminal_view++;
    terminal_render();
}

static void clock_stop(void) {
    if (clock_process_pid >= 0) {
        process_kill((unsigned int)clock_process_pid);
        clock_process_pid = -1;
    }
    clock_last_second = 0xFF;
}

static void clock_write_cell(int row, int col, char value) {
    ((unsigned short *)VGA_BASE)[row * VGA_WIDTH + col] =
        (unsigned short)(vga_attr << 8) | (unsigned char)value;
}

static void clock_update(void) {
    if (clock_process_pid < 0) return;

    unsigned char second = rtc_read(0x00);
    if (second == clock_last_second) return;
    clock_last_second = second;

    unsigned char minute = rtc_read(0x02);
    unsigned char hour = rtc_read(0x04);
    unsigned char day = rtc_read(0x07);
    unsigned char month = rtc_read(0x08);
    unsigned char year = rtc_read(0x09);
    unsigned char status_b = rtc_read(0x0B);

    if (!(status_b & 0x04)) {
        second = rtc_bcd_to_binary(second);
        minute = rtc_bcd_to_binary(minute);
        hour = rtc_bcd_to_binary(hour & 0x7F);
        day = rtc_bcd_to_binary(day);
        month = rtc_bcd_to_binary(month);
        year = rtc_bcd_to_binary(year);
    }
    if (!(status_b & 0x02)) {
        if (hour & 0x80) hour = (unsigned char)(((hour & 0x7F) + 12) % 24);
        else hour &= 0x7F;
    }

    char text[12] = {
        (char)('0' + hour / 10), (char)('0' + hour % 10), ':',
        (char)('0' + minute / 10), (char)('0' + minute % 10), ':',
        (char)('0' + second / 10), (char)('0' + second % 10),
        ' ', 'U', 'T', 'C'
    };
    char date[10] = {
        '2', '0', (char)('0' + year / 10), (char)('0' + year % 10), '-',
        (char)('0' + month / 10), (char)('0' + month % 10), '-',
        (char)('0' + day / 10), (char)('0' + day % 10)
    };
    for (int i = 0; i < 12; i++) clock_write_cell(VGA_HEIGHT - 1, 68 + i, text[i]);
    for (int i = 0; i < 10; i++) clock_write_cell(VGA_HEIGHT - 2, 70 + i, date[i]);
}

void putchar(char c) {
    terminal_follow_bottom();
    if (c == '\n') { vga_col = 0; vga_row++; }
    else if (c == '\r') vga_col = 0;
    else if (c == '\b') {
        if (vga_col > 0) {
            vga_col--;
            terminal_history[terminal_line][vga_col] = (unsigned short)(vga_attr << 8) | ' ';
        }
    } else {
        terminal_history[terminal_line][vga_col] = (unsigned short)(vga_attr << 8) | (unsigned char)c;
        vga_col++;
    }
    if (vga_col >= VGA_WIDTH) { vga_col = 0; terminal_newline(); }
    if (c == '\n') terminal_newline();
    terminal_follow_bottom();
    terminal_render();
}

void print(const char *str) { while (*str) putchar(*str++); }

void clear_screen() {
    vga_col = vga_row = 0;
    terminal_line = terminal_view = 0;
    for (int line = 0; line < TERMINAL_HISTORY_LINES; line++)
        terminal_fill_line(line);
    terminal_render();
}

void set_color(unsigned char fg, unsigned char bg) { vga_attr = (bg << 4) | (fg & 0x0F); }

/* =============== 键盘输入与 Shift 状态机 =============== */
#define KEYBOARD_PORT 0x60
#define KEYBOARD_STATUS 0x64

static int shift_pressed = 0; 
static int caps_lock = 0;
static char kbd_us_normal[] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0
};
static char kbd_us_shift[] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '\"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0
};

static unsigned char scancode_to_ascii(unsigned char sc) {
    if (sc >= sizeof(kbd_us_normal)) return 0;
    char normal = kbd_us_normal[sc];
    if (normal >= 'a' && normal <= 'z') {
        if (shift_pressed != caps_lock) normal -= 32;
        return normal;
    }
    if (shift_pressed && sc < sizeof(kbd_us_shift)) return kbd_us_shift[sc];
    return normal;
}

static void drain_input_buffer(void) {
    int timeout = 10000;
    while (timeout-- > 0) {
        if (!(inb(KEYBOARD_STATUS) & 1)) return;
        inb(KEYBOARD_PORT);
    }
}

static int getchar() {
    unsigned char sc;
    int extended = 0;
    while (1) {
        clock_update();
        unsigned char status = inb(0x64);
        
        if (status & 1) {
            sc = inb(0x60);
            
            // Shift 键处理
            if (sc == 0x2A || sc == 0x36) { shift_pressed = 1; continue; }
            if (sc == 0xAA || sc == 0xB6) { shift_pressed = 0; continue; }
            if (sc == 0x3A) { caps_lock = !caps_lock; continue; }
            if (sc == 0x3A) { caps_lock = !caps_lock; continue; }
            if (sc == 0xE0) { extended = 1; continue; }
            if (extended) {
                extended = 0;
                if (sc == 0x48) return 0x100 + 'U';
                if (sc == 0x50) return 0x100 + 'D';
                continue;
            }
            
            // 只处理按下事件
            if (!(sc & 0x80)) {
                char ascii = scancode_to_ascii(sc);
                if (ascii) return ascii;
            }
        }
    }
}

static int extended_key = 0;
static int getkey() {
    while (1) {
        unsigned char status;
        while (!((status = inb(0x64)) & 1));
        if (status & 0x20) { inb(0x60); continue; }
        int sc = inb(0x60);
        if (sc == 0x2A || sc == 0x36) { shift_pressed = 1; continue; }
        if (sc == 0xAA || sc == 0xB6) { shift_pressed = 0; continue; }
        if (sc == 0xE0) { extended_key = 1; continue; }
        if (extended_key) {
            extended_key = 0;
            if (sc == 0x48) return 0x100 + 'U'; if (sc == 0x50) return 0x100 + 'D';
            if (sc == 0x4B) return 0x100 + 'L'; if (sc == 0x4D) return 0x100 + 'R'; return 0;
        }
        if (!(sc & 0x80)) {
            if (sc == 0x01) return 27;  if (sc == 0x3C) return 0x200 + 'F'; 
            unsigned char ascii = scancode_to_ascii(sc); if (ascii) return ascii;
        }
    }
}

static void readline(char *buf, int max_len) {
    int i = 0;
    while (i < max_len - 1) {
        int key = getchar();
        char c = (char)key;
        if (c == '\n') { putchar('\n'); break; }
        else if (c == '\b') { if (i>0) { putchar('\b'); i--; } }
        else if (c == '\t') {
            for (int spaces = 0; spaces < 4 && i < max_len - 1; spaces++) {
                putchar(' ');
                buf[i++] = ' ';
            }
        }
        else { putchar(c); buf[i++] = c; }
    }
    buf[i] = '\0';
}

/* =============== 高级定点数计算引擎 =============== */
void print_int(int val) {
    if (val == 0) { putchar('0'); return; }
    if (val < 0) { putchar('-'); val = -val; }
    char buf[16]; int i = 0;
    while (val > 0) { buf[i++] = (val % 10) + '0'; val /= 10; }
    while (i > 0) putchar(buf[--i]);
}

int parse_fixed(char **str) {
    int res = 0; int sign = 1; char *s = *str;
    while (*s == ' ') s++;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') { res = res * 10 + (*s - '0'); s++; }
    res *= 1000; 
    if (*s == '.') {
        s++;
        int frac = 0; int weight = 100;
        while (*s >= '0' && *s <= '9' && weight > 0) {
            frac += (*s - '0') * weight;
            weight /= 10; s++;
        }
        while (*s >= '0' && *s <= '9') s++;
        res += frac;
    }
    *str = s; return res * sign;
}

void print_fixed(int val) {
    if (val < 0) { putchar('-'); val = -val; }
    int i_part = val / 1000;
    int f_part = val % 1000;
    print_int(i_part);
    putchar('.');
    putchar((f_part / 100) + '0');
    putchar(((f_part / 10) % 10) + '0');
    putchar((f_part % 10) + '0');
}

void calc_eval(char *expr) {
    char *p = expr;
    int a = parse_fixed(&p);
    while (*p == ' ') p++;
    if (*p == '\0') return;
    char op = *p++;
    int b = parse_fixed(&p);

    int res = 0;
    if (op == '+') res = a + b;
    else if (op == '-') res = a - b;
    else if (op == '*') {
        int abs_a = (a < 0) ? -a : a;
        int abs_b = (b < 0) ? -b : b;
        int sign = ((a < 0) ^ (b < 0)) ? -1 : 1;
        int iA = abs_a/1000, fA = abs_a%1000;
        int iB = abs_b/1000, fB = abs_b%1000;
        int abs_res = iA*iB*1000 + iA*fB + iB*fA + (fA*fB)/1000;
        res = (sign == -1) ? -abs_res : abs_res;
    }
    else if (op == '/') {
        if (b == 0) { print("Error: Division by zero\n"); return; }
        int abs_a = (a < 0) ? -a : a;
        int abs_b = (b < 0) ? -b : b;
        int sign = ((a < 0) ^ (b < 0)) ? -1 : 1;
        int iA = abs_a / 1000;
        int fA = abs_a % 1000;
        int abs_res = (iA * 1000000) / abs_b + (fA * 1000) / abs_b;
        res = (sign == -1) ? -abs_res : abs_res;
    } else {
        print("Error: Unknown operator\n"); return;
    }
    print("Result: "); print_fixed(res); print("\n");
}

static unsigned char rtc_bcd_to_binary(unsigned char value) {
    return (unsigned char)((value & 0x0F) + ((value >> 4) * 10));
}

static unsigned char rtc_read(unsigned char reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static void print_two_digits(unsigned char value) {
    putchar((value / 10) + '0');
    putchar((value % 10) + '0');
}

static void print_current_time(void) {
    unsigned char status_b;
    unsigned char second;
    unsigned char minute;
    unsigned char hour;
    unsigned char day;
    unsigned char month;
    unsigned char year;

    do {
        while (rtc_read(0x0A) & 0x80) {}
        second = rtc_read(0x00);
        minute = rtc_read(0x02);
        hour = rtc_read(0x04);
        day = rtc_read(0x07);
        month = rtc_read(0x08);
        year = rtc_read(0x09);
        status_b = rtc_read(0x0B);
    } while (second != rtc_read(0x00));

    if (!(status_b & 0x04)) {
        second = rtc_bcd_to_binary(second);
        minute = rtc_bcd_to_binary(minute);
        hour = (unsigned char)((hour & 0x80) | rtc_bcd_to_binary(hour & 0x7F));
        day = rtc_bcd_to_binary(day);
        month = rtc_bcd_to_binary(month);
        year = rtc_bcd_to_binary(year);
    }
    if (!(status_b & 0x02) && (hour & 0x80)) {
        hour = (unsigned char)(((hour & 0x7F) + 12) % 24);
    } else {
        hour &= 0x7F;
    }

    print("Date: 20");
    print_two_digits(year);
    putchar('-');
    print_two_digits(month);
    putchar('-');
    print_two_digits(day);
    print("\nTime: ");
    print_two_digits(hour);
    putchar(':');
    print_two_digits(minute);
    putchar(':');
    print_two_digits(second);
    print("\n");
}

/* =============== 文本编辑器 =============== */
static int editor_line_start(const char *text, int pos) {
    while (pos > 0 && text[pos - 1] != '\n') pos--;
    return pos;
}

static void editor_sync_xy(const char *text, int text_len, int text_pos, int win_w, int *cx, int *cy) {
    int x = 0, y = 0;
    for (int i = 0; i < text_pos && i < text_len; i++) {
        if (text[i] == '\n') { y++; x = 0; }
        else { x++; if (x >= win_w) { y++; x = 0; } }
    }
    *cx = x;
    *cy = y;
}

static int editor_move_up(const char *text, int text_pos) {
    if (text_pos == 0) return 0;
    int col = text_pos - editor_line_start(text, text_pos);
    int line_start = editor_line_start(text, text_pos);
    if (line_start == 0) return 0;
    int prev_start = editor_line_start(text, line_start - 1);
    int prev_end = line_start - 1;
    int new_pos = prev_start + col;
    if (new_pos > prev_end) new_pos = prev_end;
    return new_pos;
}

static int editor_move_down(const char *text, int text_len, int text_pos) {
    if (text_pos >= text_len) return text_pos;
    int col = text_pos - editor_line_start(text, text_pos);
    int line_start = editor_line_start(text, text_pos);
    int line_end = line_start;
    while (line_end < text_len && text[line_end] != '\n') line_end++;
    if (line_end >= text_len) return text_pos;
    int next_start = line_end + 1;
    int next_end = next_start;
    while (next_end < text_len && text[next_end] != '\n') next_end++;
    int new_pos = next_start + col;
    if (new_pos > next_end) new_pos = next_end;
    return new_pos;
}

static void window_text_editor(const char *filename) {
    int win_x = 10, win_y = 3, win_w = 60, win_h = 15;
    unsigned short *buf = (unsigned short *)VGA_BASE;
    for (int y = win_y; y < win_y + win_h; y++)
        for (int x = win_x; x < win_x + win_w; x++) buf[y * VGA_WIDTH + x] = 0x07DB;

    char text[60*15];
    int text_len = 0, text_pos = 0, cur_x = 0, cur_y = 0;
    char open_filename[24] = "";
    
    if (filename && filename[0] != '\0') {
        for (int i = 0; i < 24 && filename[i]; i++) open_filename[i] = filename[i];
        char *filebuf = (char *)0x50000;
        int bytes = fat32_read_file_content(filename, filebuf, sizeof(text)-1);
        if (bytes > 0) { for (int i = 0; i < bytes; i++) text[i] = filebuf[i]; text_len = bytes; }
    }

    #define REDRAW_EDITOR() do { \
        editor_sync_xy(text, text_len, text_pos, win_w, &cur_x, &cur_y); \
        for (int _y = 0; _y < win_h; _y++) for (int _x = 0; _x < win_w; _x++) buf[(win_y + _y) * VGA_WIDTH + (win_x + _x)] = 0x07DB; \
        int _pos = 0; \
        for (int _line = 0; _line < win_h; _line++) { \
            for (int _col = 0; _col < win_w && _pos < text_len; _col++, _pos++) { \
                char ch = text[_pos]; if (ch == '\n') break; \
                if (ch >= 32 && ch <= 126) buf[(win_y + _line) * VGA_WIDTH + (win_x + _col)] = 0x0700 | ch; \
            } \
            while (_pos < text_len && text[_pos] == '\n') _pos++; if (_pos >= text_len) break; \
        } \
        if (cur_y < win_h) buf[(win_y + cur_y) * VGA_WIDTH + (win_x + cur_x)] = 0x0FDB; \
        if (open_filename[0]) { \
            for (int i = 0; i < win_w; i++) buf[(win_y+win_h)*VGA_WIDTH+(win_x+i)] = 0x1E00 | ' '; \
            for (int i = 0; open_filename[i]; i++) buf[(win_y+win_h)*VGA_WIDTH+(win_x+2+i)] = 0x1E00 | open_filename[i]; \
        } \
    } while (0)

    REDRAW_EDITOR();

    while (1) {
        int key = getkey();
        if (key == 27) break;  
        if (key == 0x200 + 'F') {  
            if (open_filename[0] == 0) continue;
            fat32_delete_file(open_filename);
            if (fat32_create_file(open_filename) == 0) {
                if (fat32_overwrite_file(open_filename, text, text_len) >= 0) {
                    for (int i = 0; i < win_w; i++) buf[(win_y+win_h)*VGA_WIDTH+(win_x+i)] = 0x2E00 | ' ';
                }
            }
            continue;
        }
        if (key == 0x100 + 'U') { text_pos = editor_move_up(text, text_pos); REDRAW_EDITOR(); continue; }
        if (key == 0x100 + 'D') { text_pos = editor_move_down(text, text_len, text_pos); REDRAW_EDITOR(); continue; }
        if (key == 0x100 + 'L') { if (text_pos > 0) text_pos--; REDRAW_EDITOR(); continue; }
        if (key == 0x100 + 'R') { if (text_pos < text_len) text_pos++; REDRAW_EDITOR(); continue; }
        if (key == '\b') {
            if (text_pos > 0) {
                for (int i = text_pos - 1; i < text_len - 1; i++) text[i] = text[i+1];
                text_len--;
                text_pos--;
                REDRAW_EDITOR();
            }
            continue;
        }
        if (key == '\n') {
            editor_sync_xy(text, text_len, text_pos, win_w, &cur_x, &cur_y);
            if (text_len < 60*15-1 && cur_y < win_h - 1) {
                for (int i = text_len; i > text_pos; i--) text[i] = text[i-1];
                text[text_pos] = '\n';
                text_len++;
                text_pos++;
                REDRAW_EDITOR();
            }
            continue;
        }
        if (key >= 32 && key <= 126 && text_len < 60*15-1) {
            editor_sync_xy(text, text_len, text_pos, win_w, &cur_x, &cur_y);
            if (cur_y < win_h) {
                for (int i = text_len; i > text_pos; i--) text[i] = text[i-1];
                text[text_pos] = (char)key;
                text_len++;
                text_pos++;
                REDRAW_EDITOR();
            }
        }
    }
}

/* =============== GUI 与底层 =============== */
static unsigned short g_buf[100 * 40];

static void mouse_wait(int a_type) {
    int timeout = 100000;
    if (a_type == 0) { while (timeout--) if ((inb(0x64) & 2) == 0) return; }
    else { while (timeout--) if ((inb(0x64) & 1) == 1) return; }
}
static void mouse_write(unsigned char data) { mouse_wait(0); outb(0x64, 0xD4); mouse_wait(0); outb(0x60, data); }
static unsigned char mouse_read() { mouse_wait(1); return inb(0x60); }
static void real_mouse_init() {
    outb(0x64, 0xA8); mouse_wait(1);
    outb(0x64, 0x20); unsigned char status = mouse_read() | 2; mouse_wait(1);
    outb(0x64, 0x60); mouse_wait(1); outb(0x60, status);
    mouse_write(0xF6); mouse_read(); mouse_write(0xF4); mouse_read();
}
static void mouse_disable(void) {
    mouse_write(0xF5);
    mouse_read();
    drain_input_buffer();
}

static void g_cell(int x, int y, unsigned char c, unsigned char color) {
    if (x >= 0 && x < 100 && y >= 0 && y < 40) g_buf[y * 100 + x] = (color << 8) | c;
}
static void g_flush(void) {
    unsigned short *vga = (unsigned short *)0xB8000;
    for (int y = 0; y < 25; y++) for (int x = 0; x < 80; x++) vga[y * 80 + x] = g_buf[y * 100 + x];
}

static void cmd_ls(char *arg);
static void cmd_snake(char *arg);
static void cmd_toolbox(char *arg);

static void cmd_gui(char *arg) {
    (void)arg;
    clock_stop();
    while (1) {
        clear_screen();
        real_mouse_init();

        int v_mx = 40, v_my = 12;
        unsigned char m_bytes[3];
        int m_cycle = 0;
        int gui_ext_key = 0;
        int trigger_app = 0;

        while (trigger_app == 0) {
            for (int y = 0; y < 40; y++) for (int x = 0; x < 100; x++) g_cell(x, y, ' ', 0x11);
            for (int x = 0; x < 100; x++) g_cell(x, 0, ' ', 0x70);
            const char *tt = " Chlorine_OS - GUI"; for (int i=0; tt[i]; i++) g_cell(2+i, 0, tt[i], 0x70);
            
            int c1 = 0x0B, c2 = 0x0E, c3 = 0x0A, c4 = 0x0C, c5 = 0x09;
            g_cell(10,5,0xDB,c1); g_cell(11,5,0xDB,c1); g_cell(10,6,0xDB,c1); g_cell(11,6,0xDB,c1);
            const char *i1="Files"; for(int i=0; i1[i]; i++) g_cell(9+i, 7, i1[i], 0x0F);
            
            g_cell(30,5,0xDB,c2); g_cell(31,5,0xDB,c2); g_cell(30,6,0xDB,c2); g_cell(31,6,0xDB,c2);
            const char *i2="Editor"; for(int i=0; i2[i]; i++) g_cell(29+i, 7, i2[i], 0x0F);
            
            g_cell(50,5,0xDB,c3); g_cell(51,5,0xDB,c3); g_cell(50,6,0xDB,c3); g_cell(51,6,0xDB,c3);
            const char *i3="Snake"; for(int i=0; i3[i]; i++) g_cell(49+i, 7, i3[i], 0x0F);

            g_cell(70,5,0xDB,c4); g_cell(71,5,0xDB,c4); g_cell(70,6,0xDB,c4); g_cell(71,6,0xDB,c4);
            const char *i4="Terminal"; for(int i=0; i4[i]; i++) g_cell(68+i, 7, i4[i], 0x0F);

            g_cell(88,5,0xDB,c5); g_cell(89,5,0xDB,c5); g_cell(88,6,0xDB,c5); g_cell(89,6,0xDB,c5);
            const char *i5="Toolbox"; for(int i=0; i5[i]; i++) g_cell(86+i, 7, i5[i], 0x0F);

            g_cell(v_mx, v_my, 'X', 0x0F); 
            g_flush();

            unsigned char status = inb(0x64);
            if (status & 1) {
                unsigned char data = inb(0x60);
                if (status & 0x20) {
                    switch (m_cycle) {
                        case 0: if (data & 0x08) { m_bytes[0] = data; m_cycle++; } break;
                        case 1: m_bytes[1] = data; m_cycle++; break;
                        case 2:
                            m_bytes[2] = data; m_cycle = 0;
                            int dx = m_bytes[1] - ((m_bytes[0] << 4) & 0x100);
                            int dy = m_bytes[2] - ((m_bytes[0] << 3) & 0x100);
                            v_mx += dx / 2; v_my -= dy / 2;
                            
                            if (m_bytes[0] & 1) {
                                if (v_mx>=8 && v_mx<=14 && v_my>=4 && v_my<=8) trigger_app = 1;
                                if (v_mx>=28 && v_mx<=34 && v_my>=4 && v_my<=8) trigger_app = 2;
                                if (v_mx>=48 && v_mx<=54 && v_my>=4 && v_my<=8) trigger_app = 3;
                                if (v_mx>=68 && v_mx<=74 && v_my>=4 && v_my<=8) trigger_app = 4;
                                if (v_mx>=86 && v_mx<=92 && v_my>=4 && v_my<=8) trigger_app = 5;
                            }
                            break;
                    }
                } else {
                    m_cycle = 0;
                    if (data == 0xE0) { gui_ext_key = 1; }
                    else if (gui_ext_key) {
                        gui_ext_key = 0;
                        if (data == 0x48) v_my -= 2;
                        else if (data == 0x50) v_my += 2;
                        else if (data == 0x4B) v_mx -= 2;
                        else if (data == 0x4D) v_mx += 2;
                    } else if (!(data & 0x80)) {
                        if (data == 0x01) { trigger_app = -1; break; }
                        if (data == 0x11) v_my--;
                        if (data == 0x1F) v_my++;
                        if (data == 0x1E) v_mx--;
                        if (data == 0x20) v_mx++;
                        if (data == 0x48) v_my -= 2;
                        if (data == 0x50) v_my += 2;
                        if (data == 0x4B) v_mx -= 2;
                        if (data == 0x4D) v_mx += 2;

                        if (data == 0x39 || data == 0x1C) {
                            if (v_mx>=8 && v_mx<=14 && v_my>=4 && v_my<=8) trigger_app = 1;
                            if (v_mx>=28 && v_mx<=34 && v_my>=4 && v_my<=8) trigger_app = 2;
                            if (v_mx>=48 && v_mx<=54 && v_my>=4 && v_my<=8) trigger_app = 3;
                            if (v_mx>=68 && v_mx<=74 && v_my>=4 && v_my<=8) trigger_app = 4;
                            if (v_mx>=86 && v_mx<=92 && v_my>=4 && v_my<=8) trigger_app = 5;
                        }
                    }
                }
            }

            if (v_mx < 0) v_mx = 0; if (v_mx > 99) v_mx = 99; 
            if (v_my < 0) v_my = 0; if (v_my > 39) v_my = 39;
            for (volatile int delay = 0; delay < 20000; delay++);
        }

        mouse_disable();

        if (trigger_app == -1 || trigger_app == 4) {
            clear_screen();
            if (trigger_app == 4) print("Switched to Terminal Mode.\n");
            return;
        }

        clear_screen();
        if (trigger_app == 1) {
            cmd_ls("");
            print("\nPress ESC to return to GUI...");
            while (getkey() != 27);
        }
        else if (trigger_app == 2) {
            print("Enter file to edit: ");
            char buf[24];
            readline(buf, 24);
            uppercase(buf);
            window_text_editor(buf);
        }
        else if (trigger_app == 3) { cmd_snake(""); }
        else if (trigger_app == 5) { cmd_toolbox(""); }
    }
}

/* =============== 贪吃蛇 =============== */
#define SNAKE_MAP_W 60
#define SNAKE_MAP_H 20
static unsigned int s_seed = 54321;
static int s_rand(int max) { s_seed = s_seed * 1103515245 + 12345; return ((unsigned int)(s_seed / 65536) % 32768) % max; }
static void s_draw(int x, int y, unsigned char c, unsigned char col) { ((unsigned short *)0xB8000)[y * 80 + x] = (col << 8) | c; }
static void cmd_snake(char *arg) {
    clear_screen();
    for(int x=0; x<SNAKE_MAP_W; x++) { s_draw(10+x, 2, '#', 0x07); s_draw(10+x, 2+SNAKE_MAP_H-1, '#', 0x07); }
    for(int y=0; y<SNAKE_MAP_H; y++) { s_draw(10, 2+y, '#', 0x07); s_draw(10+SNAKE_MAP_W-1, 2+y, '#', 0x07); }
    struct { int x, y; } snk[200]; int len = 3, dir = 3;
    for(int i=0; i<len; i++) { snk[i].x = 10 + SNAKE_MAP_W/2 - i; snk[i].y = 2 + SNAKE_MAP_H/2; }
    struct { int x, y; } food = {10 + 1 + s_rand(SNAKE_MAP_W-2), 2 + 1 + s_rand(SNAKE_MAP_H-2)};
    int score = 0, run = 1;

    while(run) {
        unsigned char st = inb(0x64);
        if (st & 1) {
            unsigned char k = inb(0x60);
            if (st & 0x20) continue;
            if (k & 0x80) continue;
            s_seed += k;
            if (k == 0x01) break;
            if (k == 0x11 && dir != 1) dir = 0;
            if (k == 0x1F && dir != 0) dir = 1;
            if (k == 0x1E && dir != 3) dir = 2;
            if (k == 0x20 && dir != 2) dir = 3;
        }
        s_draw(snk[len-1].x, snk[len-1].y, ' ', 0);
        for(int i=len-1; i>0; i--) snk[i] = snk[i-1];
        if(dir==0) snk[0].y--; if(dir==1) snk[0].y++; if(dir==2) snk[0].x--; if(dir==3) snk[0].x++;
        if(snk[0].x <= 10 || snk[0].x >= 10+SNAKE_MAP_W-1 || snk[0].y <= 2 || snk[0].y >= 2+SNAKE_MAP_H-1) break;
        for(int i=1; i<len; i++) if(snk[0].x == snk[i].x && snk[0].y == snk[i].y) { run = 0; break; }
        if(!run) break;
        if(snk[0].x == food.x && snk[0].y == food.y) {
            score += 10; if(len < 200) len++; food.x = 10 + 1 + s_rand(SNAKE_MAP_W-2); food.y = 2 + 1 + s_rand(SNAKE_MAP_H-2);
        }
        s_draw(food.x, food.y, '@', 0x0C); s_draw(snk[0].x, snk[0].y, 0xDB, 0x0A);
        for(int i=1; i<len; i++) s_draw(snk[i].x, snk[i].y, 0xDB, 0x02);
        for(volatile int i=0; i<((dir==0||dir==1)?12000000:7000000); i++);
    }
    clear_screen(); set_color(0x0C, 0); print("\n    GAME OVER!\n"); set_color(0x0F, 0); print("    Score: "); 
    print_int(score); print("\n    Press any key..."); 
    while(inb(0x64)&1)inb(0x60); while(!(inb(0x64)&1)); inb(0x60); clear_screen();
}

/* =============== 改进的 auto_mount =============== */
static void auto_mount() {
    unsigned short *vga = (unsigned short *)0xB8000;
    
    vga[80] = 0x0F00 | 'A';
    vga[81] = 0x0F00 | 'M';
    vga[82] = 0x0F00 | '1';
    
    ide_init();
    
    vga[84] = 0x0F00 | 'I';
    vga[85] = 0x0F00 | 'D';
    vga[86] = 0x0F00 | 'E';
    vga[87] = 0x0F00 | 'O';
    vga[88] = 0x0F00 | 'K';
    
    // ===== 直接挂载整个硬盘（不检查 MBR） =====
    vga[90] = 0x0F00 | 'F';
    vga[91] = 0x0F00 | 'A';
    vga[92] = 0x0F00 | 'T';
    vga[93] = 0x0F00 | '1';
    
    int ret = fat32_init(0);
    
    vga[95] = 0x0F00 | 'F';
    vga[96] = 0x0F00 | '2';
    
    if (ret == 0) {
        first_part_lba = 0;
        fat32_mounted = 1;
        vga[98] = 0x0F00 | 'O';
        vga[99] = 0x0F00 | 'K';
        print("FAT32 mounted successfully!\n");
        print("Root directory contents:\n");
        fat32_list_root();
        return;
    } else {
        vga[98] = 0x0F00 | 'E';
        vga[99] = 0x0F00 | 'R';
        vga[100] = 0x0F00 | 'R';
        print("fat32_init(0) failed with error: ");
        print_int(ret);
        print("\n");
        
        // 尝试第二种方法：直接读取 MBR
        vga[102] = 0x0F00 | 'M';
        vga[103] = 0x0F00 | 'B';
        vga[104] = 0x0F00 | 'R';
        
        unsigned char sector[512]; 
        if (ide_read_sectors(0, 1, sector) == 0) {
            MBR *mbr = (MBR *)sector; 
            if (mbr->signature == 0xAA55) {
                print("MBR found, scanning partitions...\n");
                for (int i = 0; i < 4; i++) {
                    MBRPartition *p = &mbr->partitions[i];
                    if (p->type == 0x0C || p->type == 0x0B) { 
                        first_part_lba = p->lba_start; 
                        print("FAT32 partition at LBA ");
                        print_int(first_part_lba);
                        print("\n");
                        ret = fat32_init(first_part_lba);
                        if (ret == 0) {
                            fat32_mounted = 1;
                            print("FAT32 mounted from partition!\n");
                            fat32_list_root();
                            return;
                        }
                    }
                }
            } else {
                print("No MBR signature (0xAA55), using whole disk\n");
                // 再试一次直接挂载
                ret = fat32_init(0);
                if (ret == 0) {
                    first_part_lba = 0;
                    fat32_mounted = 1;
                    print("FAT32 mounted successfully (whole disk)!\n");
                    fat32_list_root();
                    return;
                }
            }
        }
    }
    
    print("No FAT32 found.\n");
    fat32_mounted = 0;
}

/* =============== 命令实现 =============== */
static void cmd_cat(char *arg) {
    if (!fat32_mounted) {
        print("No FAT32 mounted.\n");
        return;
    }
    if (!arg || arg[0] == '\0') {
        print("Usage: cat <filename>\n");
        return;
    }
    uppercase(arg);
    char *buf = (char *)0x50000;
    int bytes = fat32_read_file_content(arg, buf, 200000);
    if (bytes < 0) {
        print("File not found.\n");
        return;
    }
    for (int i = 0; i < bytes; i++) putchar((buf[i] >= 32 && buf[i] <= 126) || buf[i]=='\n' ? buf[i] : '?');
    print("\n");
}

static void cmd_rm(char *arg) {
    if (!fat32_mounted) {
        print("No FAT32 mounted.\n");
        return;
    }
    if (!arg || arg[0] == '\0') {
        print("Usage: rm <filename>\n");
        return;
    }
    uppercase(arg);
    if (fat32_delete_file(arg) == 0) print("Deleted.\n"); else print("Failed.\n");
}

static void cmd_new(char *arg) {
    if (!fat32_mounted) {
        print("No FAT32 mounted.\n");
        return;
    }
    if (!arg || arg[0] == '\0') {
        print("Usage: new <filename>\n");
        return;
    }
    uppercase(arg);
    char *tb = (char *)0x50000;
    if (fat32_read_file_content(arg, tb, 1) >= 0) {
        print("File exists.\n");
        return;
    }
    int result = fat32_create_file(arg);
    if (result == 0) print("Created.\n");
    else {
        print("Error code: ");
        print_int(-result);
        print("\n");
    }
}

static void cmd_edit(char *arg) {
    if (!fat32_mounted) {
        print("No FAT32 mounted.\n");
        return;
    }
    if (!arg || arg[0] == '\0') {
        print("Usage: edit <filename>\n");
        return;
    }
    uppercase(arg);
    char *testbuf = (char *)0x50000;
    if (fat32_read_file_content(arg, testbuf, 1) < 0)
        print("File not found, creating new...\n");
    window_text_editor(arg);
    clear_screen();
    print("ChlorineOS Shell ready.\n");
}

void cmd_color(char *arg) {
    if (!arg || arg[0] == '\0') {
        vga_attr = 0x07;
    } else {
        int bg = hex_to_int(arg[0]);
        int fg = hex_to_int(arg[1]);
        if (bg == -1 || fg == -1) {
            print("Format: color 1A\n");
            return;
        }
        vga_attr = ((bg & 0x0F) << 4) | (fg & 0x0F);
    }
    unsigned char *vga_ptr = (unsigned char *)0xB8001;
    for (int i = 0; i < 80 * 25; i++) {
        *vga_ptr = vga_attr;
        vga_ptr += 2;
    }
}

static void cmd_toolbox(char *arg) {
    while(1) {
        clear_screen();
        set_color(0x0B, 0x00);
        print("       ChlorineOS_OS Multi-Toolbox       \n");
        set_color(0x07, 0x00);
        print("1. Advanced Calculator (Decimals & Division)\n");
        print("2. Clock and Calendar\n");
        print("ESC. Exit to Shell\n");
        
        int key = getkey();
        if (key == 27) break;
        if (key == '1') {
            print("\n--- Calculator ---\n");
            print("Enter expression (e.g. 10.5 / 2.5): ");
            char expr[64];
            readline(expr, sizeof(expr));
            calc_eval(expr);
            print("\nPress any key to continue...");
            getkey();
        }
        if (key == '2') {
            print("\n--- Clock ---\n");
            print_current_time();
            print("\nPress any key to continue...");
            getkey();
        }
    }
    clear_screen();
}

static void cmd_ver(char *arg) {
    set_color(0x09,0x00); print("ChlorineOS (v2026 - 1.01)\n");
    set_color(0x07,0x00); print("Engine: VGA Text Mode 80x25\n\n");
    print("00000000000000000000000000000000   000000000000000000000\n");
    print(" 000000000000000000000000000000   0000000000000000000000\n");
    print("  0000000000000000000000000000   00000000000000000000000\n");
    print("                  00000000000   0000000000   0000000000 \n");
    print("                 00000000000   0000000000   0000000000  \n");
    print("                00000000000   0000000000   0000000000   \n");
    print("               00000000000   0000000000   0000000000    \n");
    print("              00000000000   0000000000   0000000000     \n");
    print("             00000000000   0000000000   0000000000      \n");
    print("            00000000000   0000000000   0000000000       \n");
    print("           00000000000   0000000000   0000000000        \n");
    print("          00000000000   0000000000   0000000000         \n");
    print("         00000000000   0000000000   0000000000          \n");
    print("        00000000000   0000000000   0000000000           \n");
    print("         000000000     00000000     00000000            \n");
    print("          0000000       000000       000000             \n\n");
    print("Please visit\n");
    set_color(0x0A,0x00); print("gz1012a.xyz/sys");
    set_color(0x07,0x00); print(" or ");
    set_color(0x0A,0x00); print("github.com/GeorgeZ787/Cl_OS\n");
    set_color(0x07,0x00); print("for more information\n\n");
}

static void cmd_cd(char *arg) {
    if (!fat32_mounted) {
        print("No FAT32 mounted.\n");
        return;
    }
    if (!arg || arg[0] == '\0') {
        char path[256];
        fat32_get_current_path(path, sizeof(path));
        print("Current directory: ");
        print(path);
        print("\n");
        return;
    }
    
    if (fat32_change_directory(arg) != 0) {
        print("Directory not found: ");
        print(arg);
        print("\n");
    }
}

static void cmd_ls(char *arg) {
    if (!fat32_mounted) {
        print("No FAT32 mounted.\n");
        return;
    }
    fat32_list_current_directory();
}

static void cmd_mk(char *arg) {
    if (!fat32_mounted) {
        print("No FAT32 mounted.\n");
        return;
    }
    if (!arg || arg[0] == '\0') {
        print("Usage: mk <dirname>\n");
        return;
    }
    uppercase(arg);
    fat32_create_directory(arg);
}

static void cmd_cls(char *arg) {
    clear_screen();
}

static void cmd_run(char *arg) {
    if (arg && arg[0] == 't' && arg[1] == 'i' && arg[2] == 'm' &&
        arg[3] == 'e' && arg[4] == '.' && arg[5] == 'b' &&
        arg[6] == 'i' && arg[7] == 'n' && arg[8] == '\0') {
        if (clock_process_pid >= 0) {
            print("time.bin is already running.\n");
            return;
        }
        clock_process_pid = process_start_service("time.bin");
        if (clock_process_pid < 0) {
            print("Failed to start time.bin\n");
            return;
        }
        clock_last_second = 0xFF;
        print("Clock process started: PID=");
        print_int(clock_process_pid);
        print("\n");
        clock_update();
        return;
    }
    if (!fat32_mounted) {
        print("No FAT32 mounted.\n");
        return;
    }
    if (!arg || arg[0] == '\0') {
        print("Usage: run <program.bin>\n");
        return;
    }
    
    print("Creating process...\n");
    int pid = process_create(arg, "");
    if (pid < 0) {
        print("Failed to create process\n");
        return;
    }
    
    print("Process created: PID=");
    print_int(pid);
    print("\n");
    
    print("Calling scheduler...\n");
    scheduler();
    print("Scheduler returned!\n");  // 如果这行打印了，说明调度器返回了
}

static void cmd_ps(char *arg) {
    process_list();
}

static void cmd_kill(char *arg) {
    if (!arg || arg[0] == '\0') {
        print("Usage: kill <pid>\n");
        return;
    }
    if (arg[0] == 't' && arg[1] == 'i' && arg[2] == 'm' &&
        arg[3] == 'e' && arg[4] == '.' && arg[5] == 'b' &&
        arg[6] == 'i' && arg[7] == 'n' && arg[8] == '\0') {
        if (clock_process_pid < 0 || process_kill_by_name("time.bin") < 0) {
            print("Process not found: time.bin\n");
            return;
        }
        clock_process_pid = -1;
        clock_last_second = 0xFF;
        print("Clock process stopped.\n");
        terminal_render();
        return;
    }
    int pid = 0;
    for (int i = 0; arg[i] >= '0' && arg[i] <= '9'; i++) {
        pid = pid * 10 + (arg[i] - '0');
    }
    process_kill(pid);
}

void *kmalloc(unsigned int size) {
    unsigned int addr = heap_ptr;
    heap_ptr += size;
    // 4字节对齐
    heap_ptr = (heap_ptr + 3) & ~3;
    return (void *)addr;
}

void kfree(void *ptr) {
    // 简单实现：不释放内存
}

static void cmd_diskinfo(char *arg) { ide_init(); ide_display_info(); mbr_list_partitions(); }
static void cmd_mem(char *arg) { print("Heap usage feature active.\n"); }
static void cmd_test_mem(char *arg) { print("Test mem executed.\n"); }
static void cmd_shutdown(char *arg) { print("Shutting down...\n"); outw(0x604,0x2000); while(1); }

/*  指令注册表 */
static void cmd_help(char *arg);
typedef void (*cmd_func_t)(char *arg);
struct cmd_entry { const char *name; const char *desc; cmd_func_t func; };

struct cmd_entry cmd_table[] = {
    {"help",    "Show commands",        cmd_help},
    {"ver",     "Show OS version",      cmd_ver},
    {"ls",      "List directory",       cmd_ls},    
    {"cd",      "Change directory",     cmd_cd},       
    {"mk",      "Make directory",       cmd_mk},       
    {"cat",     "Display file",         cmd_cat},     
    {"new",     "Create file",          cmd_new},
    {"edit",    "Edit file",            cmd_edit},
    {"rm",      "Delete file",          cmd_rm},
    {"run",     "Run program",          cmd_run},
    {"ps",      "List processes",       cmd_ps},
    {"kill",    "Kill process",         cmd_kill},
    {"di",      "Show disk info",       cmd_diskinfo},
    {"gui",     "Launch Desktop GUI",   cmd_gui},     
    {"snake",   "Play Snake game",      cmd_snake},
    {"col",     "Set color",            cmd_color},
    {"toolbox", "Open Multi-Toolbox",   cmd_toolbox},
    {"mem",     "Check memory status",  cmd_mem},
    {"shut",    "Shutdown VM",          cmd_shutdown},
    {"testmem", "Test kmalloc",         cmd_test_mem},
    {"cls",     "clear screen",         cmd_cls},
    {NULL, NULL, NULL}
};

static void cmd_help(char *arg) {
    set_color(0x0E, 0x00); print("Commands:\n"); set_color(0x07, 0x00);
    for (int i = 0; cmd_table[i].name != NULL; i++) {
        print("  ");
        set_color(0x0B, 0x00);
        print(cmd_table[i].name);
        set_color(0x07, 0x00);
        print(" - ");
        print(cmd_table[i].desc);
        print("\n");
    }
}

static void set_idt_gate(int n, unsigned int handler) {
    idt[n].offset_low = handler & 0xFFFF;
    idt[n].selector = 0x08; // 你的内核代码段选择子
    idt[n].zero = 0;
    idt[n].type_attr = 0xEE; // 中断门，运行在内核/用户态 (0x80 或 0xEE)
    idt[n].offset_high = (handler >> 16) & 0xFFFF;
}

/* =============== 核心解析循环 =============== */
static void init_idt() {
    idtp.limit = sizeof(idt) - 1;
    idtp.base = (unsigned int)idt;

    for (int i = 0; i < 256; i++) {
        idt[i].offset_low = 0;
        idt[i].selector = 0;
        idt[i].zero = 0;
        idt[i].type_attr = 0;
        idt[i].offset_high = 0;
    }

    // 绑定 0x80 中断号到我们的系统调用处理函数
    set_idt_gate(0x80, (unsigned int)syscall_handler);

    asm volatile("lidt (%0)" : : "r"(&idtp));

    // 重新映射 PIC 并且取消 0x80 相关的屏蔽
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);

    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

static void init_keyboard() {
    int timeout = 100000;
    while (timeout-- > 0) {
        if ((inb(0x64) & 2) == 0) break;
    }
    
    outb(0x64, 0xAA);
    
    timeout = 100000;
    while (timeout-- > 0) {
        if (inb(0x64) & 1) {
            unsigned char reply = inb(0x60);
            if (reply == 0x55) {
                //print("Keyboard self-test passed.\n");
                break;
            }
        }
    }
    
    outb(0x64, 0xAE);
    outb(0x60, 0xF4);  
}

// ============================================================
// Shell 主控环境
// ============================================================
static void shell_entry() {
    char cmd_buf[64];
    while (1) {
        print("\\> ");
        
        int i = 0;
        cmd_buf[0] = '\0';
        while (i < 63) {
            int key = getchar();
            if (key == 0x100 + 'U') {
                terminal_scroll(-1);
                continue;
            }
            if (key == 0x100 + 'D') {
                terminal_scroll(1);
                continue;
            }
            char c = (char)key;
            if (c == '\n' || c == '\r') {
                putchar('\n');
                break;
            } else if (c == '\b') {
                if (i > 0) {
                    i--;
                    putchar('\b');
                    putchar(' ');
                    putchar('\b');
                }
            } else if (c == '\t') {
                for (int spaces = 0; spaces < 4 && i < 63; spaces++) {
                    putchar(' ');
                    cmd_buf[i++] = ' ';
                }
            } else if (c >= 32 && c <= 126) {
                putchar(c);
                cmd_buf[i++] = c;
            }
        }
        cmd_buf[i] = '\0';

        char *p = cmd_buf;
        while (*p == ' ') p++;
        if (*p == '\0') {
            continue;
        }

        char *arg = p;
        while (*arg != ' ' && *arg != '\0') arg++;
        if (*arg == ' ') {
            *arg = '\0';
            arg++;
            while (*arg == ' ') arg++;
        }

        int found = 0;
        for (int idx = 0; cmd_table[idx].name != NULL; idx++) {
            char *s1 = p;
            char *s2 = (char *)cmd_table[idx].name;
            int match = 1;
            while (*s1 != '\0' && *s2 != '\0') {
                if (*s1 != *s2) { match = 0; break; }
                s1++; s2++;
            }
            if (*s1 != *s2) match = 0;

            if (match) {
                cmd_table[idx].func(arg);
                found = 1;
                break;
            }
        }

        if (!found) {
            print("Unknown command: ");
            print(p);
            print("\n");
        }
    }
}

// ===== kernel_main - 真正活起来 =====
void kernel_main() {
    clear_screen();
    
    unsigned short *vga = (unsigned short *)0xB8000;
    vga[0] = 0x0F43;  // 'C'
    vga[1] = 0x0F4C;  // 'L'
    vga[2] = 0x0F5F;  // '_'
    vga[3] = 0x0F4F;  // 'O'
    vga[4] = 0x0F53;  // 'S'
    
    set_color(0x0F, 0x00);
    print("\n=== ChlorineOS ===\n");
    print("Initializing system...\n");

    init_idt(); // 添加了缺失的调用
    init_keyboard();
    init_heap();
    print("Heap initialized.\n");

    auto_mount();
    if (fat32_mounted) {
        print("FAT32 mounted successfully.\n");
    } else {
        print("Warning: No FAT32 partition found.\n");
    }

    // ===== 初始化进程管理 =====
    process_init();

    clear_screen();
    print("Type 'help' for commands\n");
    
    // ===== 进入主控 Shell =====
    shell_entry();
    
    // 不会执行到这里
    while (1) { asm volatile("hlt"); }
}