[org 0x7c00]
[bits 16]
start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00

    ; 保存驱动器号
    mov [drive], dl

    mov si, msg_boot
    call print

    ; 重置软盘
    mov ah, 0x00
    mov dl, [drive]
    int 0x13
    jc error

    ; 加载 loader (1 扇区，LBA=1)
    mov ax, 0x0000
    mov es, ax
    mov bx, 0x8000
    mov ah, 0x02
    mov al, 1
    mov ch, 0
    mov cl, 2           
    mov dh, 0
    mov dl, [drive]
    int 0x13
    jc error

    ; 加载内核到 0x10000 
    ; 第1块
    mov ax, 0x1000
    mov es, ax
    xor bx, bx
    mov ah, 0x02
    mov al, 16
    mov ch, 0
    mov cl, 3
    mov dh, 0
    mov dl, [drive]
    int 0x13
    jc error

    ; 第2块
    mov ax, 0x1200      ; 0x1000 + (16 * 0x20) = 0x1200
    mov es, ax
    xor bx, bx
    mov ah, 0x02
    mov al, 18
    mov ch, 0
    mov cl, 1
    mov dh, 1          ; 磁头 1
    mov dl, [drive]
    int 0x13
    jc error

    ; 第3块
    mov ax, 0x1440      ; 0x1200 + (18 * 0x20) = 0x1440
    mov es, ax
    xor bx, bx
    mov ah, 0x02
    mov al, 18
    mov ch, 1
    mov cl, 1
    mov dh, 0
    mov dl, [drive]
    int 0x13
    jc error

    ; 第4块
    mov ax, 0x1680      ; 0x1440 + (18 * 0x20) = 0x1680
    mov es, ax
    xor bx, bx
    mov ah, 0x02
    mov al, 18
    mov ch, 1
    mov cl, 1
    mov dh, 1          ; 磁头 1
    mov dl, [drive]
    int 0x13
    jc error

    ; 第5块
    mov ax, 0x18C0      ; 0x1680 + (18 * 0x20) = 0x18C0
    mov es, ax
    xor bx, bx
    mov ah, 0x02
    mov al, 18
    mov ch, 2          ; 柱面 2
    mov cl, 1          ; 扇区 1开始
    mov dh, 0          ; 磁头 0
    mov dl, [drive]
    int 0x13
    jc error

    ; 第6块
    mov ax, 0x1B00      ; 0x18C0 + (18 * 0x20) = 0x1B00
    mov es, ax
    xor bx, bx
    mov ah, 0x02
    mov al, 18
    mov ch, 2          ; 柱面 2
    mov cl, 1          ; 扇区 1开始
    mov dh, 1          ; 磁头 1
    mov dl, [drive]
    int 0x13
    jc error

    ; 跳转到 loader
    jmp 0x0000:0x8000

error:
    mov si, msg_err
    call print
    jmp $

print:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0e
    int 0x10
    jmp print
.done:
    ret

msg_boot db "B", 13, 10, 0
msg_err  db "ERR", 0
drive    db 0

times 510-($-$$) db 0
dw 0xAA55