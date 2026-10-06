[bits 32]
global _start
global vbe_set_mode_runtime
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

; Runtime BIOS bridge. The kernel runs in 32-bit protected mode, so BIOS
; services must only be called after temporarily returning to real mode.
vbe_set_mode_runtime:
    pushad
    mov eax, [esp + 36]
    mov [0x8FE0], eax
    mov [0x8FE4], esp
    mov eax, cr0
    and eax, 0xFFFFFFFE
    mov cr0, eax
    jmp 0x1000:(runtime_real_entry - 0x10000)

[bits 16]
runtime_real_entry:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov sp, 0xFF00
    ; Keep interrupts disabled: the current IDT is a protected-mode IDT,
    ; and using it while executing BIOS real-mode code can hang the machine.

    mov ax, [0x8FE0]
    cmp ax, 0x0003
    je runtime_text_mode

    ; Prefer the known 1024x768x16 mode, then try the alternate VBE mode.
    mov cx, 0x0117
    call runtime_try_vbe_mode
    jc runtime_try_mode_118
    jmp runtime_return_protected

runtime_try_mode_118:
    mov cx, 0x0118
    call runtime_try_vbe_mode
    jc runtime_vbe_failed
    jmp runtime_return_protected

runtime_try_vbe_mode:
    push cx
    mov ax, 0x4F01
    mov di, 0x9200
    int 0x10
    cmp ax, 0x004F
    jne .fail_pop

    mov ax, 0x4F02
    pop cx
    mov bx, cx
    or bx, 0x4000
    int 0x10
    cmp ax, 0x004F
    jne .fail

    mov eax, [0x9228]
    mov [0x9000], eax
    xor eax, eax
    mov ax, [0x9210]
    mov [0x9004], eax
    xor eax, eax
    mov ax, [0x9212]
    mov [0x9008], eax
    xor eax, eax
    mov ax, [0x9214]
    mov [0x900A], eax
    mov ax, [0x9219]
    mov [0x900C], al
    mov byte [0x900D], 1
    clc
    ret

.fail:
    stc
    ret

.fail_pop:
    pop cx
    stc
    ret

runtime_vbe_failed:
    mov dword [0x9000], 0
    mov byte [0x900D], 0
    jmp runtime_return_protected

runtime_text_mode:
    mov ax, 0x0003
    int 0x10
    mov dword [0x9000], 0
    mov word [0x9004], 160
    mov word [0x9008], 80
    mov word [0x900A], 25
    mov byte [0x900C], 0
    mov byte [0x900D], 0
    jmp runtime_return_protected

runtime_return_protected:
    cli
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    ; The CPU is still decoding 16-bit instructions until this jump.
    ; Assemble the far jump with a 32-bit offset so the 0x10000 kernel
    ; address does not produce an R_386_16 relocation.
[bits 32]
    jmp 0x08:runtime_protected_return

runtime_protected_return:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, [0x8FE4]
    popad
    ret