; include/print.inc - 16位实模式打印

print_string:
    push ax
    push si
.loop:
    lodsb               ; 从 [SI] 加载字符到 AL
    or al, al           ; 检查是否为 0
    jz .done
    mov ah, 0x0E        ; BIOS teletype 输出
    int 0x10            ; 调用 BIOS
    jmp .loop
.done:
    pop si
    pop ax
    ret

; 用法示例：
;   mov si, msg
;   call print_string
;
; msg db 'Hello', 0