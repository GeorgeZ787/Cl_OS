; include/gdt.inc - 全局描述符表

; GDT Null Descriptor
gdt_null:
    dd 0
    dd 0

; GDT Code Segment (32-bit, 4GB)
gdt_code:
    dw 0xFFFF           ; Limit (0-15)
    dw 0                ; Base (0-15)
    db 0                ; Base (16-23)
    db 10011010b        ; P=1, DPL=0, S=1, Type=Code (可执行, 可读)
    db 11001111b        ; G=1, D/B=1, L=0, AVL=0, Limit (16-19)
    db 0                ; Base (24-31)

; GDT Data Segment (32-bit, 4GB)
gdt_data:
    dw 0xFFFF           ; Limit (0-15)
    dw 0                ; Base (0-15)
    db 0                ; Base (16-23)
    db 10010010b        ; P=1, DPL=0, S=1, Type=Data (可写)
    db 11001111b        ; G=1, D/B=1, L=0, AVL=0, Limit (16-19)
    db 0                ; Base (24-31)

gdt_end:

; GDT 描述符 (提供给 LGDT)
gdt_descriptor:
    dw gdt_end - gdt_null - 1   ; GDT 大小
    dd gdt_null                 ; GDT 起始地址

; 段选择子 (在代码中使用)
CODE_SEL equ gdt_code - gdt_null
DATA_SEL equ gdt_data - gdt_null