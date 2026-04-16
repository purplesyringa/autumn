#include <fcntl.h>
#include <immintrin.h>
#include <stdint.h>
// #include <stdio.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

unsigned char module_bytes[1024 * 1024];
register unsigned char *p asm ("r12");

static void copy_forward(void *dst, const void *src, size_t n) {
    asm volatile ("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
}

static void *memcpy(void *dst, const void *src, size_t n) {
    copy_forward(dst, src, n);
    return dst;
}

static void *memmove(void *dst, const void *src, size_t n) {
    if ((uintptr_t)src < (uintptr_t)dst) {
        copy_forward(dst, src, n);
    } else {
        dst = (char *)dst + n - 1;
        src = (char *)src + n - 1;
        asm volatile ("std; rep movsb; cld" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
    }
    return dst;
}

static long syscall2(long sysno, long a, long b) {
    asm volatile ("syscall" : "+a"(sysno) : "D"(a), "S"(b) : "rcx", "r11", "memory");
    return sysno;
}
static long syscall3(long sysno, long a, long b, long c) {
    asm volatile ("syscall" : "+a"(sysno) : "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return sysno;
}

__attribute__((noinline))
static unsigned long impl_read_int(_Bool is_signed) {
    int shift = 64;
    unsigned long out = 0;
    do {
        out = (out >> 7) | ((unsigned long)(*p & 0x7f) << (64 - 7));
        shift -= 7;
    } while (*p++ & 0x80);
    if (is_signed) {
        out = (long)out >> shift;
    } else {
        out >>= shift;
    }
    return out;
}

static unsigned long read_uint() {
    return impl_read_int(0);
}

static long read_sint() {
    return impl_read_int(1);
}

struct func_info {
    unsigned char *func;
    unsigned typeidx;
};

unsigned char *declared_types[1024];
unsigned long globals[1024];
unsigned main_funcidx;
unsigned start_funcidx = -1;
unsigned long func_table[1024];
struct func_info funcs[1024];
unsigned n_funcs;
unsigned char memory[2 * 1024 * 1024];
unsigned long stack[1024];
register unsigned long *stack_head asm ("r13");
unsigned long locals_stack[1024];
unsigned long *locals = locals_stack + sizeof(locals_stack) / sizeof(locals_stack[0]);

enum caller_info_variant {
    BLOCK_OR_IF,
    LOOP,
    FUNC,
};
struct caller_info {
    enum caller_info_variant variant;
    unsigned char *saved_p;
    union {
        _Bool executed;
        unsigned long *saved_locals;
    };
};
struct caller_info caller_stack[1024];
struct caller_info *caller_stack_head = caller_stack;
unsigned break_level;

static void call_func(unsigned funcidx);

static void eval_instr() {
#define PARSED if (break_level) break

    unsigned char opcode = *p++;
    // if (break_level == 0) {
    //     printf("opcode 0x%02x\n", opcode);
    // } else {
    //     printf("skip 0x%02x\n", opcode);
    // }
    switch (opcode) {
    case 0x00: // unreachable
        PARSED;
        __builtin_trap();
    case 0x01: // nop
        break;
    case 0x02: // block
    case 0x03: // loop
    case 0x04: // if
    {
        p++; // blocktype
        *caller_stack_head++ = (struct caller_info) {
            .variant = opcode == 0x03 ? LOOP : BLOCK_OR_IF,
            .saved_p = p - 2,
            .executed = break_level == 0,
        };
        break_level += opcode == 0x04 && break_level == 0 && *stack_head++;
        break;
    }
    case 0x0b: // end
    {
        struct caller_info *caller = --caller_stack_head;
        if (caller->variant == FUNC) {
            break_level = 0;
            locals = caller->saved_locals;
            p = caller->saved_p;
        } else {
            if (caller->executed && break_level > 0) {
                break_level--;
                if (caller->variant == LOOP && break_level == 0) {
                    p = caller->saved_p;
                }
            }
        }
        break;
    }
    case 0x0c: // br
    case 0x0d: // br_if
    {
        unsigned labelidx = read_uint();
        PARSED;
        if (opcode == 0x0c || *stack_head++) {
            break_level = labelidx + 1;
        }
        break;
    }
    case 0x0e: // br_table
    {
        unsigned n_labels = read_uint();
        unsigned jump_table[n_labels];
        for (unsigned i = 0; i < n_labels; i++) {
            jump_table[i] = read_uint();
        }
        unsigned otherwise = read_uint();
        PARSED;
        unsigned i = *stack_head++;
        break_level = i < n_labels ? jump_table[i] : otherwise;
        break;
    }
    case 0x0f: // return
        PARSED;
        break_level = -1U;
        break;
    case 0x10: // call
    {
        unsigned funcidx = read_uint();
        PARSED;
        call_func(funcidx);
        break;
    }
    case 0x11: // call_indirect
    {
        read_uint(); // typeidx
        read_uint(); // tableidx. always 0, but may be overlong
        PARSED;
        unsigned tableidx = *stack_head++;
        call_func(func_table[tableidx]);
        break;
    }
    case 0x1a: // drop
        PARSED;
        stack_head++;
        break;
    case 0x1b: // select
    {
        PARSED;
        unsigned long cond = *stack_head++;
        unsigned long b = *stack_head++;
        // printf("select with condition %lu\n", cond);
        *stack_head = cond ? *stack_head : b;
        break;
    }
    case 0x20: // local.get
    {
        unsigned localidx = read_uint();
        PARSED;
        *--stack_head = locals[localidx];
        break;
    }
    case 0x21: // local.set
    case 0x22: // local.tee
    {
        unsigned localidx = read_uint();
        PARSED;
        locals[localidx] = *stack_head;
        stack_head += opcode == 0x21;
        break;
    }
    case 0x23: // global.get
    {
        unsigned globalidx = read_uint();
        PARSED;
        *--stack_head = globals[globalidx];
        break;
    }
    case 0x24: // global.set
    {
        unsigned globalidx = read_uint();
        PARSED;
        globals[globalidx] = *stack_head++;
        break;
    }
    case 0x28: // i32.load
    case 0x29: // i64.load
    case 0x2a: // f32.load
    case 0x2b: // f64.load
    case 0x2c: // i32.load8_s
    case 0x2d: // i32.load8_u
    case 0x2e: // i32.load16_s
    case 0x2f: // i32.load16_u
    case 0x30: // i64.load8_s
    case 0x31: // i64.load8_u
    case 0x32: // i64.load16_s
    case 0x33: // i64.load16_u
    case 0x34: // i64.load32_s
    case 0x35: // i64.load32_u
    {
        read_uint(); // align
        unsigned offset = read_uint();
        PARSED;
        unsigned address = offset + *stack_head;

        unsigned long value;
        __builtin_memcpy(&value, memory + address, 8);

        unsigned char shift = (0x3038000000203038U >> ((opcode * 4) & 0x38)) & 0xff;

        value <<= shift;
        if (opcode % 2 == 0) { // signed or pure 32-bit
            value = (long)value >> shift;
            if (opcode < 0x30) { // 32-bit destination
                value &= -1U;
            }
        } else { // unsigned or pure 64-bit
            value >>= shift;
        }

        *stack_head = value;
        break;
    }
    case 0x36: // i32.store
    case 0x37: // i64.store
    case 0x38: // f32.store
    case 0x39: // f64.store
    case 0x3a: // i32.store8
    case 0x3b: // i32.store16
    case 0x3c: // i64.store8
    case 0x3d: // i64.store16
    case 0x3e: // i64.store32
    {
        read_uint(); // align
        unsigned offset = read_uint();
        PARSED;
        unsigned long *value = stack_head++;
        unsigned address = offset + *stack_head++;
        unsigned long len_const = 0x0804020102010804UL;
        asm ("ror %b1, %0" : "+r"(len_const) : "c"(opcode * 8) : "flags");
        memcpy(memory + address, value, (unsigned char)len_const);
        break;
    }
    case 0x3f: // memory.size
        PARSED;
        *--stack_head = sizeof(memory) - 7;
        break;
    case 0x40: // memory.grow
        PARSED;
        *stack_head = -1U;
        break;
    case 0x41: // i32.const
    case 0x42: // i64.const
    case 0x43: // f32.const
    case 0x44: // f64.const
    {
        long c = read_sint();
        PARSED;
        *--stack_head = opcode & 1 ? (unsigned)c : c;
        break;
    }
    case 0x45: // i32.eqz
    case 0x50: // i64.eqz
    case 0x67: // i32.clz
    case 0x68: // i32.ctz
    case 0x69: // i32.popcnt
    case 0x79: // i64.clz
    case 0x7a: // i64.ctz
    case 0x7b: // i64.popcnt
    case 0x8b: // f32.abs
    case 0x8c: // f32.neg
    case 0x99: // f64.abs
    case 0x9a: // f64.neg
    case 0xa7: // i32.wrap_i64
    case 0xac: // i64.extend_i32_s
    case 0xad: // i64.extend_i32_u
    case 0xc0: // i32.extend8_s
    case 0xc1: // i32.extend16_s
    case 0xc2: // i64.extend8_s
    case 0xc3: // i64.extend16_s
    case 0xc4: // i64.extend32_s
    {
        PARSED;

        extern unsigned char unop_handlers;

        unsigned char *handler = &unop_handlers;
        while (*handler++ != opcode) {
            while (*handler++ != 0xc3); // ret
        }

        asm volatile (
            "call *%[handler];"
            ".pushsection .text.op;"
            "unop_handlers:"
            ".byte 0x45; jmp 1f; ret;"
            ".byte 0x50; 1: test %0, %0; mov $0, %k0; sete %b0; ret;"
            ".byte 0x67; lzcnt %k0, %k0; ret;"
            ".byte 0x68; tzcnt %k0, %k0; ret;"
            ".byte 0x69; jmp 1f; ret;"
            ".byte 0x7b; 1: popcnt %0, %0; ret;"
            ".byte 0x79; lzcnt %0, %0; ret;"
            ".byte 0x7a; tzcnt %0, %0; ret;"
            ".byte 0x8b; btr $31, %k0; ret;"
            ".byte 0x8c; btc $31, %k0; ret;"
            ".byte 0x99; btr $63, %0; ret;"
            ".byte 0x9a; btc $63, %0; ret;"
            ".byte 0xa7; jmp 1f; ret;"
            ".byte 0xad; 1: mov %k0, %k0; ret;"
            ".byte 0xac; jmp 1f; ret;"
            ".byte 0xc4; 1: movsx %k0, %0; ret;"
            ".byte 0xc0; 1: movsx %b0, %k0; ret;"
            ".byte 0xc1; 1: movsx %w0, %k0; ret;"
            ".byte 0xc2; 1: movsx %b0, %0; ret;"
            ".byte 0xc3; 1: movsx %w0, %0; ret;"
            ".popsection"
            : "+a"(*stack_head) // specific register to make sure 0xc3 doesn't appear by accident
            : [handler]"r"(handler)
            : "flags"
        );
        break;
    }
    case 0x46: // i32.eq
    case 0x47: // i32.ne
    case 0x48: // i32.lt_s
    case 0x49: // i32.lt_u
    case 0x4a: // i32.gt_s
    case 0x4b: // i32.gt_u
    case 0x4c: // i32.le_s
    case 0x4d: // i32.le_u
    case 0x4e: // i32.ge_s
    case 0x4f: // i32.ge_u
    case 0x51: // i64.eq
    case 0x52: // i64.ne
    case 0x53: // i64.lt_s
    case 0x54: // i64.lt_u
    case 0x55: // i64.gt_s
    case 0x56: // i64.gt_u
    case 0x57: // i64.le_s
    case 0x58: // i64.le_u
    case 0x59: // i64.ge_s
    case 0x5a: // i64.ge_u
    {
        PARSED;
        unsigned long b = *stack_head++;
        unsigned long a = *stack_head;
        if (opcode < 0x51) { // i32
            if (opcode % 2 == 0) { // signed or i32.eq
                // sign-extend
                a = (long)(int)a;
                b = (long)(int)b;
            }
            opcode += 0x51 - 0x46;
        }
        static unsigned char opcode_bytes[] = {
            0x94, // sete
            0x95, // setne
            0x9c, // setl
            0x92, // setb
            0x9f, // setg
            0x97, // seta
            0x9e, // setle
            0x96, // setbe
            0x9d, // setge
            0x93, // setae
        };
        _Bool cond;
        asm (
            "cmp %2, %1;"
            "mov %3, 1f + 1(%%rip);"
            "1:"
            "setb %0"
            : "=r"(cond)
            : "r"(a), "r"(b), "r"(opcode_bytes[opcode - 0x51])
            : "flags"
        );
        *stack_head = cond;
        break;
    }
    case 0x5b: // f32.eq
    case 0x5c: // f32.ne
    case 0x5d: // f32.lt
    case 0x5e: // f32.gt
    case 0x5f: // f32.le
    case 0x60: // f32.ge
    case 0x61: // f64.eq
    case 0x62: // f64.ne
    case 0x63: // f64.lt
    case 0x64: // f64.gt
    case 0x65: // f64.le
    case 0x66: // f64.ge
    {
        PARSED;
        unsigned long *b = stack_head++;
        __m128i a = _mm_loadu_si64(stack_head);

        unsigned char size_byte = 0xc2;
        if (opcode < 0x61) {
            size_byte++;
            opcode += 0x61 - 0x5b;
        }

        unsigned imm8_const = 0xd2e140U;
        asm ("ror %b1, %0" : "+r"(imm8_const) : "c"(opcode * 4) : "flags");
        unsigned imm8 = imm8_const & 0xf;

        asm (
            "mov %2, 1f + 2(%%rip);"
            "mov %3, 1f + 4(%%rip);"
            "1:"
            "vcmpss $0, %1, %0, %0"
            : "+x"(a)
            : "m"(*b), "r"(size_byte), "r"(imm8)
        );
        *stack_head = _mm_cvtsi128_si32(a) & 1;
        break;
    }
    case 0x6a: // i32.add
    case 0x6b: // i32.sub
    case 0x6c: // i32.mul
    case 0x6d: // i32.div_s
    case 0x6e: // i32.div_u
    case 0x6f: // i32.rem_s
    case 0x70: // i32.rem_u
    case 0x71: // i32.and
    case 0x72: // i32.or
    case 0x73: // i32.xor
    case 0x74: // i32.shl
    case 0x75: // i32.shr_s
    case 0x76: // i32.shr_u
    case 0x77: // i32.rotl
    case 0x78: // i32.rotr
    case 0x7c: // i64.add
    case 0x7d: // i64.sub
    case 0x7e: // i64.mul
    case 0x7f: // i64.div_s
    case 0x80: // i64.div_u
    case 0x81: // i64.rem_s
    case 0x82: // i64.rem_u
    case 0x83: // i64.and
    case 0x84: // i64.or
    case 0x85: // i64.xor
    case 0x86: // i64.shl
    case 0x87: // i64.shr_s
    case 0x88: // i64.shr_u
    case 0x89: // i64.rotl
    case 0x8a: // i64.rotr
    {
        PARSED;

        extern unsigned char binop_handlers;

        unsigned long b = *stack_head++;
        unsigned long a = *stack_head;

        unsigned char *handler = &binop_handlers;
        while (opcode != 0x6a && opcode != 0x7c) {
            opcode -= *handler++ == 0xc3; // ret
        }
        handler += opcode == 0x6a; // skip REX.W if 32-bit

        unsigned long zero = 0;

        asm volatile (
            "call *%[handler];"
            ".pushsection .text.op;"
            "binop_handlers:"
            "add %[b], %[a]; ret;"
            "sub %[b], %[a]; ret;"
            "imul %[b], %[a]; ret;"
            "idiv %[b]; ret;"
            "div %[b]; ret;"
            "idiv %[b]; mov %%rdx, %%rax; ret;"
            "div %[b]; mov %%rdx, %%rax; ret;"
            "and %[b], %[a]; ret;"
            "or %[b], %[a]; ret;"
            "xor %[b], %[a]; ret;"
            "shl %b[b], %[a]; ret;"
            "sar %b[b], %[a]; ret;"
            "shr %b[b], %[a]; ret;"
            "rol %b[b], %[a]; ret;"
            "ror %b[b], %[a]; ret;"
            ".popsection"
            : [a]"+a"(a), "+d"(zero) // specific register and zero for `div`
            : [b]"c"(b), [handler]"r"(handler) // specific register for shifts
            : "flags"
        );

        *stack_head = a;
        break;
    }
    case 0x8d: // f32.ceil
    case 0x8e: // f32.floor
    case 0x8f: // f32.trunc
    case 0x90: // f32.nearest
    case 0x91: // f32.sqrt
    case 0x9b: // f64.ceil
    case 0x9c: // f64.floor
    case 0x9d: // f64.trunc
    case 0x9e: // f64.nearest
    case 0x9f: // f64.sqrt
    {
        PARSED;

        _Bool is_f64 = opcode >= 0x9b;
        if (is_f64) {
            opcode -= 0x9b - 0x8d;
        }

        char *handler;
        if (opcode != 0x91) {
            // rounding
            extern char op_round[];
            handler = op_round;
            handler[5] = 0x0a + is_f64;
            handler[7] = (0b00110110 >> ((opcode - 0x8d) * 2)) & 3;
        } else {
            extern char op_sqrt[];
            handler = op_sqrt;
            *handler = 0xf3 - is_f64;
        }

        __m128i a;
        asm (
            "xorps %0, %0;"
            "call *%[handler];"
            "movq %0, %1;"
            ".pushsection .text.op;"
            "op_round: roundsd $0, %1, %0; ret;"
            "op_sqrt: sqrtsd %1, %0; ret;"
            ".popsection"
            : "=x"(a), "+m"(*stack_head)
            : [handler]"r"(handler)
        );
        break;
    }
    case 0x92: // f32.add
    case 0x93: // f32.sub
    case 0x94: // f32.mul
    case 0x95: // f32.div
    case 0xa0: // f64.add
    case 0xa1: // f64.sub
    case 0xa2: // f64.mul
    case 0xa3: // f64.div
    {
        PARSED;

        unsigned char size_byte = 0xf2;
        if (opcode >= 0xa0) { // f64
            size_byte++;
            opcode -= 0xa0 - 0x92;
        }
        unsigned char op_byte = 0x5e595c58U >> ((opcode - 0x92) * 8);

        unsigned long *b = stack_head++;
        unsigned long *a = stack_head;
        asm (
            "mov %2, 1f(%%rip);"
            "mov %3, 1f + 2(%%rip);"
            "1:"
            "addsd %1, %0;"
            : "+x"(*a)
            : "m"(*b), "r"(size_byte), "r"(op_byte)
        );
        break;
    }
    case 0x98: // f32.copysign
    case 0xa6: // f64.copysign
    {
        PARSED;
        unsigned long b = *stack_head++;
        asm (
            "shl %[c], %[a];"
            "shl %[c], %[b];"
            "rcr %[c], %[a];"
            : [a]"+r"(*stack_head), [b]"+r"(b)
            : [c]"c"((unsigned char)(opcode == 0x98 ? 33 : 1))
            : "flags"
        );
        break;
    }
    case 0xfc:
        opcode = *p++;
        switch (opcode) {
        case 0x0a: // memory.copy
        {
            p += 2; // memidx x2
            PARSED;
            unsigned n = *stack_head++;
            unsigned src = *stack_head++;
            unsigned dst = *stack_head++;
            memmove(memory + dst, memory + src, n);
            break;
        }
        default:
            // printf("Unknown opcode 0xfc 0x%02x\n", opcode);
            __builtin_trap();
        }
        break;
    default:
        // printf("Unknown opcode 0x%02x\n", opcode);
        __builtin_trap();
    }
}

static void call_func(unsigned funcidx) {
    struct func_info *info = &funcs[funcidx];
    if (info->typeidx == -1U) {
        // native code
        ((void (*)())info->func)();
        return;
    }

    // printf("Enter %u at %p\n", funcidx, p);
    unsigned char *saved_p = p;
    p = info->func;

    unsigned n_local_groups = read_uint();
    unsigned n_locals = 0;
    while (n_local_groups--) {
        n_locals += read_uint(); // n
        p++; // valtype
    }

    unsigned char *body_p = p;
    p = declared_types[info->typeidx];
    unsigned n_args = read_uint();

    unsigned long *saved_locals = locals;
    locals -= n_args + n_locals;
    for (unsigned i = 0; i < n_args; i++) {
        locals[i] = stack_head[n_args - 1 - i];
    }
    stack_head += n_args;
    p = body_p;

    // printf("push func\n");
    *caller_stack_head++ = (struct caller_info) {
        .variant = FUNC,
        .saved_p = saved_p,
        .saved_locals = saved_locals,
    };
}

static void fd_write() {
    unsigned n_written = *stack_head++;
    unsigned iovs_len = *stack_head++;
    unsigned iovs = *stack_head++;
    unsigned fd = *stack_head;

    struct wasi_iovec {
        unsigned buf;
        unsigned buf_len;
    };
    struct wasi_iovec *wasi_iovs = (void*)(memory + iovs);
    struct iovec native_iovs[iovs_len];
    for (unsigned i = 0; i < iovs_len; i++) {
        native_iovs[i] = (struct iovec){
            .iov_base = memory + wasi_iovs[i].buf,
            .iov_len = wasi_iovs[i].buf_len,
        };
    }
    ssize_t native_out = syscall3(SYS_writev, fd, (long)native_iovs, iovs_len);

    unsigned long wasi_out;
    if (native_out < 0) {
        wasi_out = -native_out;
    } else {
        __builtin_memcpy(memory + n_written, &native_out, 4);
        wasi_out = 0;
    }

    *stack_head = wasi_out;
}

int main(int argc, char **argv) {
    (void)argc;

    int fd = syscall2(SYS_open, (long)argv[1], O_RDONLY);
    int len = syscall3(SYS_read, fd, (long)module_bytes, sizeof(module_bytes));

    p = module_bytes + 8;
    while (p != module_bytes + len) {
        unsigned char section_type = *p++;
        unsigned byte_len = read_uint();
        // printf("section of type %d of length %u\n", section_type, byte_len);

        if (section_type == 1) {
            // Type section
            unsigned n_functypes = read_uint();
            // printf("%u types\n", n_functypes);
            for (unsigned i = 0; i < n_functypes; i++) {
                p++; // 0x60
                declared_types[i] = p;
                for (int j = 0; j < 2; j++) {
                    unsigned n_valtypes = read_uint();
                    p += n_valtypes; // valtype is single-byte
                }
            }
        } else if (section_type == 2) {
            // Import section
            unsigned n_imports = read_uint();
            // printf("%u imports\n", n_imports);

            while (n_imports--) {
                unsigned mod_len = read_uint();
                p += mod_len;

                unsigned name_len = read_uint();
                // printf("import %.*s\n", name_len, p);

                unsigned long name;
                __builtin_memcpy(&name, p, 8);

                void (*func)() = NULL;
                if (name_len == 8 && name == 0x65746972775f6466 /* fd_write */) {
                    func = fd_write;
                }
                funcs[n_funcs++] = (struct func_info){
                    .func = (unsigned char*)func,
                    .typeidx = -1U,
                };

                p += name_len;
                p++; // 0x00
                read_uint();
            }
        } else if (section_type == 3) {
            // Function section
            unsigned n_sigs = read_uint();
            // printf("%u function signatures\n", n_sigs);
            for (unsigned i = 0; i < n_sigs; i++) {
                funcs[n_funcs + i].typeidx = read_uint();
            }
        } else if (section_type == 6) {
            // Global section
            unsigned n_globals = read_uint();
            // printf("%u globals\n", n_globals);

            for (unsigned i = 0; i < n_globals; i++) {
                unsigned char valtype = *p++;
                p++; // mut
                p++; // t.const
                switch (valtype) {
                case 0x7f:
                case 0x7e:
                    // i32/i64
                    globals[i] = read_uint();
                    break;
                case 0x7d:
                    // f32
                    __builtin_memcpy(&globals[i], p, 4);
                    p += 4;
                    break;
                case 0x7c:
                    // f64
                    __builtin_memcpy(&globals[i], p, 8);
                    p += 8;
                    break;
                }
                p++; // end
            }
        } else if (section_type == 7) {
            // Export section
            unsigned n_exports = read_uint();
            // printf("%u exports\n", n_exports);

            for (unsigned i = 0; i < n_exports; i++) {
                unsigned name_len = read_uint();
                // printf("export %.*s\n", name_len, p);
                unsigned long name;
                __builtin_memcpy(&name, p, 8);
                _Bool is_start = name_len == 6 && (name & 0xffffffffffff) == 0x74726174735f /* _start */;
                p += name_len;
                p++; // exportdesc variant
                unsigned index = read_uint(); // exportdesc index
                if (is_start) {
                    main_funcidx = index;
                }
            }
        } else if (section_type == 8) {
            // Start section
            start_funcidx = read_uint();
        } else if (section_type == 9) {
            // Element section
            unsigned n_elems = read_uint();

            while (n_elems--) {
                p++; // 0x00 tableidx
                p++; // i32.const
                unsigned offset = read_uint();
                p++; // end
                unsigned n_funcidxs = read_uint();
                // printf("%u..%u elems\n", offset, offset + n_funcidxs);
                while (n_funcidxs--) {
                    func_table[offset++] = read_uint();
                }
            }
        } else if (section_type == 10) {
            // Code section
            unsigned n_codes = read_uint();
            // printf("%u codes\n", n_codes);

            while (n_codes--) {
                unsigned int size = read_uint();
                funcs[n_funcs++].func = p;
                p += size;
            }
        } else if (section_type == 11) {
            // Data section
            unsigned n_datas = read_uint();
            // printf("%u datas\n", n_datas);

            for (unsigned i = 0; i < n_datas; i++) {
                p++; // 0x00 memidx
                p++; // i32.const
                unsigned offset = read_uint();
                p++; // end
                unsigned len = read_uint();
                // printf("%u..%u bytes\n", offset, offset + len);
                memcpy(memory + offset, p, len);
                p += len;
            }
        } else {
            p += byte_len;
        }
    }

    p = NULL;
    stack_head = stack + sizeof(stack) / sizeof(stack[0]);
    call_func(main_funcidx);
    if (start_funcidx != (unsigned)-1) {
        call_func(start_funcidx);
    }
    while (p) {
        eval_instr();
    }
}

asm (
    ".globl _start;"
    "_start:"
    "pop %rdi;" // argc
    "mov %rsp, %rsi;" // argv
    // we don't need envp
    "call main;"
    "mov $60, %eax;" // exit
    "xor %edi, %edi;"
    "syscall"
);
