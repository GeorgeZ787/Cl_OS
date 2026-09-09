[bits 32]
global _start
extern kernel_main

_start:
    mov esp, 0x30000
    mov ebp, esp

    ; 显示 "K" 确认进入内核
    mov word [0xB8002], 0x0F4B   ; 'K'

    call kernel_main

    cli
    hlt
    jmp $