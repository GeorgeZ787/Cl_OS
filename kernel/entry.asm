[bits 32]
global _start
extern kernel_main
extern __bss_start
extern __bss_end

_start:
    mov esp, 0x90000
    mov ebp, esp

    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    cld
    rep stosb

    call kernel_main

    cli
    hlt
    jmp $

global video_set_mode
video_set_mode:
    push ebx
    push esi
    push edi
    push ebp
    mov eax, [esp + 20]
    mov [video_requested_mode], eax
    pushfd
    pop eax
    mov [video_saved_flags], eax
    mov [video_saved_esp], esp
    sidt [video_saved_idtr]
    cli
    lgdt [video_gdtr]
    db 0xEA
    dd video_pm16_entry - _start
    dw 0x0018

[bits 16]
video_pm16_entry:
    mov eax, cr0
    and eax, 0xFFFFFFFE
    mov cr0, eax
    db 0xEA
    dw video_real_entry - _start
    dw 0x1000

video_real_entry:
    mov ax, 0x1000
    mov ds, ax
    mov es, ax
    xor ax, ax
    mov ss, ax
    mov sp, 0x7000

    lidt [cs:video_real_idtr - _start]
    mov word [video_result - _start], 0
    mov ax, [video_requested_mode - _start]
    cmp ax, 0x0003
    je .set_text_mode
    mov ax, [video_requested_mode - _start]
    mov bx, ax
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne .restore_protected_mode
    jmp .mode_set
.set_text_mode:
    mov ax, 0x0003
    int 0x10
    ; Reapply mode 3 so BIOS returns to the standard 80-column VGA layout.
    mov ax, 0x0003
    int 0x10
.text_mode_set:
    mov ax, 0x0200
    xor bx, bx
    xor dx, dx
    int 0x10
    mov ax, 0x0106
    mov cx, 0x0607
    int 0x10
.mode_set:
    mov ax, 0x1000
    mov ds, ax
    mov word [video_result - _start], 1
.restore_protected_mode:
    cli
    o32 lidt [cs:video_saved_idtr - _start]
    lgdt [cs:video_gdtr - _start]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    db 0x66, 0xEA
    dd video_protected_entry
    dw 0x0008

[bits 32]
video_protected_entry:
    mov ax, 0x0010
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, [video_saved_esp]
    pop ebp
    pop edi
    pop esi
    pop ebx
    movzx eax, word [video_result]
    push dword [video_saved_flags]
    popfd
    ret

align 8
video_gdt:
    dq 0
    dq 0x00CF9A000000FFFF
    dq 0x00CF92000000FFFF
    dq 0x00009A010000FFFF
video_gdt_end:
video_gdtr:
    dw video_gdt_end - video_gdt - 1
    dd video_gdt

align 4
video_requested_mode: dd 0
video_saved_flags: dd 0
video_saved_esp: dd 0
video_result: dw 0
video_saved_idtr: times 6 db 0
video_real_idtr:
    dw 0x03FF
    dd 0