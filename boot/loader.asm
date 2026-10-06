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

    ; 开启 A20 (BIOS 方法)
    mov ax, 0x2401
    int 0x15
    xor ax, ax
    mov ds, ax
    mov es, ax

    ; Request a 1024x768x24 VBE linear framebuffer.
    mov ax, 0x4F01
    mov cx, 0x0118
    mov di, 0x5000
    int 0x10
    cmp ax, 0x004F
    jne vbe_error
    xor ax, ax
    mov ds, ax
    mov ax, [0x5000]
    and ax, 0x0081
    cmp ax, 0x0081
    jne vbe_error
    cmp word [0x5012], 1024
    jne vbe_error
    cmp word [0x5014], 768
    jne vbe_error
    cmp byte [0x5019], 24
    jne vbe_error
    cmp byte [0x501B], 6
    jne vbe_error
    cmp word [0x5010], 3072
    jb vbe_error
    cmp dword [0x5028], 0
    je vbe_error

    ; Keep the BIOS VGA text mode active until the GUI requests VBE.
    ; Copy the BIOS 8x8 font for rendering text in the graphical desktop.
    mov ax, 0x1130
    mov bh, 0x03
    int 0x10
    push ds
    push es
    push si
    push di
    push cx
    mov si, bp
    push es
    pop ds
    xor ax, ax
    mov es, ax
    mov di, 0x6000
    mov cx, 2048
    cld
    rep movsb
    pop cx
    pop di
    pop si
    pop es
    pop ds

    cli

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

    ; 跳转到内核
    jmp 0x10000

vbe_error:
    xor ax, ax
    mov ds, ax
    mov si, msg_vbe_error
    call print16
    cli
.halt:
    hlt
    jmp .halt

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
msg_vbe_error db "VBE 1024x768x24 unavailable", 13, 10, 0

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