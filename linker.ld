ENTRY(kernel_main)
SECTIONS
{
    . = 0x100000;          /* 内核加载到 1MB */
    .text : { *(.text) }
    .data : { *(.data) }
    .bss : { *(.bss) }
}