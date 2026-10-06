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

#define VBE_INFO ((volatile unsigned char *)0x5000)
#define BIOS_FONT ((const unsigned char *)0x6000)
#define GFX_WIDTH 1024
#define GFX_HEIGHT 768

static volatile unsigned char *framebuffer;
static unsigned int framebuffer_pitch;
static unsigned char red_position, green_position, blue_position;
static unsigned char red_size, green_size, blue_size;
static int graphics_ready;

static void graphics_fill(int x, int y, int width, int height, unsigned int color);

static unsigned int graphics_channel(unsigned char value, unsigned char size, unsigned char position) {
    unsigned int channel = value;
    if (size < 8) channel >>= 8 - size;
    return channel << position;
}

static unsigned int graphics_rgb(unsigned char red, unsigned char green, unsigned char blue) {
    return graphics_channel(red, red_size, red_position) |
           graphics_channel(green, green_size, green_position) |
           graphics_channel(blue, blue_size, blue_position);
}

static int graphics_init(void) {
    if (VBE_INFO[27] != 6 ||
        VBE_INFO[25] != 24) return 0;

    unsigned short width = *(volatile unsigned short *)(VBE_INFO + 18);
    unsigned short height = *(volatile unsigned short *)(VBE_INFO + 20);
    unsigned short pitch = *(volatile unsigned short *)(VBE_INFO + 50);
    unsigned int address = *(volatile unsigned int *)(VBE_INFO + 40);
    if (pitch == 0) pitch = *(volatile unsigned short *)(VBE_INFO + 16);
    if (width != GFX_WIDTH || height != GFX_HEIGHT ||
        pitch < GFX_WIDTH * 3 || address == 0) return 0;

    red_size = VBE_INFO[53];
    red_position = VBE_INFO[54];
    green_size = VBE_INFO[55];
    green_position = VBE_INFO[56];
    blue_size = VBE_INFO[57];
    blue_position = VBE_INFO[58];
    if (red_size == 0 || green_size == 0 || blue_size == 0) {
        red_size = VBE_INFO[31];
        red_position = VBE_INFO[32];
        green_size = VBE_INFO[33];
        green_position = VBE_INFO[34];
        blue_size = VBE_INFO[35];
        blue_position = VBE_INFO[36];
    }
    if (red_size == 0 || red_size > 8 || red_position + red_size > 24 ||
        green_size == 0 || green_size > 8 || green_position + green_size > 24 ||
        blue_size == 0 || blue_size > 8 || blue_position + blue_size > 24) {
        red_size = 8;
        red_position = 16;
        green_size = 8;
        green_position = 8;
        blue_size = 8;
        blue_position = 0;
    }

    framebuffer = (volatile unsigned char *)address;
    framebuffer_pitch = pitch;
    graphics_ready = 1;
    return 1;
}

extern int video_set_mode(unsigned int mode);

static void graphics_pixel(int x, int y, unsigned int color) {
    if (!graphics_ready || x < 0 || x >= GFX_WIDTH || y < 0 || y >= GFX_HEIGHT) return;
    volatile unsigned char *pixel = framebuffer + y * framebuffer_pitch + x * 3;
    pixel[0] = (unsigned char)color;
    pixel[1] = (unsigned char)(color >> 8);
    pixel[2] = (unsigned char)(color >> 16);
}

static void graphics_fill(int x, int y, int width, int height, unsigned int color) {
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > GFX_WIDTH) width = GFX_WIDTH - x;
    if (y + height > GFX_HEIGHT) height = GFX_HEIGHT - y;
    if (width <= 0 || height <= 0) return;

    for (int row = y; row < y + height; row++) {
        volatile unsigned char *pixel = framebuffer + row * framebuffer_pitch + x * 3;
        for (int column = 0; column < width; column++) {
            pixel[column * 3] = (unsigned char)color;
            pixel[column * 3 + 1] = (unsigned char)(color >> 8);
            pixel[column * 3 + 2] = (unsigned char)(color >> 16);
        }
    }
}

static void graphics_line(int x0, int y0, int x1, int y1, unsigned int color) {
    int dx = x1 >= x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 >= y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;

    while (1) {
        graphics_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int twice_error = error * 2;
        if (twice_error >= dy) { error += dy; x0 += sx; }
        if (twice_error <= dx) { error += dx; y0 += sy; }
    }
}

static void graphics_text(int x, int y, const char *text, unsigned int color, int scale) {
    while (*text) {
        unsigned char character = (unsigned char)*text++;
        const unsigned char *glyph = BIOS_FONT + character * 8;
        for (int row = 0; row < 8; row++) {
            unsigned char bits = glyph[row];
            for (int column = 0; column < 8; column++) {
                if (bits & (0x80 >> column))
                    graphics_fill(x + column * scale, y + row * scale,
                                  scale, scale, color);
            }
        }
        x += 8 * scale;
    }
}

static void graphics_cursor(int x, int y) {
    for (int row = 0; row < 18; row++) {
        int width = row < 12 ? row / 2 + 1 : (17 - row) / 2;
        for (int column = 0; column < width; column++) {
            graphics_pixel(x + column, y + row,
                           column == 0 || row == 0 ? graphics_rgb(10, 15, 25) :
                           graphics_rgb(255, 255, 255));
        }
    }
    graphics_line(x, y, x, y + 17, graphics_rgb(10, 15, 25));
    graphics_line(x, y, x + 9, y + 9, graphics_rgb(10, 15, 25));
}

static unsigned int gui_background(int y) {
    unsigned char red = (unsigned char)(21 + y * 8 / GFX_HEIGHT);
    unsigned char green = (unsigned char)(56 + y * 14 / GFX_HEIGHT);
    unsigned char blue = (unsigned char)(105 + y * 28 / GFX_HEIGHT);
    return graphics_rgb(red, green, blue);
}

static void graphics_desktop_background(void) {
    for (int y = 0; y < GFX_HEIGHT; y++)
        graphics_fill(0, y, GFX_WIDTH, 1, gui_background(y));
    graphics_fill(0, 0, GFX_WIDTH, 42, graphics_rgb(23, 34, 57));
    graphics_text(24, 15, "CHLORINE OS", graphics_rgb(238, 245, 255), 1);
    graphics_fill(0, GFX_HEIGHT - 48, GFX_WIDTH, 48, graphics_rgb(23, 34, 57));
    graphics_fill(14, GFX_HEIGHT - 41, 112, 34, graphics_rgb(42, 93, 162));
    graphics_text(36, GFX_HEIGHT - 29, "START", graphics_rgb(255, 255, 255), 1);
    graphics_fill(GFX_WIDTH - 194, GFX_HEIGHT - 41, 180, 34, graphics_rgb(144, 55, 67));
    graphics_text(GFX_WIDTH - 174, GFX_HEIGHT - 29, "EXIT TO CLI",
                  graphics_rgb(255, 255, 255), 1);
}

static void graphics_desktop_icon(int x, int y, int app, const char *label) {
    unsigned int accent = graphics_rgb(92 + app * 20, 167 - app * 9, 238 - app * 17);
    graphics_fill(x - 34, y - 34, 68, 68, graphics_rgb(255, 255, 255));
    graphics_fill(x - 32, y - 32, 64, 64, graphics_rgb(40, 55, 82));

    if (app == 1) {
        graphics_fill(x - 22, y - 12, 21, 7, accent);
        graphics_fill(x - 25, y - 6, 50, 27, accent);
        graphics_fill(x - 21, y - 2, 42, 19, graphics_rgb(249, 193, 83));
    } else if (app == 2) {
        graphics_fill(x - 18, y - 23, 36, 46, graphics_rgb(245, 248, 253));
        graphics_fill(x - 11, y - 12, 22, 3, accent);
        graphics_fill(x - 11, y - 4, 22, 3, graphics_rgb(110, 126, 151));
        graphics_fill(x - 11, y + 4, 16, 3, graphics_rgb(110, 126, 151));
        graphics_fill(x - 11, y + 12, 20, 3, graphics_rgb(110, 126, 151));
    } else if (app == 3) {
        graphics_fill(x - 22, y - 9, 39, 8, graphics_rgb(65, 202, 119));
        graphics_fill(x - 9, y - 17, 8, 25, graphics_rgb(65, 202, 119));
        graphics_fill(x - 1, y - 17, 8, 25, graphics_rgb(65, 202, 119));
        graphics_fill(x + 7, y - 17, 12, 8, graphics_rgb(65, 202, 119));
        graphics_fill(x - 23, y + 10, 8, 8, graphics_rgb(251, 110, 108));
    } else if (app == 4) {
        graphics_fill(x - 24, y - 18, 48, 33, graphics_rgb(15, 23, 39));
        graphics_fill(x - 20, y - 14, 40, 25, graphics_rgb(126, 218, 190));
        graphics_fill(x - 4, y + 15, 8, 7, graphics_rgb(225, 232, 244));
        graphics_fill(x - 14, y + 21, 28, 4, graphics_rgb(225, 232, 244));
    } else {
        graphics_fill(x - 22, y - 22, 44, 44, accent);
        graphics_fill(x - 14, y - 14, 28, 28, graphics_rgb(40, 55, 82));
        graphics_fill(x - 7, y - 7, 14, 14, graphics_rgb(249, 193, 83));
        graphics_fill(x - 26, y - 4, 8, 8, accent);
        graphics_fill(x + 18, y - 4, 8, 8, accent);
        graphics_fill(x - 4, y - 26, 8, 8, accent);
        graphics_fill(x - 4, y + 18, 8, 8, accent);
    }

    int text_width = 0;
    while (label[text_width / 8]) text_width += 8;
    int text_x = x - text_width / 2;
    graphics_fill(text_x - 6, y + 40, text_width + 12, 20, graphics_rgb(28, 42, 67));
    graphics_text(text_x, y + 46, label, graphics_rgb(255, 255, 255), 1);
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
#define TERMINAL_CELL_WIDTH 10
#define TERMINAL_CELL_HEIGHT 18
#define TERMINAL_PIXEL_X 112
#define TERMINAL_PIXEL_Y 128
static int vga_col = 0, vga_row = 0;
static int terminal_frame_drawn;
static int terminal_render_valid;
static int terminal_render_lines[VGA_HEIGHT];
static unsigned short terminal_render_cache[VGA_HEIGHT][VGA_WIDTH];
static int terminal_cursor_x = -1, terminal_cursor_y = -1;

/* Keep a bounded line history so the CLI can move its viewport without
 * changing the behavior of programs that draw directly to VGA memory. */
#define TERMINAL_HISTORY_LINES 256
static unsigned short terminal_history[TERMINAL_HISTORY_LINES][VGA_WIDTH];
static int terminal_line = 0;
static int terminal_view = 0;
static int clock_process_pid = -1;
static unsigned char clock_last_second = 0xFF;
static void (*gui_output_redirect)(char);

static unsigned char rtc_read(unsigned char reg);
static unsigned char rtc_bcd_to_binary(unsigned char value);
static void clock_update(void);

static unsigned int vga_color(unsigned char color) {
    static const unsigned char palette[16][3] = {
        {0, 0, 0}, {0, 0, 170}, {0, 170, 0}, {0, 170, 170},
        {170, 0, 0}, {170, 0, 170}, {170, 85, 0}, {170, 170, 170},
        {85, 85, 85}, {85, 85, 255}, {85, 255, 85}, {85, 255, 255},
        {255, 85, 85}, {255, 85, 255}, {255, 255, 85}, {255, 255, 255}
    };
    return graphics_rgb(palette[color & 0x0F][0],
                        palette[color & 0x0F][1],
                        palette[color & 0x0F][2]);
}

static void graphics_cell(int x, int y, unsigned short cell, int cursor) {
    unsigned char character = (unsigned char)cell;
    unsigned char attribute = (unsigned char)(cell >> 8);
    unsigned int foreground = vga_color(attribute & 0x0F);
    unsigned int background = vga_color(attribute >> 4);
    if (cursor) {
        unsigned int swap = foreground;
        foreground = background;
        background = swap;
    }

    int pixel_x = TERMINAL_PIXEL_X + x * TERMINAL_CELL_WIDTH;
    int pixel_y = TERMINAL_PIXEL_Y + y * TERMINAL_CELL_HEIGHT;
    graphics_fill(pixel_x, pixel_y, TERMINAL_CELL_WIDTH, TERMINAL_CELL_HEIGHT, background);
    const unsigned char *glyph = BIOS_FONT + character * 8;
    for (int row = 0; row < TERMINAL_CELL_HEIGHT; row++) {
        unsigned char bits = glyph[row * 8 / TERMINAL_CELL_HEIGHT];
        for (int column = 0; column < TERMINAL_CELL_WIDTH; column++) {
            if (bits & (0x80 >> (column * 8 / TERMINAL_CELL_WIDTH)))
                graphics_pixel(pixel_x + column, pixel_y + row, foreground);
        }
    }
}

static void graphics_terminal_frame(void) {
    for (int y = 0; y < GFX_HEIGHT; y++)
        graphics_fill(0, y, GFX_WIDTH, 1, gui_background(y));
    graphics_fill(0, 0, GFX_WIDTH, 42, graphics_rgb(23, 34, 57));
    graphics_text(24, 15, "CHLORINE OS", graphics_rgb(238, 245, 255), 1);
    graphics_fill(0, GFX_HEIGHT - 48, GFX_WIDTH, 48, graphics_rgb(23, 34, 57));
    graphics_fill(14, GFX_HEIGHT - 41, 112, 34, graphics_rgb(42, 93, 162));
    graphics_text(36, GFX_HEIGHT - 29, "START", graphics_rgb(255, 255, 255), 1);

    graphics_fill(88, 82, 848, 520, graphics_rgb(10, 18, 32));
    graphics_fill(92, 86, 840, 512, graphics_rgb(239, 243, 249));
    graphics_fill(92, 86, 840, 34, graphics_rgb(37, 56, 84));
    graphics_text(110, 99, "CHLORINE OS TERMINAL", graphics_rgb(245, 248, 255), 1);
    graphics_fill(912, 99, 7, 7, graphics_rgb(255, 111, 105));
    graphics_fill(898, 99, 7, 7, graphics_rgb(255, 190, 78));
    graphics_fill(884, 99, 7, 7, graphics_rgb(82, 201, 136));
    graphics_fill(104, 126, 816, 456, graphics_rgb(16, 22, 34));
}

static void move_cursor() {
    if (graphics_ready) return;
    unsigned short pos = vga_row * VGA_WIDTH + vga_col;
    outb(0x3D4, 0x0F); outb(0x3D5, (unsigned char)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (unsigned char)((pos >> 8) & 0xFF));
}

static void terminal_fill_line(int line) {
    for (int x = 0; x < VGA_WIDTH; x++)
        terminal_history[line][x] = (unsigned short)(vga_attr << 8) | ' ';
}

static void terminal_render(void) {
    if (graphics_ready) {
        if (!terminal_frame_drawn) {
            graphics_terminal_frame();
            terminal_frame_drawn = 1;
            terminal_render_valid = 0;
        }
        int cursor_y = terminal_line - terminal_view;
        for (int y = 0; y < VGA_HEIGHT; y++) {
            int line = terminal_view + y;
            for (int x = 0; x < VGA_WIDTH; x++) {
                unsigned short cell = (line < TERMINAL_HISTORY_LINES) ?
                    terminal_history[line][x] : (unsigned short)(vga_attr << 8) | ' ';
                int cursor = line == terminal_line && y == cursor_y && x == vga_col;
                int cursor_changed = (x == terminal_cursor_x && y == terminal_cursor_y) ||
                                     (cursor && (terminal_cursor_x != vga_col ||
                                                 terminal_cursor_y != cursor_y));
                if (!terminal_render_valid || terminal_render_lines[y] != line ||
                    terminal_render_cache[y][x] != cell || cursor_changed)
                    graphics_cell(x, y, cell, cursor);
                terminal_render_cache[y][x] = cell;
            }
            terminal_render_lines[y] = line;
        }
        terminal_render_valid = 1;
        terminal_cursor_x = vga_col;
        terminal_cursor_y = cursor_y;
        clock_update();
        vga_row = terminal_line - terminal_view;
        return;
    }

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
    terminal_render_valid = 0;
}

static void clock_write_cell(int row, int col, char value) {
    if (graphics_ready && row >= 0 && row < VGA_HEIGHT && col >= 0 && col < VGA_WIDTH) {
        graphics_cell(col, row, (unsigned short)(vga_attr << 8) | (unsigned char)value, 0);
        return;
    }
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
    if (gui_output_redirect) {
        gui_output_redirect(c);
        return;
    }
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

static void put_text_cell(int index, unsigned short cell) {
    if (index < 0 || index >= VGA_WIDTH * VGA_HEIGHT) return;
    if (graphics_ready) {
        graphics_cell(index % VGA_WIDTH, index / VGA_WIDTH, cell, 0);
        return;
    }
    ((unsigned short *)VGA_BASE)[index] = cell;
}

void clear_screen() {
    if (gui_output_redirect) {
        gui_output_redirect('\f');
        return;
    }
    vga_col = vga_row = 0;
    terminal_line = terminal_view = 0;
    terminal_frame_drawn = 0;
    terminal_render_valid = 0;
    terminal_cursor_x = terminal_cursor_y = -1;
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
    for (int y = win_y; y < win_y + win_h; y++)
        for (int x = win_x; x < win_x + win_w; x++)
            put_text_cell(y * VGA_WIDTH + x, 0x07DB);

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
        for (int _y = 0; _y < win_h; _y++) for (int _x = 0; _x < win_w; _x++) put_text_cell((win_y + _y) * VGA_WIDTH + (win_x + _x), 0x07DB); \
        int _pos = 0; \
        for (int _line = 0; _line < win_h; _line++) { \
            for (int _col = 0; _col < win_w && _pos < text_len; _col++, _pos++) { \
                char ch = text[_pos]; if (ch == '\n') break; \
                if (ch >= 32 && ch <= 126) put_text_cell((win_y + _line) * VGA_WIDTH + (win_x + _col), 0x0700 | ch); \
            } \
            while (_pos < text_len && text[_pos] == '\n') _pos++; if (_pos >= text_len) break; \
        } \
        if (cur_y < win_h) put_text_cell((win_y + cur_y) * VGA_WIDTH + (win_x + cur_x), 0x0FDB); \
        if (open_filename[0]) { \
            for (int i = 0; i < win_w; i++) put_text_cell((win_y+win_h)*VGA_WIDTH+(win_x+i), 0x1E00 | ' '); \
            for (int i = 0; open_filename[i]; i++) put_text_cell((win_y+win_h)*VGA_WIDTH+(win_x+2+i), 0x1E00 | open_filename[i]); \
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
                    for (int i = 0; i < win_w; i++)
                        put_text_cell((win_y+win_h)*VGA_WIDTH+(win_x+i), 0x2E00 | ' ');
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
static int mouse_wait(int a_type) {
    int timeout = 100000;
    while (timeout-- > 0) {
        unsigned char status = inb(0x64);
        if (a_type == 0 && !(status & 2)) return 1;
        if (a_type == 1 && (status & 0x21) == 0x21) return 1;
        if (a_type == 2 && (status & 1)) return 1;
    }
    return 0;
}
static int mouse_write(unsigned char data) {
    if (!mouse_wait(0)) return 0;
    outb(0x64, 0xD4);
    if (!mouse_wait(0)) return 0;
    outb(0x60, data);
    return 1;
}
static int mouse_read(unsigned char *data) {
    if (!mouse_wait(1)) return 0;
    *data = inb(0x60);
    return 1;
}
static int real_mouse_init(void) {
    unsigned char status, response;
    drain_input_buffer();
    if (!mouse_wait(0)) return 0;
    outb(0x64, 0xA8);
    if (!mouse_wait(0)) return 0;
    outb(0x64, 0x20);
    if (!mouse_wait(2)) return 0;
    status = (inb(0x60) & (unsigned char)~0x20) | 0x02;
    if (!mouse_wait(0)) return 0;
    outb(0x64, 0x60);
    if (!mouse_wait(0)) return 0;
    outb(0x60, status);
    if (!mouse_write(0xF6) || !mouse_read(&response) || response != 0xFA) return 0;
    if (!mouse_write(0xF4) || !mouse_read(&response) || response != 0xFA) return 0;
    return 1;
}
static void mouse_disable(void) {
    unsigned char response;
    if (mouse_write(0xF5)) mouse_read(&response);
    drain_input_buffer();
}

static void graphics_desktop(void) {
    static const int icon_y[5] = {160, 270, 380, 490, 600};
    graphics_desktop_background();
    graphics_text(214, 84, "Desktop", graphics_rgb(255, 255, 255), 2);
    //graphics_text(214, 118, "Click an app to open. Use EXIT TO CLI to return to the shell.",
                  //graphics_rgb(222, 232, 247), 1);
    graphics_desktop_icon(104, icon_y[0], 1, "Files");
    graphics_desktop_icon(104, icon_y[1], 2, "Editor");
    graphics_desktop_icon(104, icon_y[2], 3, "Snake");
    graphics_desktop_icon(104, icon_y[3], 4, "Terminal");
    graphics_desktop_icon(104, icon_y[4], 5, "Toolbox");
}

#define GUI_MAX_WINDOWS 5
#define GUI_FILE_ROWS 20
#define GUI_EDITOR_CAPACITY 900
#define GUI_WINDOW_FILES 1
#define GUI_WINDOW_EDITOR 2
#define GUI_WINDOW_SNAKE 3
#define GUI_WINDOW_TERMINAL 4
#define GUI_WINDOW_TOOLBOX 5

typedef struct {
    int type;
    int x, y, width, height;
    int selected;
    int save_prompt;
    unsigned int text_length;
    unsigned int text_cursor;
    unsigned int file_count;
    unsigned int file_sizes[GUI_FILE_ROWS];
    char filename[24];
    char text[GUI_EDITOR_CAPACITY];
    char files[GUI_FILE_ROWS][13];
    unsigned char file_is_directory[GUI_FILE_ROWS];
} gui_window_t;

static gui_window_t *gui_output_window;
static void gui_terminal_execute(gui_window_t *window);

static gui_window_t gui_windows[GUI_MAX_WINDOWS];
static int gui_window_count;
static int gui_active_window = -1;
static int gui_file_selected;

static void gui_refresh_files(gui_window_t *window) {
    window->file_count = 0;
    for (unsigned int i = 0; i < GUI_FILE_ROWS; i++) {
        unsigned int size;
        int is_directory;
        int found = fat32_get_current_directory_entry(i, window->files[i],
                         sizeof(window->files[i]), &size, &is_directory);
        if (found <= 0) break;
        window->file_sizes[i] = size;
        window->file_is_directory[i] = (unsigned char)is_directory;
        window->file_count++;
    }
    window->selected = 0;
}

static void gui_open_window(int type, const char *filename) {
    if (gui_window_count >= GUI_MAX_WINDOWS) return;
    gui_window_t *window = &gui_windows[gui_window_count];
    for (unsigned int i = 0; i < sizeof(*window); i++)
        ((unsigned char *)window)[i] = 0;
    window->type = type;
    window->x = 180 + gui_window_count * 36;
    window->y = 92 + gui_window_count * 32;
    window->width = type == GUI_WINDOW_FILES ? 500 :
                    type == GUI_WINDOW_SNAKE ? 500 :
                    type == GUI_WINDOW_TERMINAL ? 620 :
                    type == GUI_WINDOW_TOOLBOX ? 500 : 560;
    window->height = type == GUI_WINDOW_FILES ? 510 :
                     type == GUI_WINDOW_SNAKE ? 420 :
                     type == GUI_WINDOW_TERMINAL ? 450 :
                     type == GUI_WINDOW_TOOLBOX ? 360 : 460;
    if (filename) {
        int i;
        for (i = 0; i < (int)sizeof(window->filename) - 1 && filename[i]; i++)
            window->filename[i] = filename[i];
        window->filename[i] = '\0';
    }
    if (type == GUI_WINDOW_FILES) {
        gui_refresh_files(window);
    } else if (type == GUI_WINDOW_TERMINAL) {
        const char *welcome = "ChlorineOS graphical terminal\nType help for commands.\n> ";
        while (*welcome && window->text_length < GUI_EDITOR_CAPACITY - 1)
            window->text[window->text_length++] = *welcome++;
        window->text[window->text_length] = '\0';
        window->text_cursor = window->selected = window->text_length;
    } else if (type == GUI_WINDOW_SNAKE) {
        int columns = (window->width - 32) / 14;
        int rows = (window->height - 78) / 14;
        if (columns > 255) columns = 255;
        if (rows > 255) rows = 255;
        if (columns < 5) columns = 5;
        if (rows < 5) rows = 5;
        window->selected = 3;
        window->save_prompt = 3;
        window->file_sizes[1] = ((unsigned int)(rows / 2) << 8) | (columns / 2);
        window->file_sizes[2] = ((unsigned int)(rows / 2) << 8) | (columns / 2 - 1);
        window->file_sizes[3] = ((unsigned int)(rows / 2) << 8) | (columns / 2 - 2);
        window->file_sizes[0] = ((unsigned int)(rows / 3) << 8) | (columns / 3);
    } else if (window->filename[0]) {
        int bytes = fat32_read_file_content(window->filename, window->text,
                                             GUI_EDITOR_CAPACITY - 1);
        if (bytes > 0) {
            window->text_length = (unsigned int)bytes;
            window->text[window->text_length] = '\0';
        }
    }
    gui_active_window = gui_window_count++;
}

static void gui_raise_window(int index) {
    if (index < 0 || index >= gui_window_count) return;
    gui_window_t selected = gui_windows[index];
    for (int i = index; i < gui_window_count - 1; i++)
        gui_windows[i] = gui_windows[i + 1];
    gui_windows[gui_window_count - 1] = selected;
    gui_active_window = gui_window_count - 1;
}

static void gui_close_window(int index) {
    if (index < 0 || index >= gui_window_count) return;
    for (int i = index; i < gui_window_count - 1; i++)
        gui_windows[i] = gui_windows[i + 1];
    gui_window_count--;
    gui_active_window = gui_window_count - 1;
}

static const char *gui_window_title(gui_window_t *window) {
    if (window->type == GUI_WINDOW_FILES) return "Files";
    if (window->type == GUI_WINDOW_SNAKE) return "Snake";
    if (window->type == GUI_WINDOW_TERMINAL) return "Terminal";
    if (window->type == GUI_WINDOW_TOOLBOX) return "Toolbox";
    return window->filename[0] ? window->filename : "Editor";
}

static void gui_draw_terminal(gui_window_t *window, int x, int y, int width, int height) {
    int columns = (width - 28) / 8;
    int rows = (height - 66) / 12;
    int row = 0, column = 0, total_rows = 1;
    if (columns < 1 || rows < 1) return;
    for (unsigned int i = 0; i < window->text_length; i++) {
        if (window->text[i] == '\n' || ++column >= columns) {
            total_rows++;
            column = 0;
        }
    }
    int first_row = total_rows > rows ? total_rows - rows : 0;
    row = column = 0;
    int text_x = x + 14, text_y = y + 40;
    for (unsigned int i = 0; i < window->text_length; i++) {
        char character = window->text[i];
        if (character == '\n' || column >= columns) {
            row++;
            column = 0;
            if (character == '\n') continue;
        }
        if (row >= first_row && row < first_row + rows &&
            character >= 32 && character <= 126) {
            char glyph[2] = {character, '\0'};
            graphics_text(text_x + column * 8,
                          text_y + (row - first_row) * 12, glyph,
                          graphics_rgb(35, 45, 61), 1);
        }
        column++;
    }
    if (row >= first_row && row < first_row + rows && column < columns)
        graphics_fill(text_x + column * 8, text_y + (row - first_row) * 12 + 9,
                      7, 2, graphics_rgb(45, 90, 165));
    graphics_fill(x + 4, y + height - 23, width - 8, 19,
                  graphics_rgb(222, 229, 239));
    graphics_text(x + 12, y + height - 17, "Enter: run    Esc: close",
                  graphics_rgb(48, 61, 81), 1);
}

static void gui_draw_snake(gui_window_t *window, int x, int y, int width, int height) {
    int columns = (width - 32) / 14;
    int rows = (height - 90) / 14;
    if (columns < 5 || rows < 5) return;
    char score[12];
    unsigned int value = window->text_cursor;
    int digits = 0;
    do { score[digits++] = '0' + value % 10; value /= 10; } while (value && digits < 11);
    graphics_text(x + 14, y + 40, "Arrows: move   Enter: restart",
                  graphics_rgb(49, 66, 89), 1);
    graphics_text(x + width - 104, y + 40, "Score:",
                  graphics_rgb(49, 66, 89), 1);
    for (int i = 0; i < digits; i++) {
        char digit[2] = {score[digits - i - 1], '\0'};
        graphics_text(x + width - 56 + i * 8, y + 40, digit,
                      graphics_rgb(49, 66, 89), 1);
    }
    int board_x = x + 12, board_y = y + 58;
    graphics_fill(board_x, board_y, columns * 14, rows * 14,
                  graphics_rgb(29, 41, 58));
    for (int i = 1; i <= window->selected; i++) {
        unsigned int position = window->file_sizes[i];
        int sx = (int)(position & 0xFF), sy = (int)((position >> 8) & 0xFF);
        graphics_fill(board_x + sx * 14 + 1, board_y + sy * 14 + 1, 12, 12,
                      i == 1 ? graphics_rgb(65, 202, 119) :
                               graphics_rgb(44, 147, 94));
    }
    unsigned int food = window->file_sizes[0];
    graphics_fill(board_x + (food & 0xFF) * 14 + 2,
                  board_y + ((food >> 8) & 0xFF) * 14 + 2, 10, 10,
                  graphics_rgb(251, 110, 108));
    if (window->file_count)
        graphics_text(x + width / 2 - 28, y + height / 2,
                      "GAME OVER", graphics_rgb(157, 57, 70), 2);
}

static void gui_draw_toolbox(gui_window_t *window, int x, int y) {
    if (window->selected == 0) {
        graphics_text(x + 24, y + 58, "1. Calculator",
                      graphics_rgb(43, 56, 77), 2);
        graphics_text(x + 24, y + 104, "2. Clock and calendar",
                      graphics_rgb(43, 56, 77), 2);
        graphics_text(x + 24, y + 170, "Press 1 or 2 to choose",
                      graphics_rgb(75, 92, 116), 1);
    } else {
        graphics_text(x + 20, y + 48,
                      window->selected == 1 ? "Calculator" :
                      window->selected == 2 ? "Clock and calendar" : "Result",
                      graphics_rgb(43, 56, 77), 2);
        int text_x = x + 22, text_y = y + 94;
        for (unsigned int i = 0; i < window->text_length; i++) {
            if (window->text[i] == '\n') {
                text_x = x + 22;
                text_y += 16;
            } else if (window->text[i] >= 32 && window->text[i] <= 126) {
                char glyph[2] = {window->text[i], '\0'};
                graphics_text(text_x, text_y, glyph, graphics_rgb(35, 45, 61), 1);
                text_x += 8;
            }
        }
        if (window->selected == 1) {
            graphics_fill(text_x, text_y + 9, 7, 2, graphics_rgb(45, 90, 165));
            graphics_text(x + 22, y + 260, "Type an expression, then Enter",
                          graphics_rgb(75, 92, 116), 1);
        } else {
            graphics_text(x + 22, y + 260, "Press any key to return",
                          graphics_rgb(75, 92, 116), 1);
        }
    }
}

static void gui_draw_window(gui_window_t *window, int active) {
    int x = window->x, y = window->y, width = window->width, height = window->height;
    unsigned int border = graphics_rgb(active ? 125 : 73, active ? 177 : 105,
                                       active ? 255 : 150);
    graphics_fill(x + 6, y + 7, width, height, graphics_rgb(8, 13, 23));
    graphics_fill(x, y, width, height, border);
    graphics_fill(x + 2, y + 2, width - 4, height - 4, graphics_rgb(232, 237, 246));
    graphics_fill(x + 3, y + 3, width - 6, 28, graphics_rgb(34, 52, 79));
    graphics_fill(x + 3, y + 31, width - 6, height - 34, graphics_rgb(247, 249, 252));
    graphics_text(x + 14, y + 12, gui_window_title(window),
                  graphics_rgb(250, 252, 255), 1);

    graphics_fill(x + width - 27, y + 7, 19, 19, graphics_rgb(157, 57, 70));
    graphics_line(x + width - 22, y + 12, x + width - 13, y + 21,
                  graphics_rgb(255, 255, 255));
    graphics_line(x + width - 13, y + 12, x + width - 22, y + 21,
                  graphics_rgb(255, 255, 255));

    if (window->type == GUI_WINDOW_FILES) {
        char path[80];
        fat32_get_current_path(path, sizeof(path));
        graphics_text(x + 13, y + 42, path, graphics_rgb(43, 56, 77), 1);
        graphics_text(x + 13, y + 62, "..  (parent directory)",
                      graphics_rgb(40, 73, 129), 1);
        for (unsigned int i = 0; i < window->file_count; i++) {
            int row_y = y + 82 + (int)i * 19;
            if (row_y + 16 >= y + height - 14) break;
            if ((int)i == window->selected)
                graphics_fill(x + 9, row_y - 3, width - 18, 18,
                              graphics_rgb(213, 229, 250));
            graphics_text(x + 15, row_y, window->files[i],
                          window->file_is_directory[i] ?
                          graphics_rgb(29, 92, 149) : graphics_rgb(37, 46, 60), 1);
            if (!window->file_is_directory[i]) {
                unsigned int size = window->file_sizes[i];
                char digits[12];
                int count = 0;
                do { digits[count++] = '0' + size % 10; size /= 10; } while (size && count < 11);
                int text_x = x + width - 104;
                while (count) {
                    char digit[2] = {digits[--count], '\0'};
                    graphics_text(text_x, row_y, digit, graphics_rgb(97, 108, 124), 1);
                    text_x += 8;
                }
                graphics_text(text_x, row_y, " B", graphics_rgb(97, 108, 124), 1);
            }
        }
    } else if (window->type == GUI_WINDOW_EDITOR) {
        int content_x = x + 12;
        int content_y = y + 40;
        int chars_per_line = (width - 24) / 8;
        int max_rows = (height - 68) / 12;
        int text_x = content_x, text_y = content_y;
        int cursor_x = content_x, cursor_y = content_y;
        for (unsigned int i = 0; i <= window->text_length; i++) {
            if (i == window->text_cursor) {
                cursor_x = text_x;
                cursor_y = text_y;
            }
            if (i == window->text_length) break;
            char character = window->text[i];
            if (character == '\n' || text_x >= content_x + chars_per_line * 8) {
                text_x = content_x;
                text_y += 12;
                if (character == '\n') continue;
            }
            if (text_y < content_y + max_rows * 12 && character >= 32 && character <= 126) {
                char glyph[2] = {character, '\0'};
                graphics_text(text_x, text_y, glyph, graphics_rgb(35, 45, 61), 1);
            }
            text_x += 8;
        }
        if (cursor_y < content_y + max_rows * 12)
            graphics_fill(cursor_x, cursor_y + 9, 7, 2, graphics_rgb(45, 90, 165));
        graphics_fill(x + 4, y + height - 23, width - 8, 19,
                      graphics_rgb(222, 229, 239));
        const char *status = window->save_prompt ? "Save as: type a name, then Enter"
                           : (window->save_prompt == 2 ? "Save failed; press a key"
                           : (window->filename[0] ? "F2: save    Esc: close"
                                                  : "F2: Save As    Esc: close"));
        graphics_text(x + 12, y + height - 17, status,
                      graphics_rgb(48, 61, 81), 1);
    } else if (window->type == GUI_WINDOW_TERMINAL) {
        gui_draw_terminal(window, x, y, width, height);
    } else if (window->type == GUI_WINDOW_SNAKE) {
        gui_draw_snake(window, x, y, width, height);
    } else {
        gui_draw_toolbox(window, x, y);
    }

    graphics_fill(x + width - 12, y + height - 12, 9, 9,
                  graphics_rgb(54, 71, 96));
    graphics_line(x + width - 10, y + height - 4, x + width - 4, y + height - 10,
                  graphics_rgb(226, 233, 244));
}

static void gui_redraw(int pointer_x, int pointer_y) {
    graphics_desktop();
    for (int i = 0; i < gui_window_count; i++)
        gui_draw_window(&gui_windows[i], i == gui_active_window);
    graphics_cursor(pointer_x, pointer_y);
}

static int gui_window_at(int x, int y) {
    for (int i = gui_window_count - 1; i >= 0; i--) {
        gui_window_t *window = &gui_windows[i];
        if (x >= window->x && x < window->x + window->width &&
            y >= window->y && y < window->y + window->height) return i;
    }
    return -1;
}

static int gui_open_file_selection(gui_window_t *window, int row) {
    if (row == 0) {
        if (fat32_change_directory("..") == 0) gui_refresh_files(window);
        return 0;
    }
    unsigned int index = (unsigned int)(row - 1);
    if (index >= window->file_count) return 0;
    char name[13];
    int is_directory;
    if (fat32_get_current_directory_entry(index, name, sizeof(name), 0,
                                          &is_directory) <= 0) return 0;
    if (is_directory) {
        if (fat32_change_directory(name) == 0) gui_refresh_files(window);
    } else {
        gui_open_window(GUI_WINDOW_EDITOR, name);
    }
    return 1;
}

static void gui_save_editor(gui_window_t *window) {
    if (!window->filename[0]) {
        window->save_prompt = 1;
        return;
    }
    char *probe = (char *)0x50000;
    if (fat32_read_file_content(window->filename, probe, 1) >= 0) {
        if (fat32_overwrite_file(window->filename, window->text, window->text_length) < 0)
            window->save_prompt = 2;
        else
            window->save_prompt = 0;
    } else if (fat32_create_file(window->filename) != 0 ||
               fat32_overwrite_file(window->filename, window->text,
                                    window->text_length) < 0) {
        window->save_prompt = 2;
    } else {
        window->save_prompt = 0;
    }
    for (int i = 0; i < gui_window_count; i++)
        if (gui_windows[i].type == GUI_WINDOW_FILES) gui_refresh_files(&gui_windows[i]);
}

static void gui_editor_key(gui_window_t *window, int key) {
    if (window->save_prompt == 1) {
        if (key == 27) window->save_prompt = 0;
        else if (key == '\b') {
            unsigned int length = 0;
            while (length < sizeof(window->filename) && window->filename[length]) length++;
            if (length) window->filename[length - 1] = '\0';
        } else if (key == '\n') {
            uppercase(window->filename);
            gui_save_editor(window);
        } else if (key >= 32 && key <= 126) {
            unsigned int length = 0;
            while (length < sizeof(window->filename) && window->filename[length]) length++;
            if (length < sizeof(window->filename) - 1) {
                window->filename[length] = (char)key;
                window->filename[length + 1] = '\0';
            }
        }
        return;
    }
    if (window->save_prompt == 2) {
        window->save_prompt = 0;
        return;
    }
    if (key == 0x200 + 'F') {
        gui_save_editor(window);
        return;
    }
    if (key == 0x100 + 'L') {
        if (window->text_cursor) window->text_cursor--;
        return;
    }
    if (key == 0x100 + 'R') {
        if (window->text_cursor < window->text_length) window->text_cursor++;
        return;
    }
    if (key == 0x100 + 'U' || key == 0x100 + 'D') {
        unsigned int line_start = window->text_cursor;
        while (line_start && window->text[line_start - 1] != '\n') line_start--;
        unsigned int column = window->text_cursor - line_start;
        if (key == 0x100 + 'U') {
            if (!line_start) return;
            unsigned int previous_end = line_start - 1, previous_start = previous_end;
            while (previous_start && window->text[previous_start - 1] != '\n') previous_start--;
            window->text_cursor = previous_start + column;
            if (window->text_cursor > previous_end) window->text_cursor = previous_end;
        } else {
            unsigned int current_end = line_start;
            while (current_end < window->text_length && window->text[current_end] != '\n') current_end++;
            if (current_end >= window->text_length) return;
            unsigned int next_start = current_end + 1, next_end = next_start;
            while (next_end < window->text_length && window->text[next_end] != '\n') next_end++;
            window->text_cursor = next_start + column;
            if (window->text_cursor > next_end) window->text_cursor = next_end;
        }
        return;
    }
    if (key == '\b') {
        if (!window->text_cursor) return;
        for (unsigned int i = window->text_cursor; i < window->text_length; i++)
            window->text[i - 1] = window->text[i];
        window->text_length--;
        window->text_cursor--;
        return;
    }
    if (key != '\n' && (key < 32 || key > 126)) return;
    if (window->text_length >= GUI_EDITOR_CAPACITY - 1) return;
    for (unsigned int i = window->text_length; i > window->text_cursor; i--)
        window->text[i] = window->text[i - 1];
    window->text[window->text_cursor++] = (char)key;
    window->text_length++;
    window->text[window->text_length] = '\0';
}

static void gui_clamp_pointer(int *x, int *y) {
    if (*x < 0) *x = 0;
    if (*x >= GFX_WIDTH) *x = GFX_WIDTH - 1;
    if (*y < 0) *y = 0;
    if (*y >= GFX_HEIGHT) *y = GFX_HEIGHT - 1;
}

static int gui_hit_test(int x, int y) {
    static const int icon_y[5] = {160, 270, 380, 490, 600};
    if (x >= GFX_WIDTH - 194 && x <= GFX_WIDTH - 14 &&
        y >= GFX_HEIGHT - 41 && y <= GFX_HEIGHT - 7) return -2;
    if (x < 48 || x > 160) return 0;
    for (int i = 0; i < 5; i++)
        if (y >= icon_y[i] - 48 && y <= icon_y[i] + 58) return i + 1;
    return 0;
}

static void cmd_ls(char *arg);
static void cmd_snake(char *arg);
static void cmd_toolbox(char *arg);
static int s_rand(int max);

static void gui_terminal_write(char character) {
    gui_window_t *window = gui_output_window;
    if (!window) return;
    if (character == '\f') {
        window->text_length = window->text_cursor = window->selected = 0;
        window->text[0] = '\0';
        return;
    }
    if (character == '\r') return;
    if (character == '\b') {
        if (window->text_length > window->selected)
            window->text[--window->text_length] = '\0';
        window->text_cursor = window->text_length;
        return;
    }
    while (window->text_length >= GUI_EDITOR_CAPACITY - 1) {
        unsigned int discard = 0;
        while (discard < window->text_length &&
               window->text[discard++] != '\n') {}
        if (!discard) break;
        for (unsigned int i = discard; i <= window->text_length; i++)
            window->text[i - discard] = window->text[i];
        window->text_length -= discard;
        window->selected = window->selected > discard ?
                           window->selected - discard : 0;
        window->text_cursor = window->text_length;
    }
    window->text[window->text_length++] = character;
    window->text[window->text_length] = '\0';
    window->text_cursor = window->text_length;
}

static void gui_terminal_key(gui_window_t *window, int key) {
    if (key == '\n') {
        gui_output_window = window;
        gui_terminal_write('\n');
        gui_output_window = 0;
        gui_terminal_execute(window);
        return;
    }
    if (key == '\b') {
        if (window->text_cursor > window->selected) {
            for (unsigned int i = window->text_cursor; i < window->text_length; i++)
                window->text[i - 1] = window->text[i];
            window->text_length--;
            window->text_cursor--;
        }
        return;
    }
    if (key == 0x100 + 'L') {
        if (window->text_cursor > window->selected) window->text_cursor--;
        return;
    }
    if (key == 0x100 + 'R') {
        if (window->text_cursor < window->text_length) window->text_cursor++;
        return;
    }
    if (key < 32 || key > 126 || window->text_length >= GUI_EDITOR_CAPACITY - 1) return;
    for (unsigned int i = window->text_length; i > window->text_cursor; i--)
        window->text[i] = window->text[i - 1];
    window->text[window->text_cursor++] = (char)key;
    window->text[window->text_length + 1] = '\0';
    window->text_length++;
}

static void gui_toolbox_key(gui_window_t *window, int key) {
    if (window->selected == 0) {
        if (key == '1') {
            window->selected = 1;
            window->text_length = window->text_cursor = 0;
            window->text[0] = '\0';
        } else if (key == '2') {
            window->selected = 2;
            window->text_length = window->text_cursor = 0;
            window->text[0] = '\0';
            gui_output_window = window;
            gui_output_redirect = gui_terminal_write;
            print_current_time();
            gui_output_redirect = 0;
            gui_output_window = 0;
        }
    } else if (window->selected == 1 && key == '\n') {
        char expression[GUI_EDITOR_CAPACITY];
        unsigned int length = window->text_length;
        for (unsigned int i = 0; i < length; i++) expression[i] = window->text[i];
        expression[length] = '\0';
        window->selected = 3;
        window->text_length = window->text_cursor = 0;
        window->text[0] = '\0';
        gui_output_window = window;
        gui_output_redirect = gui_terminal_write;
        calc_eval(expression);
        gui_output_redirect = 0;
        gui_output_window = 0;
    } else if (window->selected == 1 && key == '\b') {
        if (window->text_length) {
            window->text[--window->text_length] = '\0';
            window->text_cursor = window->text_length;
        }
    } else if (window->selected == 1 &&
               ((key >= '0' && key <= '9') || key == '.' || key == ' ' ||
                key == '+' || key == '-' || key == '*' || key == '/') &&
               window->text_length < GUI_EDITOR_CAPACITY - 1) {
        window->text[window->text_length++] = (char)key;
        window->text[window->text_length] = '\0';
        window->text_cursor = window->text_length;
    } else if (window->selected != 0) {
        window->selected = 0;
        window->text_length = window->text_cursor = 0;
        window->text[0] = '\0';
    }
}

static void gui_snake_reset(gui_window_t *window) {
    int columns = (window->width - 32) / 14;
    int rows = (window->height - 90) / 14;
    int x = columns / 2, y = rows / 2;
    if (columns < 5 || rows < 5) return;
    window->selected = 3;
    window->save_prompt = 3;
    window->file_count = 0;
    window->text_cursor = 0;
    window->text_length = 0;
    window->file_sizes[1] = ((unsigned int)y << 8) | x;
    window->file_sizes[2] = ((unsigned int)y << 8) | (x - 1);
    window->file_sizes[3] = ((unsigned int)y << 8) | (x - 2);
    int food_x, food_y, collision;
    do {
        food_x = 1 + s_rand(columns - 2);
        food_y = 1 + s_rand(rows - 2);
        collision = 0;
        for (int i = 1; i <= window->selected; i++) {
            unsigned int part = window->file_sizes[i];
            if ((part & 0xFF) == (unsigned int)food_x &&
                ((part >> 8) & 0xFF) == (unsigned int)food_y)
                collision = 1;
        }
    } while (collision);
    window->file_sizes[0] = ((unsigned int)food_y << 8) | food_x;
}

static int gui_snake_tick(gui_window_t *window) {
    int columns = (window->width - 32) / 14;
    int rows = (window->height - 90) / 14;
    if (window->file_count || columns < 5 || rows < 5) return 0;
    if (++window->text_length < 2048) return 0;
    window->text_length = 0;
    unsigned int head = window->file_sizes[1];
    int x = (int)(head & 0xFF), y = (int)((head >> 8) & 0xFF);
    if (window->save_prompt == 0) y--;
    else if (window->save_prompt == 1) y++;
    else if (window->save_prompt == 2) x--;
    else x++;
    if (x <= 0 || x >= columns - 1 || y <= 0 || y >= rows - 1) {
        window->file_count = 1;
        return 1;
    }
    int ate = (window->file_sizes[0] & 0xFF) == (unsigned int)x &&
              ((window->file_sizes[0] >> 8) & 0xFF) == (unsigned int)y;
    int length = window->selected;
    for (int i = 1; i <= length; i++) {
        unsigned int part = window->file_sizes[i];
        if ((part & 0xFF) == (unsigned int)x &&
            ((part >> 8) & 0xFF) == (unsigned int)y) {
            window->file_count = 1;
            return 1;
        }
    }
    int new_length = ate && length < GUI_FILE_ROWS - 1 ? length + 1 : length;
    for (int i = new_length; i > 1; i--)
        window->file_sizes[i] = window->file_sizes[i - 1];
    window->file_sizes[1] = ((unsigned int)y << 8) | (unsigned int)x;
    window->selected = new_length;
    if (ate) {
        window->text_cursor += 10;
        int food_x, food_y, collision;
        do {
            food_x = 1 + s_rand(columns - 2);
            food_y = 1 + s_rand(rows - 2);
            collision = 0;
            for (int i = 1; i <= new_length; i++) {
                unsigned int part = window->file_sizes[i];
                if ((part & 0xFF) == (unsigned int)food_x &&
                    ((part >> 8) & 0xFF) == (unsigned int)food_y)
                    collision = 1;
            }
        } while (collision);
        window->file_sizes[0] = ((unsigned int)food_y << 8) | food_x;
    }
    return 1;
}

static void gui_snake_key(gui_window_t *window, int key) {
    if (window->file_count && key == '\n') {
        gui_snake_reset(window);
        return;
    }
    if (key == 0x100 + 'U' && window->save_prompt != 1) window->save_prompt = 0;
    else if (key == 0x100 + 'D' && window->save_prompt != 0) window->save_prompt = 1;
    else if (key == 0x100 + 'L' && window->save_prompt != 3) window->save_prompt = 2;
    else if (key == 0x100 + 'R' && window->save_prompt != 2) window->save_prompt = 3;
}

static void cmd_gui(char *arg) {
    (void)arg;
    clock_stop();
    if (!video_set_mode(0x4118)) {
        print("GUI unavailable: VBE mode switch failed.\n");
        return;
    }
    if (!graphics_init()) {
        if (video_set_mode(0x0003)) graphics_ready = 0;
        print("GUI unavailable: invalid VBE framebuffer.\n");
        return;
    }
    clear_screen();
    int v_mx = 104, v_my = 160;
    int mouse_left_pressed = 0;
    int mouse_enabled = real_mouse_init();
    int drag_mode = 0;
    int drag_window = -1;
    int keyboard_extended = 0;
    unsigned char mouse_bytes[3];
    int mouse_cycle = 0;
    gui_window_count = 0;
    gui_active_window = -1;
    gui_redraw(v_mx, v_my);

    while (1) {
        int trigger_app = 0;

        unsigned char status = inb(0x64);
        if (status & 1) {
            unsigned char data = inb(0x60);
            if (status & 0x20) {
                if (mouse_enabled) {
                    switch (mouse_cycle) {
                        case 0: if (data & 0x08) { mouse_bytes[0] = data; mouse_cycle++; } break;
                        case 1: mouse_bytes[1] = data; mouse_cycle++; break;
                        case 2:
                            mouse_bytes[2] = data;
                            mouse_cycle = 0;
                            if (!(mouse_bytes[0] & 0xC0)) {
                                int dx = mouse_bytes[1], dy = mouse_bytes[2];
                                if (mouse_bytes[0] & 0x10) dx -= 256;
                                if (mouse_bytes[0] & 0x20) dy -= 256;
                                v_mx += dx * 3;
                                v_my -= dy * 3;
                                gui_clamp_pointer(&v_mx, &v_my);
                                int left_pressed = (mouse_bytes[0] & 1) != 0;

                                if (left_pressed && !mouse_left_pressed) {
                                    int index = gui_window_at(v_mx, v_my);
                                    if (index >= 0) {
                                        gui_raise_window(index);
                                        gui_window_t *window = &gui_windows[gui_active_window];
                                        if (v_mx >= window->x + window->width - 27 &&
                                            v_my < window->y + 31) {
                                            gui_close_window(gui_active_window);
                                        } else {
                                            int left = v_mx < window->x + 12;
                                            int right = v_mx >= window->x + window->width - 12;
                                            int top = v_my < window->y + 12;
                                            int bottom = v_my >= window->y + window->height - 12;
                                            drag_window = gui_active_window;
                                            if ((left || right) && (top || bottom))
                                                drag_mode = (left ? 1 : 2) | (top ? 4 : 8);
                                            else if (v_my < window->y + 31)
                                                drag_mode = 16;
                                            else if (window->type == GUI_WINDOW_FILES &&
                                                     v_my >= window->y + 58 &&
                                                     v_my < window->y + 80) {
                                                if (fat32_change_directory("..") == 0)
                                                    gui_refresh_files(window);
                                            } else if (window->type == GUI_WINDOW_FILES &&
                                                       v_my >= window->y + 80) {
                                                int row = (v_my - window->y - 80) / 19;
                                                if (row >= 0 && row < GUI_FILE_ROWS) {
                                                    window->selected = row;
                                                    gui_open_file_selection(window, row + 1);
                                                }
                                            } else if (window->type == GUI_WINDOW_TOOLBOX &&
                                                       window->selected == 0 &&
                                                       v_my < window->y + 160) {
                                                gui_toolbox_key(window,
                                                    v_my < window->y + 98 ? '1' : '2');
                                            }
                                        }
                                    } else {
                                        int app = gui_hit_test(v_mx, v_my);
                                        if (app == -2) trigger_app = -2;
                                        else if (app == 1) gui_open_window(GUI_WINDOW_FILES, 0);
                                        else if (app == 2) gui_open_window(GUI_WINDOW_EDITOR, 0);
                                        else if (app == 3) gui_open_window(GUI_WINDOW_SNAKE, 0);
                                        else if (app == 4) gui_open_window(GUI_WINDOW_TERMINAL, 0);
                                        else if (app == 5) gui_open_window(GUI_WINDOW_TOOLBOX, 0);
                                    }
                                } else if (left_pressed && drag_mode &&
                                           drag_window >= 0 && drag_window < gui_window_count) {
                                    gui_window_t *window = &gui_windows[drag_window];
                                    if (drag_mode == 16) {
                                        window->x += dx * 3;
                                        window->y -= dy * 3;
                                        if (window->x < 0) window->x = 0;
                                        if (window->y < 44) window->y = 44;
                                        if (window->x + window->width > GFX_WIDTH)
                                            window->x = GFX_WIDTH - window->width;
                                        if (window->y + window->height > GFX_HEIGHT - 48)
                                            window->y = GFX_HEIGHT - 48 - window->height;
                                    } else {
                                        int move_x = dx * 3, move_y = -dy * 3;
                                        if (drag_mode & 1) {
                                            window->x += move_x;
                                            window->width -= move_x;
                                        } else if (drag_mode & 2) window->width += move_x;
                                        if (drag_mode & 4) {
                                            window->y += move_y;
                                            window->height -= move_y;
                                        } else if (drag_mode & 8) window->height += move_y;
                                        if (window->width < 280) window->width = 280;
                                        if (window->height < 220) window->height = 220;
                                        if (window->x < 0) window->x = 0;
                                        if (window->y < 44) window->y = 44;
                                        if (window->x + window->width > GFX_WIDTH)
                                            window->width = GFX_WIDTH - window->x;
                                        if (window->y + window->height > GFX_HEIGHT - 48)
                                            window->height = GFX_HEIGHT - 48 - window->y;
                                    }
                                }
                                if (!left_pressed) {
                                    drag_mode = 0;
                                    drag_window = -1;
                                }
                                mouse_left_pressed = left_pressed;
                                gui_redraw(v_mx, v_my);
                            }
                            break;
                    }
                }
            }
            else if (data == 0x2A || data == 0x36) shift_pressed = 1;
            else if (data == 0xAA || data == 0xB6) shift_pressed = 0;
            else if (data == 0x3A && !(data & 0x80)) caps_lock = !caps_lock;
            else if (data == 0xE0) keyboard_extended = 1;
            else {
                int extended = keyboard_extended;
                keyboard_extended = 0;
                if (!(data & 0x80)) {
                int key = 0;
                if (extended) {
                    if (data == 0x48) key = 0x100 + 'U';
                    else if (data == 0x50) key = 0x100 + 'D';
                    else if (data == 0x4B) key = 0x100 + 'L';
                    else if (data == 0x4D) key = 0x100 + 'R';
                } else if (data == 0x01) key = 27;
                else if (data == 0x3C) key = 0x200 + 'F';
                else key = scancode_to_ascii(data);

                if (key == 27 && gui_active_window >= 0)
                    gui_close_window(gui_active_window);
                else if (gui_active_window >= 0) {
                    gui_window_t *window = &gui_windows[gui_active_window];
                    if (window->type == GUI_WINDOW_EDITOR) gui_editor_key(window, key);
                    else if (window->type == GUI_WINDOW_TERMINAL)
                        gui_terminal_key(window, key);
                    else if (window->type == GUI_WINDOW_SNAKE)
                        gui_snake_key(window, key);
                    else if (window->type == GUI_WINDOW_TOOLBOX)
                        gui_toolbox_key(window, key);
                    else if (key == 0x100 + 'U' && window->selected > 0)
                        window->selected--;
                    else if (key == 0x100 + 'D' &&
                             window->selected + 1 < (int)window->file_count)
                        window->selected++;
                    else if ((key == '\n' || key == ' ') && window->file_count)
                        gui_open_file_selection(window, window->selected + 1);
                } else if (key == 0x100 + 'U') v_my -= 24;
                else if (key == 0x100 + 'D') v_my += 24;
                else if (key == 0x100 + 'L') v_mx -= 24;
                else if (key == 0x100 + 'R') v_mx += 24;
                else if ((key == '\n' || key == ' ') && gui_hit_test(v_mx, v_my) == 1)
                    gui_open_window(GUI_WINDOW_FILES, 0);
                else if ((key == '\n' || key == ' ') && gui_hit_test(v_mx, v_my) == 2)
                    gui_open_window(GUI_WINDOW_EDITOR, 0);
                else if ((key == '\n' || key == ' ') && gui_hit_test(v_mx, v_my) == 3)
                    gui_open_window(GUI_WINDOW_SNAKE, 0);
                else if ((key == '\n' || key == ' ') && gui_hit_test(v_mx, v_my) == 4)
                    gui_open_window(GUI_WINDOW_TERMINAL, 0);
                else if ((key == '\n' || key == ' ') && gui_hit_test(v_mx, v_my) == 5)
                    gui_open_window(GUI_WINDOW_TOOLBOX, 0);
                else if ((key == '\n' || key == ' ') && gui_hit_test(v_mx, v_my) == -2)
                    trigger_app = -2;
                gui_clamp_pointer(&v_mx, &v_my);
                gui_redraw(v_mx, v_my);
                }
            }
        }

        if (trigger_app == -2) {
            if (mouse_enabled) mouse_disable();
            if (!video_set_mode(0x0003)) {
                graphics_text(350, 370, "Could not restore VGA text mode",
                              graphics_rgb(255, 220, 220), 2);
                mouse_enabled = real_mouse_init();
                continue;
            }
            graphics_ready = 0;
            clear_screen();
            print("Returned to VGA text CLI. Type 'shut' to power off.\n");
            return;
        }

        if (!trigger_app) {
            int redraw = 0;
            for (int i = 0; i < gui_window_count; i++)
                if (gui_windows[i].type == GUI_WINDOW_SNAKE &&
                    gui_snake_tick(&gui_windows[i])) redraw = 1;
            if (redraw) {
                gui_redraw(v_mx, v_my);
                continue;
            }
            for (volatile int delay = 0; delay < 10000; delay++);
            continue;
        }
        if (mouse_enabled) mouse_disable();
        clear_screen();
        drain_input_buffer();
        mouse_left_pressed = 1;
        mouse_enabled = real_mouse_init();
        gui_redraw(v_mx, v_my);
    }
}

/* =============== 贪吃蛇 =============== */
#define SNAKE_MAP_W 60
#define SNAKE_MAP_H 20
static unsigned int s_seed = 54321;
static int s_rand(int max) { s_seed = s_seed * 1103515245 + 12345; return ((unsigned int)(s_seed / 65536) % 32768) % max; }
static void s_draw(int x, int y, unsigned char c, unsigned char col) {
    put_text_cell(y * VGA_WIDTH + x, (col << 8) | c);
}
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
    ide_init();
    
    // ===== 直接挂载整个硬盘（不检查 MBR） =====
    int ret = fat32_init(0);
    
    if (ret == 0) {
        first_part_lba = 0;
        fat32_mounted = 1;
        print("FAT32 mounted successfully!\n");
        print("Root directory contents:\n");
        fat32_list_root();
        return;
    } else {
        print("fat32_init(0) failed with error: ");
        print_int(ret);
        print("\n");
        
        // 尝试第二种方法：直接读取 MBR
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
    for (int y = 0; y < VGA_HEIGHT; y++) {
        int line = terminal_view + y;
        if (line >= TERMINAL_HISTORY_LINES) continue;
        for (int x = 0; x < VGA_WIDTH; x++)
            terminal_history[line][x] =
                (unsigned short)(vga_attr << 8) | (terminal_history[line][x] & 0xFF);
    }
    terminal_render();
}

static void cmd_toolbox(char *arg) {
    while(1) {
        clear_screen();
        set_color(0x0B, 0x00);
        print("       ChlorineOS_OS Multi-Toolbox       \n");
        set_color(0x07, 0x00);
        print("1. Calculator \n");
        print("2. Clock and Calendar\n");
        print("ESC. Exit\n");
        
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
    set_color(0x09,0x00); print("ChlorineOS (v26.1.03)\n");
    set_color(0x07,0x00); print("Engine: VBE engine\n\n");
    set_color(0x09,0x00);
    print(" 0000000000000000000000000000   000000000000000000000\n");
    print("  00000000000000000000000000   0000000000000000000000\n");
    print("   000000000000000000000000   00000000000000000000000\n");
    print("               00000000000   0000000000   0000000000 \n");
    print("              00000000000   0000000000   0000000000  \n");
    print("             00000000000   0000000000   0000000000   \n");
    print("            00000000000   0000000000   0000000000    \n");
    print("           00000000000   0000000000   0000000000     \n");
    print("          00000000000   0000000000   0000000000      \n");
    print("         00000000000   0000000000   0000000000       \n");
    print("        00000000000   0000000000   0000000000        \n");
    print("       00000000000   0000000000   0000000000  ");
    set_color(0x07,0x00);
    print("Architecture: x86 (32-bit)\n");
    set_color(0x09,0x00);
    print("      00000000000   0000000000   0000000000   ");
    set_color(0x07,0x00);
    print("Boot Device : Floppy (cl_os.img)\n");
    set_color(0x09,0x00);
    print("     00000000000   0000000000   0000000000    ");
    set_color(0x07,0x00);
    print("Data Disk   : hdd.img (FAT32)\n");
    set_color(0x09,0x00);
    print("      000000000     00000000     00000000     ");
    set_color(0x07,0x00);
    print("Memory      : 64 MB\n");
    set_color(0x09,0x00);
    print("       0000000       000000       000000      ");
    set_color(0x07,0x00);
    print("Date        : ");
    print(__DATE__); print("\n\n");
    print("                                Please visit\n");
    set_color(0x0A,0x00); print("                gz1012a.xyz/sys");
    set_color(0x07,0x00); print(" or ");
    set_color(0x0A,0x00); print("github.com/GeorgeZ787/Cl_OS\n");
    set_color(0x07,0x00); print("                            for more information\n\n");
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
    {"shutdown","Shutdown VM",       cmd_shutdown},
    {"testmem", "Test kmalloc",         cmd_test_mem},
    {"cls",     "clear screen",         cmd_cls},
    {NULL, NULL, NULL}
};

static void gui_terminal_execute(gui_window_t *window) {
    char command[GUI_EDITOR_CAPACITY];
    unsigned int length = window->text_length > window->selected ?
                          window->text_length - window->selected - 1 : 0;
    if (length >= sizeof(command)) length = sizeof(command) - 1;
    for (unsigned int i = 0; i < length; i++)
        command[i] = window->text[window->selected + i];
    command[length] = '\0';
    window->selected = window->text_length;

    char *name = command;
    while (*name == ' ') name++;
    char *argument = name;
    while (*argument && *argument != ' ') argument++;
    if (*argument) {
        *argument++ = '\0';
        while (*argument == ' ') argument++;
    }

    gui_output_window = window;
    gui_output_redirect = gui_terminal_write;
    if (*name) {
        int found = 0;
        for (int i = 0; cmd_table[i].name; i++) {
            const char *registered = cmd_table[i].name;
            char *typed = name;
            while (*typed && *registered && *typed == *registered) {
                typed++;
                registered++;
            }
            if (*typed || *registered) continue;
            found = 1;
            if (cmd_table[i].func == cmd_gui || cmd_table[i].func == cmd_snake ||
                cmd_table[i].func == cmd_toolbox || cmd_table[i].func == cmd_edit ||
                cmd_table[i].func == cmd_shutdown || cmd_table[i].func == cmd_color) {
                print("Use the desktop icon or VGA CLI for this command.\n");
            } else {
                cmd_table[i].func(argument);
            }
            break;
        }
        if (!found) {
            print("Unknown command: ");
            print(name);
            print("\n");
        }
    }
    print("> ");
    gui_output_redirect = 0;
    gui_output_window = 0;
    window->selected = window->text_cursor = window->text_length;
}

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