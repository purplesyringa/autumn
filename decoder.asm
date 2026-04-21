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
    ; rbx -- value
    ; rdi -- current output byte
    ; rbp -- range
    ; r10 -- current input bit index
    ;
    ; While decoding bits:
    ; rsi -- total c0
    ; r14 -- total c1
    ;
    ; While querying models:
    ; rax -- 8-bit mask / hash / counters
    ; rcx -- model count
    ; rdx -- 64-bit mask / masked prev_bytes / c0
    ;
    ; Teaching models:
    ; rcx -- model counter
    ; rdx -- calculated bit

entry:
    mov edi, output_stream - 1
    dec ebp
    mov ebx, initial

next_byte:
    inc edi
    cmp di, output_stream_end & 0xffff
    je output_stream
    ; If we're here, cmp set CF = 1, which is then inserted into the new byte by the `rcl` below.

write_bit:
    rcl byte [rdi], 1
    jc next_byte

    push 1
    pop rsi ; c0
    mov r14d, esi ; c1

    mov cl, models_end - models

query_model:
    ; Load 8-bit mask
    mov dl, byte [rcx + models - 1]

    ; Compute hash
    xor eax, eax
    crc32 eax, dl
    sub edi, 8
hash:
    inc edi
    shl dl, 1
    jnc skip_byte
    crc32 eax, byte [rdi]
skip_byte:
    jne hash

    ; Compute hash table entry address
    and eax, 32 * 1024 * 1024 - 2
    add eax, hash_table

    ; Save address for later adjustment
    push rax

    ; Accumulate c0/c1
    movzx edx, byte [rax + 1]
    movzx eax, byte [rax]
    add esi, eax
    add r14d, edx

    loop query_model

    ; We now know c0 and c1 
    add esi, r14d ; c0 += c1

    ; mid = range * c1 / (c0 + c1)
    mov eax, ebp
    mul r14d
    div esi

    cmp ebx, eax
    sbb rdx, rdx ; bit = 0 => rdx = 0, bit = 1 => rdx = -1
    je bit0

    ; bit = 1, x < mid
    mov ebp, eax
    xor eax, eax

bit0:
    ; bit = 0, x >= mid
    sub ebx, eax
    sub ebp, eax
    js bit_done

increase_precision:
    bt [rel input_stream], r10
    rcl ebx, 1
    inc r10d
    shl ebp, 1
    jns increase_precision

bit_done:
    ; Teach models
    mov cl, models_end - models
teach_model:
    pop rsi
    inc byte [rsi]
    inc byte [rsi + 1]
    shr byte [rsi + rdx + 1], 1
    loop teach_model

    shr dl, 1
    jmp write_bit

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

    ; Align to a known address, as well as add a few preceding zeros so that `prev_bytes` is
    ; computed correctly.
    resb output_stream - ($ - $$ + start)

; output_stream:
    resb output_len
    output_stream_end equ output_stream + output_len
