    start equ 0x400000
    output_stream equ 0x2401000

    bits 64
    org start

    db 0x7f, "ELF"
    times 12 db 0 ; bitness, endianness, version, ABI, ABI version, padding -- all ignored
    dw 2 ; ET_EXEC
    dw 0x3e ; x86-64
    times 4 db 0 ; version, ignored
    dq entry ; entry point
    dq program_header - $$ ; program header
    times 12 db 0 ; section header and flags, ignored
    dw elf_header_end - $$ ; header size
    dw 56 ; program header entry size
program_header:
    dw 1, 0 ; EH: number of program header entries, section header entry size, PH: PT_LOAD
    dw 7, 0 ; EH: number of section header entries, .shstrtab section index, PH: rwx
elf_header_end:
    dq 0 ; file offset
    dq $$ ; virtual address
    times 8 db 0 ; physical address, ignored
    dq input_stream_end - $$ ; file size
    dq 0x103000000 ; memory size
    ; 8 bytes of alignment, ignored

    ; Register allocation:
    ;
    ; Shared among all stages:
    ; rbx -- value
    ; rdi -- current output byte
    ; rbp -- range
    ; r10 -- current input bit index
    ;
    ; While decoding bits:
    ; rdx -- total c1
    ; rsi -- total c0
    ;
    ; While querying models:
    ; rax -- hash / counters
    ; rcx -- model count / 8-bit mask / c1
    ;
    ; Teaching models:
    ; rcx -- model counter
    ; rdx -- calculated bit
    ; rsi -- hashtable entry pointer

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
    mov edx, esi ; c1

    mov cl, models_end - models

query_model:
    push rcx

    ; Load 8-bit mask
    mov cl, byte [rcx + models - 1]

    ; Compute hash
    xor eax, eax
    crc32 eax, cl
    sub edi, 8
hash:
    inc edi
    shl cl, 1
    jnc skip_byte
    crc32 eax, byte [rdi]
skip_byte:
    jne hash

    ; Compute hash table entry address
    and eax, 32 * 1024 * 1024 - 2
    add eax, hash_table

    ; Accumulate c0/c1
    movzx ecx, byte [rax + 1]
    add edx, ecx
    movzx ecx, byte [rax]
    add esi, ecx

    pop rcx

    ; Save address for later adjustment
    push rax

    loop query_model

    ; We now know c0 and c1 
    add esi, edx ; c0 += c1

    ; mid = range * c1 / (c0 + c1)
    mov eax, ebp
    mul edx
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
