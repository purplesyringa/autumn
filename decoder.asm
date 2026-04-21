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

    ; Register allocation:
    ;
    ; Shared among all stages:
    ; rbx (prev_bytes << 8) | next_byte
    ; rdi -- current output byte
    ; rbp -- range
    ; r10 -- current input bit index
    ; r12 -- value
    ;
    ; While decoding bits:
    ; r8 -- total c0
    ; r14 -- total c1
    ;
    ; While querying models:
    ; rax -- 8-bit mask / hash / counters
    ; rcx -- model count
    ; rdx -- 64-bit mask / masked prev_bytes / c0
    ; rsi -- model list
    ;
    ; Teaching models:
    ; rcx -- model counter
    ; rdx -- calculated bit

entry:
    mov edi, output_stream
    dec ebp
    mov r12d, initial

decode_byte:
    shl rbx, 8
    inc bl

decode_bit:
    push 1
    pop r8 ; c0
    mov r14d, r8d ; c1

    mov esi, models
    mov ecx, models_end - models

query_model:
    xor eax, eax

    ; Load 8-bit mask
    lodsb

    ; Extend mask to 64-bit
    pdep rdx, rax, [rel ones]
    imul rdx, 0xff

    ; Apply mask to `prev_bytes`
    and rdx, rbx

    ; Compute hash table entry address
    crc32 rax, rdx
    xor eax, edx
    and eax, 16 * 1024 * 1024 - 1
    lea eax, [rax * 2 + hash_table]

    ; Save address for later adjustment
    push rax

    ; Accumulate c0/c1
    mov ax, [rax]
    movzx edx, ah
    movzx eax, al
    add r8d, edx
    add r14d, eax

    loop query_model

    ; We now know c0 and c1 
    add r14d, r8d ; c1 += c0

    ; mid = range * c0 / (c0 + c1)
    mov eax, ebp
    mul r8d
    div r14d

    xor edx, edx
    cmp r12d, eax
    setae dl
    jae bit1

    ; bit = 0, x < mid
    mov ebp, eax
    xor eax, eax

bit1:
    ; bit = 1, x >= mid
    sub r12d, eax
    sub ebp, eax
    js bit_done

increase_precision:
    bt [rel input_stream], r10
    rcl r12d, 1
    inc r10d
    shl ebp, 1
    jns increase_precision

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
    mov al, bl
    stosb
    cmp edi, output_stream_end
    jne decode_byte

ready:
    jmp output_stream

ones:
    dq 0x0101010101010101

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
output_stream_end:
