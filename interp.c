#include <fcntl.h>
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
unsigned long *stack_head = stack + sizeof(stack) / sizeof(stack[0]);
unsigned long locals_stack[1024];
unsigned long *locals = locals_stack + sizeof(locals_stack) / sizeof(locals_stack[0]);

enum caller_info_variant {
    BLOCK_OR_IF,
    LOOP,
    FUNC,
};
union caller_info {
    enum caller_info_variant variant;
    struct {
        enum caller_info_variant variant;
        _Bool executed;
    } block_or_if;
    struct {
        enum caller_info_variant variant;
        _Bool executed;
        unsigned char *saved_p;
    } loop;
    struct {
        enum caller_info_variant variant;
        unsigned long *saved_locals;
        unsigned char *saved_p;
    } func;
};
union caller_info caller_stack[1024];
union caller_info *caller_stack_head = caller_stack;
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
    {
        p++; // blocktype
        // printf("push block\n");
        *caller_stack_head++ = (union caller_info) {
            .block_or_if = {
                .variant = BLOCK_OR_IF,
                .executed = break_level == 0,
            },
        };
        break;
    }
    case 0x03: // loop
    {
        p++; // blocktype
        // printf("push loop\n");
        *caller_stack_head++ = (union caller_info) {
            .loop = {
                .variant = LOOP,
                .executed = break_level == 0,
                .saved_p = p - 2,
            },
        };
        break;
    }
    case 0x04: // if
    {
        p++; // blocktype
        // printf("push if\n");
        *caller_stack_head++ = (union caller_info) {
            .block_or_if = {
                .variant = BLOCK_OR_IF,
                .executed = break_level == 0,
            },
        };
        break_level += break_level == 0 && *stack_head++;
        break;
    }
    case 0x0b: // end
    {
        union caller_info *caller = --caller_stack_head;
        switch (caller->variant) {
        case BLOCK_OR_IF:
            // printf("pop block/if\n");
            break_level -= caller->block_or_if.executed && break_level > 0;
            break;
        case LOOP:
            // printf("pop loop\n");
            if (caller->loop.executed && break_level > 0 && --break_level == 0) {
                p = caller->loop.saved_p;
            }
            break;
        case FUNC:
            // printf("pop func at bl=%d\n", break_level);
            break_level = 0;
            locals = caller->func.saved_locals;
            p = caller->func.saved_p;
            // printf("Exit\n", p);
            break;
        }
        break;
    }
    case 0x0c: // br
    {
        unsigned labelidx = read_uint();
        PARSED;
        // printf("break for %u\n", labelidx + 1);
        break_level = labelidx + 1;
        break;
    }
    case 0x0d: // br_if
    {
        unsigned labelidx = read_uint();
        PARSED;
        // printf("br_if with condition %lu\n", *stack_head);
        if (*stack_head++) {
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
    {
        unsigned localidx = read_uint();
        PARSED;
        locals[localidx] = *stack_head++;
        break;
    }
    case 0x22: // local.tee
    {
        unsigned localidx = read_uint();
        PARSED;
        locals[localidx] = *stack_head;
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

        static unsigned char shifts[] = { 32, 0, 56, 56, 48, 48, 56, 56, 48, 48, 32, 32 };
        unsigned char shift = shifts[opcode - 0x28];

        value <<= shift;
        if (opcode % 2 == 0) { // signed
            value = (long)value >> shift;
            if (opcode < 0x30 && opcode != 0x29) { // 32-bit destination
                value &= -1U;
            }
        } else {
            value >>= shift;
        }

        *stack_head = value;
        break;
    }
    case 0x36: // i32.store
    case 0x37: // i64.store
    case 0x3a: // i32.store8
    case 0x3b: // i32.store16
    case 0x3c: // i64.store8
    case 0x3d: // i64.store16
    case 0x3e: // i64.store32
    {
        read_uint(); // align
        unsigned offset = read_uint();
        PARSED;
        unsigned long value = *stack_head++;
        unsigned address = offset + *stack_head++;
        unsigned len = (
            opcode == 0x36 || opcode == 0x3e ? 4 :
            opcode == 0x37 ? 8 :
            opcode == 0x3a || opcode == 0x3c ? 1 :
            opcode == 0x3b || opcode == 0x3d ? 2 :
            -1U
        );
        memcpy(memory + address, &value, len);
        break;
    }
    case 0x41: // i32.const
    case 0x42: // i64.const
    {
        long c = read_sint();
        PARSED;
        *--stack_head = opcode == 0x41 ? (unsigned)c : c;
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
    {
        PARSED;
        unsigned long x = *stack_head;
        x = (
            opcode == 0x45 || opcode == 0x50 ? x == 0 :
            opcode == 0x67 ? __builtin_clzg(x, 32) :
            opcode == 0x68 ? __builtin_ctzg(x, 32) :
            opcode == 0x69 || opcode == 0x7b ? __builtin_popcount(x) :
            opcode == 0x79 ? __builtin_clzg(x, 64) :
            opcode == 0x7a ? __builtin_ctzg(x, 64) :
            -1UL
        );
        *stack_head = x;
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

        *handler = opcode == 0x7c ? 0x48 /* REX.W */ : 0x40 /* REX */;

        unsigned long zero = 0;

        asm volatile (
            "call *%[handler];"
            ".pushsection .text.op;"
            "binop_handlers:"
#define BINOP(code) code "; ret;"
            BINOP("add %[b], %[a]")
            BINOP("sub %[b], %[a]")
            BINOP("imul %[b], %[a]")
            BINOP("idiv %[b]")
            BINOP("div %[b]")
            BINOP("idiv %[b]; mov %%rdx, %%rax")
            BINOP("div %[b]; mov %%rdx, %%rax")
            BINOP("and %[b], %[a]")
            BINOP("or %[b], %[a]")
            BINOP("xor %[b], %[a]")
            BINOP("shl %b[b], %[a]")
            BINOP("sar %b[b], %[a]")
            BINOP("shr %b[b], %[a]")
            BINOP("rol %b[b], %[a]")
            BINOP("ror %b[b], %[a]")
            ".popsection"
            : [a]"+a"(a), "+d"(zero) // specific register and zero for `div`
            : [b]"c"(b), [handler]"r"(handler) // specific register for shifts
            : "flags"
        );

        *stack_head = a;
        break;
    }
    case 0x99: // f64.abs
        PARSED;
        *stack_head &= (-1ULL) >> 1;
        break;
    case 0xad: // i64.extend_i32_u
        PARSED;
        *stack_head = (unsigned)*stack_head;
        break;
    case 0xc0: // i32.extend8_s
        PARSED;
        *stack_head = (unsigned long)(int)(signed char)*stack_head;
        break;
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
    *caller_stack_head++ = (union caller_info) {
        .func = {
            .variant = FUNC,
            .saved_locals = saved_locals,
            .saved_p = saved_p,
        },
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
