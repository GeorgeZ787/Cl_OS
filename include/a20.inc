; include/a20.inc - 打开 A20 地址线

; 方法 1: 通过 0x92 端口 (简单，但某些虚拟机不支持)
enable_a20_fast:
    in al, 0x92
    or al, 2
    out 0x92, al
    ret

; 方法 2: 通过键盘控制器 (更通用)
enable_a20_kb:
    cli                 ; 关中断
    call empty_8042     ; 等待键盘缓冲区空
    mov al, 0xAD        ; 禁用键盘
    out 0x64, al

    call empty_8042
    mov al, 0xD0        ; 读取输入端口
    out 0x64, al

    call empty_8042
    in al, 0x60         ; 读取返回值
    push eax

    call empty_8042
    mov al, 0xD1        ; 写入输出端口
    out 0x64, al

    call empty_8042
    pop eax
    or al, 2            ; 设置 A20 位
    out 0x60, al

    call empty_8042
    mov al, 0xAE        ; 启用键盘
    out 0x64, al

    call empty_8042
    sti                 ; 开中断
    ret

; 辅助函数：等待 8042 缓冲区空
empty_8042:
    in al, 0x64
    test al, 2          ; 输入缓冲区是否满？
    jnz empty_8042
    ret

; 主函数：尝试多种方法
enable_a20:
    call check_a20
    jc .a20_enabled     ; 如果已开启，跳过

    call enable_a20_fast
    call check_a20
    jc .a20_enabled

    call enable_a20_kb
    call check_a20
    jc .a20_enabled

    ; 如果都失败，死循环
    mov si, a20_fail_msg
    call print_string
    jmp $

.a20_enabled:
    ret

; 检查 A20 是否已开启
check_a20:
    push ds
    push es
    push di
    push si

    ; 尝试写 0x0000:0x7DFE 和 0xFFFF:0x7DFE
    ; 如果 A20 关闭，这两个地址会映射到同一物理地址
    mov ax, 0x0000
    mov ds, ax
    mov ax, 0xFFFF
    mov es, ax

    mov di, 0x7DFE
    mov si, 0x7DFE

    mov byte [si], 0x00
    mov byte [di], 0xFF

    cmp byte [si], 0xFF
    je .a20_on

    clc     ; CF=0 表示 A20 关闭
    jmp .done

.a20_on:
    stc     ; CF=1 表示 A20 开启

.done:
    pop si
    pop di
    pop es
    pop ds
    ret

; 数据
a20_fail_msg db 'A20 Failed!', 0