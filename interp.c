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

static void copy_backward(void *dst, const void *src, size_t n) {
    dst = (char *)dst + n - 1;
    src = (char *)src + n - 1;
    asm volatile ("std; rep movsb; cld" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
}

static void *memcpy(void *dst, const void *src, size_t n) {
    copy_forward(dst, src, n);
    return dst;
}

static void *memmove(void *dst, const void *src, size_t n) {
    if ((uintptr_t)src < (uintptr_t)dst) {
        copy_forward(dst, src, n);
    } else {
        copy_backward(dst, src, n);
    }
    return dst;
}

static void *memset(void *s, int c, size_t n) {
    void *orig_s = s;
    asm volatile ("rep stosb" : "+D"(s), "+c"(n) : "a"(c) : "memory");
    return orig_s;
}

static long syscall2(long sysno, long a, long b) {
    asm volatile ("syscall" : "+a"(sysno) : "D"(a), "S"(b) : "rcx", "r11", "memory");
    return sysno;
}
static long syscall3(long sysno, long a, long b, long c) {
    asm volatile ("syscall" : "+a"(sysno) : "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return sysno;
}

#define SCASB(ptr, count, value) asm ("repne scasb" : "+D"(ptr), "+c"(count) : "a"((unsigned char)value) : "flags");

struct read_int_output {
    unsigned long value;
    unsigned char shift;
};

__attribute__((noinline))
static struct read_int_output impl_read_int() {
    unsigned char shift = 64;
    unsigned long value = 0;
    unsigned char c;
    do {
        c = *p;
        p++;
        unsigned char bits = shift < 7 ? shift : 7;
        asm ("shrd %2, %q1, %0" : "+r"(value) : "r"(c), "c"(bits) : "flags");
        shift -= bits;
    } while (c & 0x80);
    return (struct read_int_output) {
        .value = value,
        .shift = shift,
    };
}

static unsigned long read_uint() {
    struct read_int_output out = impl_read_int();
    return out.value >> out.shift;
}

static long read_sint() {
    struct read_int_output out = impl_read_int();
    return (long)out.value >> out.shift;
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
struct func_info funcs[4096];
unsigned n_funcs;
unsigned char memory[2 * 1024 * 1024];
unsigned long stack[1024];
register unsigned long *stack_head asm ("r13");
unsigned long locals_stack[1024];
unsigned long *locals = locals_stack + sizeof(locals_stack) / sizeof(locals_stack[0]);

struct caller_info {
    unsigned char opcode;
    unsigned char *saved_p;
    unsigned long *saved_stack_head;
    union {
        struct {
            _Bool has_result;
            _Bool skipped_if;
        };
        unsigned long *saved_locals;
    };
};
struct caller_info caller_stack[1024];
struct caller_info *caller_stack_head = caller_stack;
register unsigned break_level asm ("r14");

static void call_func(unsigned funcidx);

#define PARSED if (break_level) return
#define DEF(name, ...) \
    static void op_##name( \
        __attribute__((unused)) unsigned char opcode, \
        __attribute__((unused)) unsigned char arg \
    )

DEF(unknown) {
    __builtin_trap();
}

DEF(unreachable, 0x00) {
    PARSED;
    __builtin_trap();
}

DEF(
    nop,
    0x01, // nop
    0xbc, // i32.reinterpret_f32
    0xbd, // i64.reinterpret_f64
    0xbe, // f32.reinterpret_i32
    0xbf, // f64.reinterpret_i64
) {}

DEF(block_like, 0x02 /* block */, 0x03 /* loop */, 0x04 /* if */) {
    unsigned char blocktype = *p++;
    _Bool skipped_if = break_level == 0 && opcode == 0x04 && !*stack_head++;
    *caller_stack_head++ = (struct caller_info) {
        .opcode = opcode,
        .saved_p = p - 2,
        .saved_stack_head = stack_head,
        .has_result = blocktype != 0x40,
        .skipped_if = skipped_if,
    };
    break_level += break_level > 0 || skipped_if;
}

DEF(else, 0x05) {
    break_level += break_level == 0;
    break_level -= caller_stack_head[-1].skipped_if;
}

DEF(end, 0x0b) {
    struct caller_info *caller = --caller_stack_head;
    if (caller->opcode == 0x10 /* call */) {
        break_level = 0;
        locals = caller->saved_locals;
        p = caller->saved_p;
    } else {
        if (break_level > 0) {
            break_level--;
            if (break_level == 0) {
                if (caller->opcode == 0x03) { // loop
                    p = caller->saved_p;
                } else { // block or if
                    if (caller->has_result) {
                        unsigned long value = *stack_head++;
                        stack_head = caller->saved_stack_head;
                        *--stack_head = value;
                    } else {
                        stack_head = caller->saved_stack_head;
                    }
                }
            }
        }
    }
}

DEF(br_like, 0x0c /* br */, 0x0d /* br_if */) {
    unsigned labelidx = read_uint();
    PARSED;
    if (opcode == 0x0c || *stack_head++) {
        break_level = labelidx + 1;
    }
}

DEF(br_table, 0x0e) {
    unsigned n_labels = read_uint();
    unsigned index = -1U;
    if (break_level == 0) {
        index = *stack_head++;
        if (index > n_labels) {
            index = n_labels;
        }
    }
    for (unsigned i = 0; i <= n_labels; i++) {
        unsigned labelidx = read_uint();
        if (i == index) {
            break_level = labelidx + 1;
        }
    }
}

DEF(return, 0x0f) {
    PARSED;
    break_level = 0x80000000U;
}

DEF(call, 0x10) {
    unsigned funcidx = read_uint();
    PARSED;
    call_func(funcidx);
}

DEF(call_indirect, 0x11) {
    read_uint(); // typeidx
    read_uint(); // tableidx. always 0, but may be overlong
    PARSED;
    unsigned tableidx = *stack_head++;
    call_func(func_table[tableidx]);
}

DEF(drop, 0x1a) {
    PARSED;
    stack_head++;
}

DEF(select, 0x1b) {
    PARSED;
    unsigned long cond = *stack_head++;
    unsigned long b = *stack_head++;
    if (!cond) {
        *stack_head = b;
    }
}

DEF(local_get, 0x20) {
    unsigned localidx = read_uint();
    PARSED;
    *--stack_head = locals[localidx];
}

DEF(local_set_like, 0x21 = 1 /* local.set */, 0x22 = 0 /* local.tee */) {
    unsigned localidx = read_uint();
    PARSED;
    locals[localidx] = *stack_head;
    stack_head += arg;
}

DEF(global_get, 0x23) {
    unsigned globalidx = read_uint();
    PARSED;
    *--stack_head = globals[globalidx];
}

DEF(global_set, 0x24) {
    unsigned globalidx = read_uint();
    PARSED;
    globals[globalidx] = *stack_head++;
}

DEF(
    load,
    0x28 = 0, // i32.load
    0x29 = 0, // i64.load
    0x2a = 0, // f32.load
    0x2b = 0, // f64.load
    0x2c = 56, // i32.load8_s
    0x2d = 56, // i32.load8_u
    0x2e = 48, // i32.load16_s
    0x2f = 48, // i32.load16_u
    0x30 = 56, // i64.load8_s
    0x31 = 56, // i64.load8_u
    0x32 = 48, // i64.load16_s
    0x33 = 48, // i64.load16_u
    0x34 = 32, // i64.load32_s
    0x35 = 32, // i64.load32_u
) {
    read_uint(); // align
    unsigned offset = read_uint();
    PARSED;
    unsigned address = offset + *stack_head;

    unsigned long value;
    __builtin_memcpy(&value, memory + address, 8);

    value <<= arg;
    if (opcode % 2 == 0) { // signed or pure 32-bit
        value = (long)value >> arg;
        if (opcode < 0x30) { // 32-bit destination
            value &= -1U;
        }
    } else { // unsigned or pure 64-bit
        value >>= arg;
    }

    *stack_head = value;
}

DEF(
    store,
    0x36 = 4, // i32.store
    0x37 = 8, // i64.store
    0x38 = 4, // f32.store
    0x39 = 8, // f64.store
    0x3a = 1, // i32.store8
    0x3b = 2, // i32.store16
    0x3c = 1, // i64.store8
    0x3d = 2, // i64.store16
    0x3e = 4, // i64.store32
) {
    read_uint(); // align
    unsigned offset = read_uint();
    PARSED;
    unsigned long *value = stack_head++;
    unsigned address = offset + *stack_head++;
    memcpy(memory + address, value, arg);
}

DEF(memory_size, 0x3f) {
    PARSED;
    *--stack_head = sizeof(memory) - 7;
}

DEF(memory_grow, 0x40) {
    PARSED;
    *stack_head = -1U;
}

DEF(int_const, 0x41 /* i32.const */, 0x42 /* i64.const */) {
    long c = read_sint();
    PARSED;
    *--stack_head = opcode & 1 ? (unsigned)c : c;
}

DEF(float_const, 0x43 /* f32.const */, 0x44 /* f64.const */) {
    unsigned long value;
    __builtin_memcpy(&value, p, 8);
    if (opcode == 0x43) { // f32.const
        value &= -1U;
        p += 4;
    } else {
        p += 8;
    }
    PARSED;
    *--stack_head = value;
}

DEF(
    int_unop,
    0x45 = unop_eqz - unop_handlers, // i32.eqz
    0x50 = unop_eqz - unop_handlers, // i64.eqz
    0x67 = unop_clz32 - unop_handlers, // i32.clz
    0x68 = unop_ctz32 - unop_handlers, // i32.ctz
    0x69 = unop_popcnt - unop_handlers, // i32.popcnt
    0x79 = unop_clz64 - unop_handlers, // i64.clz
    0x7a = unop_ctz64 - unop_handlers, // i64.ctz
    0x7b = unop_popcnt - unop_handlers, // i64.popcnt
    0x8b = unop_abs - unop_handlers + 1, // f32.abs
    0x8c = unop_neg - unop_handlers + 1, // f32.neg
    0x99 = unop_abs - unop_handlers, // f64.abs
    0x9a = unop_neg - unop_handlers, // f64.neg
    0xa7 = unop_trunc - unop_handlers, // i32.wrap_i64
    0xac = unop_sxt32 - unop_handlers, // i64.extend_i32_s
    0xad = unop_trunc - unop_handlers, // i64.extend_i32_u
    0xb6 = unop_dtof - unop_handlers, // f32.demote_f64
    0xbb = unop_dtof - unop_handlers + 1, // f64.promote_f32
    0xc0 = unop_sxt8 - unop_handlers + 1, // i32.extend8_s
    0xc1 = unop_sxt16 - unop_handlers + 1, // i32.extend16_s
    0xc2 = unop_sxt8 - unop_handlers, // i64.extend8_s
    0xc3 = unop_sxt16 - unop_handlers, // i64.extend16_s
    0xc4 = unop_sxt32 - unop_handlers, // i64.extend32_s
) {
    PARSED;
    extern unsigned char unop_handlers;
    unsigned long xmm = *stack_head;
    asm (
        "call *%[handler];"
        ".pushsection .text.op;"
        "unop_handlers:"
        "unop_eqz: test %0, %0; mov $0, %k0; sete %b0; ret;"
        "unop_clz32: lzcnt %k0, %k0; ret;"
        "unop_clz64: lzcnt %0, %0; ret;"
        "unop_ctz32: tzcnt %k0, %k0; ret;"
        "unop_ctz64: tzcnt %0, %0; ret;"
        "unop_popcnt: popcnt %0, %0; ret;"
        "unop_abs: btr $63, %0; ret;"
        "unop_neg: btc $63, %0; ret;"
        "unop_trunc: mov %k0, %k0; ret;"
        "unop_sxt32: movsx %k0, %0; ret;"
        "unop_dtof: cvtpd2ps %1, %1; movq %1, %0; ret;"
        "unop_sxt8: movsx %b0, %0; ret;"
        "unop_sxt16: movsx %w0, %0; ret;"
        ".popsection"
        : "+R"(*stack_head), "+x"(xmm)
        : [handler]"r"(&unop_handlers + arg)
        : "flags"
    );
}

DEF(
    int_compare,
    0x46 = 0x94, // i32.eq
    0x47 = 0x95, // i32.ne
    0x48 = 0x9c, // i32.lt_s
    0x49 = 0x92, // i32.lt_u
    0x4a = 0x9f, // i32.gt_s
    0x4b = 0x97, // i32.gt_u
    0x4c = 0x9e, // i32.le_s
    0x4d = 0x96, // i32.le_u
    0x4e = 0x9d, // i32.ge_s
    0x4f = 0x93, // i32.ge_u
    0x51 = 0x94, // i64.eq
    0x52 = 0x95, // i64.ne
    0x53 = 0x9c, // i64.lt_s
    0x54 = 0x92, // i64.lt_u
    0x55 = 0x9f, // i64.gt_s
    0x56 = 0x97, // i64.gt_u
    0x57 = 0x9e, // i64.le_s
    0x58 = 0x96, // i64.le_u
    0x59 = 0x9d, // i64.ge_s
    0x5a = 0x93, // i64.ge_u
) {
    PARSED;
    unsigned long b = *stack_head++;
    unsigned long a = *stack_head;
    if (opcode < 0x51 && opcode % 2 == 0) { // i32.eq or i32.*_s
        // sign-extend
        a = (long)(int)a;
        b = (long)(int)b;
    }
    _Bool cond;
    asm (
        "cmp %2, %1;"
        "mov %3, 1f + 1(%%rip);"
        "1:"
        "setb %0"
        : "=R"(cond)
        : "r"(a), "r"(b), "r"(arg)
        : "flags"
    );
    *stack_head = cond;
}

DEF(
    float_compare,
    0x5b = 0x0, // f32.eq
    0x5c = 0x4, // f32.ne
    0x5d = 0x1, // f32.lt
    0x5e = 0xe, // f32.gt
    0x5f = 0x2, // f32.le
    0x60 = 0xd, // f32.ge
    0x61 = 0x0, // f64.eq
    0x62 = 0x4, // f64.ne
    0x63 = 0x1, // f64.lt
    0x64 = 0xe, // f64.gt
    0x65 = 0x2, // f64.le
    0x66 = 0xd, // f64.ge
) {
    PARSED;
    unsigned long *b = stack_head++;
    unsigned long out;
    asm (
        "mov %[imm8], 1f + 4(%%rip);"
        "cmp $0x61, %[opcode];"
        "jb 1f + 1;" // f32
        "1:"
        "cmppd $0, %[b], %[a]"
        : "=x"(out)
        : [a]"0"(*stack_head), [b]"x"(*b), [opcode]"r"(opcode), [imm8]"r"(arg)
        : "flags"
    );
    *stack_head = out & 1;
}

DEF(
    int_binop,
    0x6a = binop_add - binop_handlers + 1, // i32.add
    0x6b = binop_sub - binop_handlers + 1, // i32.sub
    0x6c = binop_mul - binop_handlers + 1, // i32.mul
    0x6d = binop_div_s32 - binop_handlers, // i32.div_s
    0x6e = binop_div_u - binop_handlers + 1, // i32.div_u
    0x6f = binop_rem_s32 - binop_handlers, // i32.rem_s
    0x70 = binop_rem_u - binop_handlers + 1, // i32.rem_u
    0x71 = binop_and - binop_handlers + 1, // i32.and
    0x72 = binop_or - binop_handlers + 1, // i32.or
    0x73 = binop_xor - binop_handlers + 1, // i32.xor
    0x74 = binop_shl - binop_handlers + 1, // i32.shl
    0x75 = binop_shr_s - binop_handlers + 1, // i32.shr_s
    0x76 = binop_shr_u - binop_handlers + 1, // i32.shr_u
    0x77 = binop_rotl - binop_handlers + 1, // i32.rotl
    0x78 = binop_rotr - binop_handlers + 1, // i32.rotr
    0x7c = binop_add - binop_handlers, // i64.add
    0x7d = binop_sub - binop_handlers, // i64.sub
    0x7e = binop_mul - binop_handlers, // i64.mul
    0x7f = binop_div_s64 - binop_handlers, // i64.div_s
    0x80 = binop_div_u - binop_handlers, // i64.div_u
    0x81 = binop_rem_s64 - binop_handlers, // i64.rem_s
    0x82 = binop_rem_u - binop_handlers, // i64.rem_u
    0x83 = binop_and - binop_handlers, // i64.and
    0x84 = binop_or - binop_handlers, // i64.or
    0x85 = binop_xor - binop_handlers, // i64.xor
    0x86 = binop_shl - binop_handlers, // i64.shl
    0x87 = binop_shr_s - binop_handlers, // i64.shr_s
    0x88 = binop_shr_u - binop_handlers, // i64.shr_u
    0x89 = binop_rotl - binop_handlers, // i64.rotl
    0x8a = binop_rotr - binop_handlers, // i64.rotr
) {
    PARSED;
    extern unsigned char binop_handlers;
    unsigned long b = *stack_head++;
    unsigned long a = *stack_head;
    unsigned long zero = 0;
    asm (
        "call *%[handler];"
        ".pushsection .text.op;"
        "binop_handlers:"
        "binop_add: add %[b], %[a]; ret;"
        "binop_sub: sub %[b], %[a]; ret;"
        "binop_mul: imul %[b], %[a]; ret;"
        "binop_div_s32: cdq; jmp binop_div_s64 + 3;"
        "binop_div_s64: cqo; idiv %[b]; ret;"
        "binop_div_u: div %[b]; ret;"
        "binop_rem_s32: cmp $-1, %k[b]; je 2f; cdq; jmp 1f + 1;"
        "binop_rem_s64: cmp $-1, %[b]; je 2f; cqo; 1: idiv %[b]; 2: mov %%rdx, %%rax; ret;"
        "binop_rem_u: div %[b]; mov %%rdx, %%rax; ret;"
        "binop_and: and %[b], %[a]; ret;"
        "binop_or: or %[b], %[a]; ret;"
        "binop_xor: xor %[b], %[a]; ret;"
        "binop_shl: shl %b[b], %[a]; ret;"
        "binop_shr_s: sar %b[b], %[a]; ret;"
        "binop_shr_u: shr %b[b], %[a]; ret;"
        "binop_rotl: rol %b[b], %[a]; ret;"
        "binop_rotr: ror %b[b], %[a]; ret;"
        ".popsection"
        : [a]"+a"(a), "+d"(zero) // specific register and zero for `div`
        : [b]"c"(b), [handler]"r"(&binop_handlers + arg) // specific register for shifts
        : "flags"
    );
    *stack_head = a;
}

DEF(
    round,
    0x8d = 0b10, // f32.ceil
    0x8e = 0b01, // f32.floor
    0x8f = 0b11, // f32.trunc
    0x90 = 0b00, // f32.nearest
    0x9b = 0b10, // f64.ceil
    0x9c = 0b01, // f64.floor
    0x9d = 0b11, // f64.trunc
    0x9e = 0b00, // f64.nearest
) {
    PARSED;
    unsigned char size_byte = 0x0a + (opcode >= 0x9b); // f64
    asm (
        "mov %[size_byte], 1f + 3(%%rip);"
        "mov %[mode], 1f + 5(%%rip);"
        "1:"
        "roundsd $0, %0, %0"
        : "+x"(*stack_head)
        : [size_byte]"r"(size_byte), [mode]"r"(arg)
        : "memory"
    );
}

DEF(sqrt, 0x91 = 0 /* f32.sqrt */, 0x9f = 1 /* f64.sqrt */) {
    PARSED;
    asm (
        "test %1, %1;"
        "je 1f + 1;"
        "1: sqrtpd %0, %0"
        : "+x"(*stack_head)
        : "r"(arg)
        : "flags"
    );
}

DEF(
    float_binop,
    0x92 = 0x58, // f32.add
    0x93 = 0x5c, // f32.sub
    0x94 = 0x59, // f32.mul
    0x95 = 0x5e, // f32.div
    0xa0 = 0x58, // f64.add
    0xa1 = 0x5c, // f64.sub
    0xa2 = 0x59, // f64.mul
    0xa3 = 0x5e, // f64.div
) {
    PARSED;
    unsigned long *b = stack_head++;
    unsigned long *a = stack_head;
    asm (
        "mov %3, 1f + 2(%%rip);"
        "cmp $0xa0, %2;"
        "jb 1f + 1;" // f32
        "1:"
        "addpd %1, %0;"
        : "+x"(*a)
        : "x"(*b), "r"(opcode), "r"(arg)
        : "flags"
    );
}

DEF(
    float_minmax,
    0x96 = 0xeb, // f32.min
    0x97 = 0xdb, // f32.max
    0xa4 = 0xeb, // f64.min
    0xa5 = 0xdb, // f64.max
) {
    PARSED;
    unsigned long *b = stack_head++;
    asm (
        "mov %[op], 2f + 2(%%rip);"
        "cmp $0xa4, %[opcode];"
        "jb 1f + 1;" // f32
        "1: ucomisd %[b], %[a];"
        "jp 3f;"
        "je 2f;"
        "adc $5, %[op];"
        "jnp 5f;"
        "movq %[b], %[a];"
        "2: pand %[b], %[a]; jmp 5f;" // -0 considered less than +0
        "3: cmp $0xa4, %[opcode];"
        "jb 4f + 1;"
        "4: addpd %[b], %[a];"
        "5:"
        : [a]"+x"(*stack_head)
        : [b]"x"(*b), [opcode]"r"(opcode), [op]"r"(arg)
        : "flags"
    );
}

DEF(copysign, 0x98 = 33 /* f32.copysign */, 0xa6 = 1 /* f64.copysign */) {
    PARSED;
    unsigned long b = *stack_head++;
    asm (
        "shl %[c], %[a];"
        "shl %[c], %[b];"
        "rcr %[c], %[a];"
        : [a]"+r"(*stack_head), [b]"+r"(b)
        : [c]"c"(arg)
        : "flags"
    );
}

DEF(
    float_to_int, 
    0xa8 = 0b11, // i32.trunc_f32_s
    0xa9 = 0b11, // i32.trunc_f32_u
    0xaa = 0b10, // i32.trunc_f64_s
    0xab = 0b10, // i32.trunc_f64_u
    0xae = 0b01, // i64.trunc_f32_s
    0xaf = 0b01, // i64.trunc_f32_u
    0xb0 = 0b00, // i64.trunc_f64_s
    0xb1 = 0b00, // i64.trunc_f64_u
) {
    PARSED;

    double x;
    asm ("movq %1, %0" : "=x"(x) : "m"(*stack_head));
    if (arg & 1) { // f32
        float f;
        __builtin_memcpy(&f, &x, 4);
        x = f;
    }

    unsigned long out;
    asm ("cvttsd2si %1, %0;" : "=r"(out) : "x"(x));

    if (opcode & 1) { // inn.trunc_f64_u
        if ((long)out < 0) {
            out = (out << 11) | (1UL << 63);
        }
    } else {
        if (arg & 2) { // i32
            out = (unsigned)out;
        }
    }

    *stack_head = out;
}

DEF(
    int_to_float,
    0xb2 = 1, // f32.convert_i32_s
    0xb3 = 0, // f32.convert_i32_u
    0xb4 = 0, // f32.convert_i64_s
    0xb5 = 2, // f32.convert_i64_u
    0xb7 = 1, // f64.convert_i32_s
    0xb8 = 0, // f64.convert_i32_u
    0xb9 = 0, // f64.convert_i64_s
    0xba = 2, // f64.convert_i64_u
) {
    PARSED;

    unsigned long x = *stack_head;

    // extend input to 64-bit
    if (arg == 1) { // fnn.convert_i32_s
        x = (long)(int)x;
    }

    double out;
    asm ("cvtsi2sd %1, %0;" : "=x"(out) : "r"(x));
    if (arg == 2 && (long)x < 0) { // fnn.convert_i64_u
        x = (x >> 1) | (x & 1);
        asm ("cvtsi2sd %1, %0;" : "=x"(out) : "r"(x));
        out += out;
    }

    if (opcode < 0xb7) { // f32
        float f = out;
        out = 0;
        __builtin_memcpy(&out, &f, 4);
    }
    __builtin_memcpy(stack_head, &out, 8);
}

DEF(fc_prefix, 0xfc) {
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
}

#include "table.i"

static void eval_instr() {
    unsigned char opcode = *p++;
    unsigned short value = opcode_map[opcode];
    void (*handler)(unsigned char, unsigned char) = handlers[value & 0xff];
    handler(opcode, value >> 8);
}

static void call_func(unsigned funcidx) {
    struct func_info *info = &funcs[funcidx];
    if (info->typeidx == -1U) {
        // native code
        asm volatile (
            "call *%0"
            :
            : "a"(info->func)
            : "memory", "flags", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11"
            // this should ideally also list xmm registers but eugh
        );
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
    memset(locals + n_args, 0, n_locals * 8);
    stack_head += n_args;
    p = body_p;

    // printf("push func\n");
    *caller_stack_head++ = (struct caller_info) {
        .opcode = 0x10 /* call */,
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
