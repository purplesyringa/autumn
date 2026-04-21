    start equ 0x400000
    output_stream equ 0x2401000

    bits 64
    org start

    db 0x7f, "ELF"
    db 2 ; 64-bit
    db 1 ; little-endian
    db 1 ; version
    db 3 ; Linux ABI
    db 0 ; ABI version, ignored
    times 7 db 0 ; padding
    dw 2 ; ET_EXEC
    dw 0x3e ; x86-64
    dd 1 ; version
    dq entry ; entry point
    dq program_header - $$ ; program header
    dq 0 ; section header, ignored
    dd 0 ; flags
    dw elf_header_end - $$ ; header size
    dw program_header_end - program_header ; program header entry size
    dw 1 ; number of program header entries
    dw 0 ; section header entry size
    dw 0 ; number of section header entries
    dw 0 ; .shstrtab section index
elf_header_end:

program_header:
    dd 1 ; PT_LOAD
    dd 7 ; rwx
    dq 0 ; file offset
    dq $$ ; virtual address
    dq 0 ; physical address
    dq input_stream_end - $$ ; file size
    dq 64 * 1024 * 1024 ; memory size
    dq 0 ; alignment
program_header_end:

entry:
    xor r9d, r9d ; current output byte index
    xor r10d, r10d ; current input bit index
    xor ebx, ebx ; (prev_bytes << 8) | next_byte
    mov r11d, 0x80000000 ; range
    mov r12d, initial ; value

decode_byte:
    shl rbx, 8
    inc bl

decode_bit:
    push 1
    pop rdi ; c0
    mov ebp, edi ; c1

    lea esi, [rel models]
    mov ecx, models_end - models

query_model:
    ; Load 8-bit mask
    lodsb

    ; Extend mask to 64-bit
    mov r8, 0x0101010101010101
    pdep r8, rax, r8
    imul r8, 0xff

    ; Apply mask to `prev_bytes`
    and r8, rbx

    ; Compute hash table entry address
    xor edx, edx
    crc32 rdx, r8
    xor dl, al
    and edx, 16 * 1024 * 1024 - 1
    lea edx, [rdx * 2 + hash_table]

    ; Save address for later adjustment
    push rdx

    ; Accumulate c0/c1
    mov ax, [rdx]
    movzx edx, ah
    movzx eax, al
    add edi, edx
    add ebp, eax

    loop query_model

    ; We now know c0 and c1 

    add ebp, edi ; c1 += c0

    ; mid = range * c0 / (c0 + c1)
    mov eax, r11d
    mul edi
    div ebp

    xor edx, edx
    cmp r12d, eax
    setae dl
    jae bit1

    ; bit = 0, x < mid
    mov r11d, eax
    jmp check_precision

bit1:
    ; bit = 1, x >= mid
    sub r11d, eax
    sub r12d, eax

check_precision:
    test r11d, r11d
    js bit_done

    ; Increase precision
    lea eax, [rel input_stream]
    bt [rax], r10
    rcl r12d, 1
    shl r11d, 1
    inc r10d
    jmp check_precision

bit_done:
    ; Teach models
    mov ecx, models_end - models
teach_model:
    pop rsi
    add word [rsi], 0x0101
    shr byte [rsi + rdx], 1
    loop teach_model

    shr dl, 1
    rcl bl, 1
    jnc decode_bit

    ; Byte done
    mov [r9 + output_stream], bl
    inc r9d
    cmp r9w, output_len
    jne decode_byte

ready:
    jmp output_stream

models:
    incbin "models.bin"
models_end:

input_stream:
    incbin "compressed.bin"

    section .bss
input_stream_end:
    resb 4 ; leave a few trailing zero bits in the stream

hash_table:
    resb 2 << 24

    resb output_stream - ($ - $$ + start)

; output_stream:
    resb output_len
