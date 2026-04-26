#include <errno.h>
#include <fcntl.h>
#include <immintrin.h>
#include <poll.h>
#include <stdint.h>
// #include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

// Put this symbol after the rest of the .bss so that all relocations fit in 32 bits.
__attribute__((section(".bss.memory"), aligned(4096)))
unsigned char memory[0x100000007]; // 7 bytes for OOB accesses in `load`
unsigned long memory_pages;
unsigned max_memory_pages = 1 << 16;

unsigned char module_bytes[3 * 1024 * 1024];
register unsigned char *p asm ("r12");

static void *memcpy(void *dst, const void *src, size_t n) {
    void *tmp = dst;
    asm volatile ("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
    return tmp;
}

static void *memmove(void *dst, const void *src, size_t n) {
    void *tmp = dst;
    asm volatile (
        "cmp %0, %1;"
        "jb 1f;" // src < dst
        "add %2, %0;"
        "dec %0;"
        "add %2, %1;"
        "dec %1;"
        "std;"
        "1: rep movsb;"
        "cld"
        : "+D"(dst), "+S"(src), "+c"(n)
        :
        : "memory", "flags"
    );
    return tmp;
}

static void *memset(void *s, int c, size_t n) {
    void *orig_s = s;
    asm volatile ("rep stosb" : "+D"(s), "+c"(n) : "a"(c) : "memory");
    return orig_s;
}

static size_t strlen(const char *s) {
    unsigned long count = -1;
    asm (
        "repne scasb"
        : "+D"(s), "+c"(count)
        : "a"(0)
        : "flags"
    );
    return -2 - count;
}

static long syscall1(long sysno, long a) {
    asm volatile ("syscall" : "+a"(sysno) : "D"(a) : "rcx", "r11", "memory");
    return sysno;
}
static long syscall2(long sysno, long a, long b) {
    asm volatile ("syscall" : "+a"(sysno) : "D"(a), "S"(b) : "rcx", "r11", "memory");
    return sysno;
}
static long syscall3(long sysno, long a, long b, long c) {
    asm volatile ("syscall" : "+a"(sysno) : "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return sysno;
}

struct read_int_output {
    unsigned long value;
    unsigned char shift;
};

// For some reason this function insists on increasing the required stack alignment, so all of its
// direct callers must be `noinline`.
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

__attribute__((noinline))
static unsigned long read_uint() {
    struct read_int_output out = impl_read_int();
    return out.value >> out.shift;
}

__attribute__((noinline))
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
unsigned long stack[1024];
register unsigned long *stack_head asm ("r15");
unsigned long locals_stack[1024];
unsigned long *locals = locals_stack + sizeof(locals_stack) / sizeof(locals_stack[0]);
register unsigned break_level asm ("r14");

static void call_func(unsigned funcidx);

#ifdef __SANITIZE_ADDRESS__
#define PARSED if (break_level) return
#else
// Technically unsound, but oh well. I couldn't find a better way to make GCC inline the prologue.
#define PARSED if (break_level) { asm volatile ("ret"); __builtin_unreachable(); }
#endif

#define DEF(name, ...) \
    __attribute__((section(".text"))) /* don't put op_unknown in .text.unlikely for relocations */ \
    void op_##name( \
        __attribute__((unused)) unsigned char opcode, \
        __attribute__((unused)) unsigned char arg \
    )

static void push(unsigned long value) {
    // `*--stack_head = value;`, but more optimized.
    asm ("sub $8, %0" : "+r"(stack_head) :: "flags");
    *stack_head = value;
}

static void eval_instr();

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
    unsigned char *saved_p = p - 1;

    unsigned n_params, n_results;
    if ((*p >> 6) == 1) { // type indicator
        n_params = 0;
        n_results = *p++ != 0x40; // epsilon blocktype
    } else { // s32
        unsigned typeidx = read_uint();
        unsigned char *saved_p = p;
        p = declared_types[typeidx];
        n_params = read_uint();
        p += n_params;
        n_results = read_uint();
        p = saved_p;
    }

    _Bool skipped_if = break_level == 0 && opcode == 0x04 && !*stack_head++;

    unsigned long *saved_stack_head = stack_head;
    break_level += break_level > 0 || skipped_if;

    while (*p != 0x0b) {
        if (*p == 0x05) {
            break_level += break_level == 0;
            break_level -= skipped_if;
            p++;
        } else {
            eval_instr();
        }
    }
    p++; // end

    if (break_level > 0) {
        break_level--;
        if (break_level == 0 && opcode == 0x03) { // loop
            p = saved_p;
            n_results = n_params;
        }
    }

    if (break_level == 0) {
        unsigned long *new_stack_head = saved_stack_head - n_results;
        // This copy can overlap, but our `memcpy` copies data in the right direction.
        memcpy(new_stack_head, stack_head, n_results * sizeof(*stack_head));
        stack_head = new_stack_head;
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

DEF(select, 0x1b) {
    PARSED;
    unsigned long cond = *stack_head++;
    unsigned long b = *stack_head++;
    if (!cond) {
        *stack_head = b;
    }
}

DEF(get, 0x20 /* local.get */, 0x23 /* global.get */) {
    unsigned idx = read_uint();
    PARSED;
    push((opcode == 0x23 ? globals : locals)[idx]);
}

DEF(set_like, 0x21 = 1 /* local.set */, 0x22 = 0 /* local.tee */, 0x24 = 1 /* global.set */) {
    unsigned idx = read_uint();
    PARSED;
    (opcode == 0x24 ? globals : locals)[idx] = *stack_head;
    stack_head += arg;
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
    p++; // memidx
    PARSED;
    push(memory_pages);
}

DEF(memory_grow, 0x40) {
    p++; // memidx
    PARSED;
    unsigned long out = memory_pages;
    unsigned long increase = *stack_head;
    if (memory_pages + increase <= max_memory_pages) {
        memory_pages += increase;
    } else {
        out = -1;
    }
    *stack_head = out;
}

DEF(int_const, 0x41 /* i32.const */, 0x42 /* i64.const */) {
    long c = read_sint();
    PARSED;
    push(opcode & 1 ? (unsigned)c : c);
}

DEF(float_const, 0x43 = 4 /* f32.const */, 0x44 = 8 /* f64.const */) {
    unsigned long value;
    __builtin_memcpy(&value, p, 8);
    p += arg;
    PARSED;
    if (opcode == 0x43) { // f32.const
        value &= -1U;
    }
    push(value);
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
    0x91 = unop_sqrt - unop_handlers + 1, // f32.sqrt
    0x99 = unop_abs - unop_handlers, // f64.abs
    0x9a = unop_neg - unop_handlers, // f64.neg
    0x9f = unop_sqrt - unop_handlers, // f64.sqrt
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
        "unop_sqrt: sqrtpd %1, %1; movq %1, %0; ret;"
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
    _Bool out;
    asm (
        "cmp $0x51, %[opcode];"
        "jb 1f + 1;" // i32
        "1: cmp %[b], %[a];"
        "mov %[cond_byte], 2f + 1(%%rip);"
        "2: setb %[out]"
        : [out]"=R"(out)
        : [a]"R"(a), [b]"R"(b), [cond_byte]"r"(arg), [opcode]"r"(opcode)
        : "flags"
    );
    *stack_head = out;
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
    unsigned char size_byte = 0xf8 + (opcode >= 0x61); // f64
    unsigned long out;
    asm (
        "mov %[size_byte], 1f + 1(%%rip);"
        "mov %[imm8], 1f + 4(%%rip);"
        "1:"
        "vcmppd $0, (%[b]), %[a], %[out]"
        : [out]"=x"(out)
        : [a]"Yz"(*stack_head), [b]"r"(b), [size_byte]"r"(size_byte), [imm8]"r"(arg)
        : "memory"
    );
    *stack_head = out & 1;
}

DEF(
    int_binop,
    0x1a = binop_drop - binop_handlers, // drop
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
    0x98 = binop_copysign32 - binop_handlers, // f32.copysign
    0xa6 = binop_copysign64 - binop_handlers, // f64.copysign
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
        "binop_drop: ret;"
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
        "binop_copysign32: shl %k[a]; shl %k[b]; rcr %k[a]; ret;"
        "binop_copysign64: shl %[a]; shl %[b]; rcr %[a]; ret;"
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
        "vroundsd $0, (%1), %0, %0"
        : "=x"(*stack_head)
        : "r"(stack_head), [size_byte]"r"(size_byte), [mode]"r"(arg)
        : "memory"
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

DEF(
    float_to_int, 
    // The `arg` is the corresponding `0xfc`-prefixed `inn.trunc_sat_fnn_[su]` opcode, so that this
    // function can be reused for both.
    0xa8 = 0, // i32.trunc_f32_s
    0xa9 = 1, // i32.trunc_f32_u
    0xaa = 2, // i32.trunc_f64_s
    0xab = 3, // i32.trunc_f64_u
    0xae = 4, // i64.trunc_f32_s
    0xaf = 5, // i64.trunc_f32_u
    0xb0 = 6, // i64.trunc_f64_s
    0xb1 = 7, // i64.trunc_f64_u
) {
    PARSED;

    double x;
    asm ("movq %1, %0" : "=x"(x) : "m"(*stack_head));
    if (!(arg & 2)) { // f32
        float f;
        __builtin_memcpy(&f, &x, 4);
        x = f;
    }

    unsigned long out;
    unsigned bitness = 31 + (arg & 1) + 32 * ((arg & 4) != 0);

    asm goto (
        "ucomisd %1, %0;"
        "jp %l2;"
        "jb %l3;"
        :
        : "x"(x), "x"((1023UL + bitness) << 52 /* pow2(bitness) */)
        : "flags"
        : maxsd, below_limit
    );

    // above limit
    out = -1UL >> ((64 - bitness) % 64);
    goto done;

below_limit:
    if (arg & 1) { // unn
    maxsd:
        asm ("maxsd %1, %0" : "+x"(x) : "x"(0));
    }

    asm ("cvttsd2si %1, %0;" : "=r"(out) : "x"(x));
    if (arg == 0 || arg == 2) { // i32.trunc_fnn_s
        asm ("cvttsd2si %1, %k0;" : "=r"(out) : "x"(x));
    }

    if ((arg & 1) && (long)out < 0) { // inn.trunc_f64_u
        unsigned long value;
        __builtin_memcpy(&value, &x, 8);
        out = (value << 11) | (1UL << 63);
    }

done:
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

    // GCC is bad at removing copies to stack
    long double ld;
    asm ("fildq %1" : "=t"(ld) : "m"(*stack_head));
    if (arg == 1) {
        asm ("fildl %1" : "=t"(ld) : "m"(*stack_head));
    }
    asm ("" : "+r"(arg)); // prevent jump threading

    if (arg == 2 && (long)*stack_head < 0) { // fnn.convert_i64_u
        ld += 0x1p64;
    }

    *stack_head = 0;
    if (opcode < 0xb7) { // f32
        float f = ld;
        __builtin_memcpy(stack_head, &f, 4);
    } else {
        double d = ld;
        __builtin_memcpy(stack_head, &d, 8);
    }
}

DEF(fc_prefix, 0xfc) {
    opcode = *p++;
    if (opcode < 8) { // inn.trunc_sat_fnn_[su]
        op_float_to_int(0, opcode);
        return;
    }
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
    case 0x0b: // memory.fill
    {
        p++; // memidx
        PARSED;
        unsigned n = *stack_head++;
        unsigned c = *stack_head++;
        unsigned dst = *stack_head++;
        memset(memory + dst, c, n);
        break;
    }
    default:
        // printf("Unknown opcode 0xfc 0x%02x\n", opcode);
        __builtin_trap();
    }
}

#include "table.i"

extern char base_sym;

static void eval_instr() {
    unsigned char opcode = *p++;
    unsigned short value = opcode_map[opcode];
    void (*handler)(unsigned char, unsigned char) = (void *)(&base_sym + handlers[value & 0xff]);
    handler(opcode, value >> 8);
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
    p += n_args;
    unsigned n_results = read_uint();

    unsigned long *saved_locals = locals;
    locals -= n_args + n_locals;
    for (unsigned i = 0; i < n_args; i++) {
        locals[i] = stack_head[n_args - 1 - i];
    }
    memset(locals + n_args, 0, n_locals * 8);
    stack_head += n_args;
    p = body_p;

    unsigned long *saved_stack_head = stack_head;

    while (*p != 0x0b) {
        eval_instr();
    }

    break_level = 0;
    locals = saved_locals;
    p = saved_p;

    unsigned long *new_stack_head = saved_stack_head - n_results;
    // This copy can overlap, but our `memcpy` copies data in the right direction.
    memcpy(new_stack_head, stack_head, n_results * sizeof(*stack_head));
    stack_head = new_stack_head;
}

#define N_ERRNOS 10
static char errno_map[N_ERRNOS * 2] = {
    (char)-EAGAIN, (char)-EINTR, (char)-EBADF, (char)-EIO,
    (char)-EISDIR, (char)-EDQUOT, (char)-EFBIG, (char)-ENOSPC,
    (char)-EPIPE, (char)-EPERM,
    6, 27, 8, 29, 31, 19, 22, 51, 64, 63,
};

__attribute__((noinline))
static unsigned long map_to_errno(long out) {
    if (out >= 0) {
        return 0;
    }
    if ((short)out >= -255) {
        char *ptr = errno_map;
        unsigned long count = N_ERRNOS;
        _Bool found;
        asm (
            "repne scasb"
            : "+D"(ptr), "+c"(count), "=@cce"(found)
            : "a"((unsigned char)out)
        );
        if (found) {
            return *(ptr - 1 + N_ERRNOS);
        }
    }
    return 28; // EINVAL
}

#define DEF_IMPORT(name) void name() // deliberately not `static` to allow asm to reference this

static void fd_op(int syscallno) {
    unsigned n_processed = *stack_head++;
    unsigned iovs_len = *stack_head++;
    unsigned iovs = *stack_head++;
    unsigned fd = *stack_head;

    struct wasi_iovec {
        unsigned buf;
        unsigned buf_len;
    };
    struct wasi_iovec *wasi_iovs = (void*)(memory + iovs);
    static struct iovec native_iovs[1024];
    for (unsigned i = 0; i < iovs_len; i++) {
        native_iovs[i] = (struct iovec){
            .iov_base = memory + wasi_iovs[i].buf,
            .iov_len = wasi_iovs[i].buf_len,
        };
    }
    ssize_t native_out = syscall3(syscallno, fd, (long)native_iovs, iovs_len);

    unsigned long wasi_out = map_to_errno(native_out);
    if (native_out >= 0) {
        *(int*)(memory + n_processed) = native_out;
    }

    *stack_head = wasi_out;
}

DEF_IMPORT(fd_read) {
    fd_op(SYS_readv);
}
DEF_IMPORT(fd_write) {
    fd_op(SYS_writev);
}

DEF_IMPORT(fd_prestat_get) {
    stack_head++; // output buffer
    // fd is overwritten
    *stack_head = 8; // EBADF
}

DEF_IMPORT(fd_fdstat_get) {
    unsigned buf = *stack_head++;
    unsigned fd = *stack_head;

    if (fd >= 3) {
        *stack_head = 8; // EBADF;
        return;
    }

    struct wasi_stat {
        unsigned char filetype;
        unsigned short fdflags;
        unsigned long rights_base;
        unsigned long rights_inheriting;
    };
    struct wasi_stat *wasi_stat = (struct wasi_stat *)(memory + buf);
    *(int *)wasi_stat = 0; // filetype = fdflags = 0
    unsigned long rights = fd == 0 ? 0x2 /* FD_READ */ : 0x40 /* FD_WRITE */;
    wasi_stat->rights_base = wasi_stat->rights_inheriting = rights;
    *stack_head = 0;
}

DEF_IMPORT(poll_oneoff) {
    unsigned n_events_ptr = *stack_head++;
    unsigned n_subscriptions = *stack_head++;
    unsigned out = *stack_head++;
    unsigned in = *stack_head;

    *stack_head = 0;

    enum {
        CLOCK,
        FD_READ,
        FD_WRITE,
    };

    struct subscription {
        long userdata;
        unsigned char type;
        union {
            struct {
                int clock_id;
                unsigned long timeout;
                unsigned long precision;
                short flags;
            } clock;
            int fd;
        };
    };

    static struct pollfd fds[1024];
    unsigned timeout = -1;

    struct subscription *subs = (struct subscription *)(memory + in);
    struct subscription *sub = subs;
    struct pollfd *pollfd = fds;
    for (unsigned i = 0; i < n_subscriptions; i++, sub++, pollfd++) {
        pollfd->events = POLLIN;
        switch (sub->type) {
        case CLOCK:
            if (sub->clock.clock_id >= 4 || sub->clock.flags != 0) {
                *stack_head = 28; // EINVAL
                return;
            }
            unsigned this_timeout = sub->clock.timeout / 1'000'000;
            if (this_timeout < timeout) {
                timeout = this_timeout;
            }
            pollfd->fd = -1;
            break;
        case FD_WRITE:
            pollfd->events = POLLOUT;
            // fallthrough
        case FD_READ:
            pollfd->fd = sub->fd;
            break;
        default:
            *stack_head = 28; // EINVAL
            return;
        }
    }

    syscall3(SYS_poll, (long)fds, n_subscriptions, timeout);

    struct event {
        long userdata;
        short err;
        unsigned char type;
        struct {
            unsigned long n_bytes;
            unsigned short flags;
        } fd_read_write;
    };

    sub = subs;
    pollfd = fds;
    struct event *event = (struct event *)(memory + out);
    unsigned n_events = 0;

    for (unsigned i = 0; i < n_subscriptions; i++, sub++, pollfd++) {
        if (!pollfd->revents) {
            continue;
        }
        event->userdata = sub->userdata;
        event->err = 0;
        event->type = sub->type;
        event->fd_read_write.n_bytes = 1;
        event->fd_read_write.flags = 0;
        event++;
        n_events++;
    }

    *(unsigned*)(memory + n_events_ptr) = n_events;
}

DEF_IMPORT(clock_time_get) {
    unsigned out_ptr = *stack_head++;
    stack_head++; // precision
    unsigned long clock_id = *stack_head;

    // Clock IDs directly correspond to Linux clock IDs.
    if (clock_id >= 4) {
        *stack_head = 28; // EINVAL
        return;
    }

    static struct timespec tp;
    syscall2(SYS_clock_gettime, clock_id, (long)&tp);
    *(unsigned long*)(memory + out_ptr) = tp.tv_sec * 1000000000UL + tp.tv_nsec;
    *stack_head = 0;
}

DEF_IMPORT(random_get) {
    unsigned buf_len = *stack_head++;
    unsigned buf = *stack_head;
    ssize_t native_out;
    do {
        native_out = syscall3(SYS_getrandom, (long)(memory + buf), buf_len, 0);
        buf_len -= native_out;
        buf += native_out;
    } while (native_out > 0);
    *stack_head = map_to_errno(native_out);
}

static char **environ, **args;
static void syslist_impl(char **p, _Bool is_sizes) {
    unsigned arg2 = *stack_head++;
    unsigned arg1 = *stack_head;
    unsigned ptrs = 0;
    unsigned pos = is_sizes ? 0 : arg2;
    for (; *p; p++) {
        if (!is_sizes) {
            ((unsigned*)(memory + arg1))[ptrs] = pos;
        }
        ptrs++;
        unsigned len = strlen(*p);
        if (!is_sizes) {
            memcpy(memory + pos, *p, len + 1);
        }
        pos += len + 1;
    }
    if (is_sizes) {
        *(unsigned*)(memory + arg2) = pos;
        *(unsigned*)(memory + arg1) = ptrs;
    }
    *stack_head = 0;
}
DEF_IMPORT(environ_get) { syslist_impl(environ, 0); }
DEF_IMPORT(environ_sizes_get) { syslist_impl(environ, 1); }
DEF_IMPORT(args_get) { syslist_impl(args, 0); }
DEF_IMPORT(args_sizes_get) { syslist_impl(args, 1); }

DEF_IMPORT(proc_exit) {
    syscall1(SYS_exit, *stack_head);
    __builtin_trap();
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    args = argv + 1;
    environ = envp;

    syscall3(SYS_madvise, (long)memory, sizeof(memory) & -4096, MADV_DONTDUMP);

    int fd = syscall2(SYS_open, (long)argv[1], O_RDONLY);
    int len = syscall3(SYS_read, fd, (long)module_bytes, sizeof(module_bytes));

    stack_head = stack + sizeof(stack) / sizeof(stack[0]) - 1;

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
                if (name_len < 8) {
                    name <<= 64 - name_len * 8;
                }
                unsigned long crc32 = name_len;
                asm ("crc32 %1, %0" : "+r"(crc32) : "r"(name));

                void (*func)() = NULL;
                for (unsigned short *ip = imports; ip != imports_end; ip += 2) {
                    if (*ip == (unsigned short)crc32) {
                        func = (void *)(&base_sym + ip[1]);
                        break;
                    }
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
        } else if (section_type == 5) {
            // Memory section
            p++; // 1 memory
            _Bool has_limit = *p++;
            memory_pages = read_uint();
            if (has_limit) {
                max_memory_pages = read_uint();
            }
        } else if (section_type == 6) {
            // Global section
            unsigned n_globals = read_uint();
            // printf("%u globals\n", n_globals);

            for (unsigned i = 0; i < n_globals; i++) {
                p++; // valtype
                p++; // mut
                do {
                    eval_instr();
                } while (*p != 0x0b); // end
                p++; // end
                globals[i] = *stack_head++;
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
                _Bool is_start = name_len == 6 && (name << 16) == 0x74726174735f0000 /* _start */;
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
    break_level = 0;
    if (start_funcidx != (unsigned)-1) {
        call_func(start_funcidx);
    }
    call_func(main_funcidx);
}

asm (
    ".pushsection .text.start;"
    ".globl _start;"
    "_start:"

#ifdef COMPRESSED
    // e8 transform
    "mov $0x12345678, %edi;" // substituted by a script
    "mov $0x12345678, %ecx;"
    "mov $0xe8, %al;"
    "1: sub %edi, (%rdi);"
    "add $4, %edi;"
    "repne scasb;"
    "je 1b;"
#endif

    "pop %rdi;" // argc
    "mov %rsp, %rsi;" // argv
    "lea 8(%rsi,%rdi,8), %rdx;" // envp
    "call main;"
    "mov $60, %eax;" // exit
    "xor %edi, %edi;"
    "syscall;"
    ".popsection"
);
