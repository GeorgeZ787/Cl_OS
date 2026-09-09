; boot/loader.asm
[org 0x8000]
[bits 16]
start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x8000

    mov si, msg_loader
    call print16

    ; ===== 关闭中断！ =====
    cli
    ; ======================

    ; 开启 A20 (BIOS 方法)
    mov ax, 0x2401
    int 0x15

    ; 加载 GDT
    lgdt [gdtdesc]

    ; 进入保护模式
    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ; 远跳转清空预取队列
    jmp 0x08:pmode

[bits 32]
pmode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    ; 显示 "P" 确认进入保护模式
    mov word [0xB8000], 0x0F50   ; 'P'

    ; 跳转到内核
    jmp 0x10000

; -------- 16位打印 --------
[bits 16]
print16:
    push ax
    push si
.loop:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0e
    int 0x10
    jmp .loop
.done:
    pop si
    pop ax
    ret

msg_loader db "L", 13, 10, 0

; -------- GDT --------
align 8
gdt_start:
    dq 0
gdt_code:
    dw 0xFFFF
    dw 0
    db 0
    db 10011010b
    db 11001111b
    db 0
gdt_data:
    dw 0xFFFF
    dw 0
    db 0
    db 10010010b
    db 11001111b
    db 0
gdt_end:
gdtdesc:
    dw gdt_end - gdt_start - 1
    dd gdt_start

times 512-($-$$) db 0