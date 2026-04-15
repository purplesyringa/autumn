#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

unsigned char module_bytes[1024 * 1024];
unsigned char *p;

unsigned long read_uint() {
    int shift = 0;
    unsigned long out = 0;
    do {
        out |= (unsigned long)(*p & 0x7f) << shift;
        shift += 7;
    } while (*p++ & 0x80);
    return out;
}

struct func_info {
    unsigned char *func;
    unsigned typeidx;
};

unsigned char* declared_types[1024];
unsigned long globals[1024];
unsigned main_funcidx;
unsigned start_funcidx = -1;
unsigned long func_table[1024];
struct func_info funcs[1024];
unsigned n_funcs;
unsigned char memory[2 * 1024 * 1024];
unsigned long stack[1024];
unsigned long* stack_head = stack;
unsigned long* locals;

unsigned broken_blocks;

void eval_until(unsigned char terminator);
void call_func(unsigned funcidx);

void eval_instr() {
#define PARSED if (broken_blocks) break

    unsigned char opcode = *p++;
    switch (opcode) {
    case 0x00:
        // unreachable
        PARSED;
        __builtin_trap();
    case 0x01:
        // nop
        break;
    case 0x02: {
        // block
        p++; // blocktype
        _Bool executed = broken_blocks == 0;
        eval_until(0x0b);
        broken_blocks -= executed && broken_blocks > 0;
        break;
    }
    case 0x03: {
        // loop
        p++; // blocktype
        unsigned char *saved_p = p;
        _Bool executed = broken_blocks == 0;
        do {
            p = saved_p;
            eval_until(0x0b);
            broken_blocks -= executed && broken_blocks > 0;
        } while (broken_blocks == 0);
        break;
    }
    case 0x04: {
        // if..end
        p++; // blocktype
        _Bool executed = broken_blocks == 0;
        broken_blocks += executed && *--stack_head;
        eval_until(0x0b);
        broken_blocks -= executed && broken_blocks > 0;
        break;
    }
    case 0x0c: {
        // br
        unsigned labelidx = read_uint();
        PARSED;
        broken_blocks = labelidx + 1;
        break;
    }
    case 0x0d: {
        // br_if
        unsigned labelidx = read_uint();
        PARSED;
        if (*--stack_head) {
            broken_blocks = labelidx + 1;
        }
        break;
    }
    case 0x0e: {
        // br_table
        unsigned n_labels = read_uint();
        unsigned jump_table[n_labels];
        for (unsigned i = 0; i < n_labels; i++) {
            jump_table[i] = read_uint();
        }
        unsigned otherwise = read_uint();
        PARSED;
        unsigned i = *--stack_head;
        broken_blocks = i < n_labels ? jump_table[i] : otherwise;
        break;
    }
    case 0x0f: {
        // return
        PARSED;
        broken_blocks = -1U;
        break;
    }
    case 0x10: {
        // call
        unsigned funcidx = read_uint();
        PARSED;
        call_func(funcidx);
        break;
    }
    case 0x11: {
        // call_indirect
        read_uint(); // typeidx
        p++; // 0x00
        PARSED;
        unsigned tableidx = *--stack_head;
        call_func(func_table[tableidx]);
        break;
    }
    case 0x1b: {
        // select
        PARSED;
        unsigned long cond = *--stack_head;
        unsigned long b = *--stack_head;
        stack_head[-1] = cond ? stack_head[-1] : b;
        break;
    }
    case 0x20: {
        // local.get
        unsigned localidx = read_uint();
        PARSED;
        *stack_head++ = locals[localidx];
        break;
    }
    case 0x21: {
        // local.set
        unsigned localidx = read_uint();
        PARSED;
        locals[localidx] = *--stack_head;
        break;
    }
    case 0x22: {
        // local.tee
        unsigned localidx = read_uint();
        PARSED;
        locals[localidx] = stack_head[-1];
        break;
    }
    case 0x23: {
        // global.get
        unsigned globalidx = read_uint();
        PARSED;
        *stack_head++ = globals[globalidx];
        break;
    }
    case 0x24: {
        // global.set
        unsigned globalidx = read_uint();
        PARSED;
        globals[globalidx] = *--stack_head;
        break;
    }
    case 0x28:
    case 0x29:
    case 0x2d: {
        // 0x28 i32.load
        // 0x29 i64.load
        // 0x2d i32.load8_u
        read_uint(); // align
        unsigned offset = read_uint();
        PARSED;
        unsigned address = offset + stack_head[-1];
        unsigned long value = 0;
        unsigned len = (
            opcode == 0x28 ? 4 :
            opcode == 0x29 ? 8 :
            opcode == 0x2d ? 1 :
            -1U
        );
        memcpy(&value, memory + address, len);
        stack_head[-1] = value;
        break;
    }
    case 0x36: 
    case 0x37:
    case 0x3a:
    case 0x3b: {
        // 0x36 i32.store
        // 0x37 i64.store
        // 0x3a i32.store8
        // 0x3b i32.store16
        read_uint(); // align
        unsigned offset = read_uint();
        PARSED;
        unsigned long value = *--stack_head;
        unsigned address = offset + *--stack_head;
        unsigned len = (
            opcode == 0x36 ? 4 :
            opcode == 0x37 ? 8 :
            opcode == 0x3a ? 1 :
            opcode == 0x3b ? 2 :
            -1U
        );
        memcpy(memory + address, &value, len);
        break;
    }
    case 0x41:
    case 0x42: {
        // 0x41 i32.const
        // 0x42 i64.const
        unsigned long c = read_uint();
        PARSED;
        *stack_head++ = c;
        break;
    }
    case 0x45: {
        // i32.eqz
        PARSED;
        stack_head[-1] = stack_head[-1] == 0;
        break;
    }
    case 0x46:
    case 0x47:
    case 0x48:
    case 0x49:
    case 0x4a:
    case 0x4b:
    case 0x51:
    case 0x52: {
        // 0x46 i32.eq
        // 0x47 i32.ne
        // 0x48 i32.lt_s
        // 0x49 i32.lt_u
        // 0x4a i32.gt_s
        // 0x4b i32.gt_u
        // 0x51 i64.eq
        // 0x52 i64.ne
        PARSED;
        unsigned long b = *--stack_head;
        unsigned long a = stack_head[-1];
        _Bool cond = (
            opcode == 0x46 || opcode == 0x51 ? a == b :
            opcode == 0x47 || opcode == 0x52 ? a != b :
            opcode == 0x48 ? (int)a < (int)b :
            opcode == 0x49 ? a < b :
            opcode == 0x4a ? (int)a > (int)b :
            opcode == 0x4b ? a > b :
            0
        );
        stack_head[-1] = cond;
        break;
    }
    case 0x6a:
    case 0x6b:
    case 0x71:
    case 0x76:
    case 0x7c:
    case 0x80:
    case 0x81: {
        // 0x6a i32.add
        // 0x6b i32.sub
        // 0x71 i32.and
        // 0x76 i32.shr_u
        // 0x7c i64.add
        // 0x80 i64.div_u
        // 0x81 i64.rem_s
        PARSED;
        unsigned long b = *--stack_head;
        unsigned long a = stack_head[-1];
        unsigned long value = (
            opcode == 0x6a ? (unsigned)(a + b) :
            opcode == 0x6b ? (unsigned)(a - b) :
            opcode == 0x71 ? a & b :
            opcode == 0x76 ? a >> (b % 32) :
            opcode == 0x7c ? a + b :
            opcode == 0x80 ? a / b :
            opcode == 0x81 ? (unsigned long)((long)a % (long)b) :
            0
        );
        stack_head[-1] = value;
        break;
    }
    case 0x99: {
        // f64.abs
        PARSED;
        stack_head[-1] = stack_head[-1] & ((-1ULL) >> 1);
        break;
    }
    default:
        printf("Unknown opcode 0x%02x\n", opcode);
        __builtin_trap();
    }
}

void eval_until(unsigned char terminator) {
    while (*p != terminator) {
        eval_instr();
    }
    p++;
}

void call_func(unsigned funcidx) {
    struct func_info* info = &funcs[funcidx];
    if (info->typeidx == -1U) {
        // native code
        ((void (*)())info->func)();
        return;
    }

    // printf("Enter %u\n", funcidx);
    unsigned char *prev_p = p;
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

    unsigned long this_locals[n_args + n_locals];
    unsigned long *prev_locals = locals;
    locals = this_locals;

    memcpy(locals, stack_head - n_args, n_args * 8);
    stack_head -= n_args;
    p = body_p;

    eval_until(0x0b);
    broken_blocks = 0;

    locals = prev_locals;
    p = prev_p;
    // printf("Exit %u\n", funcidx);
}

void fd_write() {
    unsigned n_written = *--stack_head;
    unsigned iovs_len = *--stack_head;
    unsigned iovs = *--stack_head;
    unsigned fd = stack_head[-1];

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
    ssize_t native_out = writev(fd, native_iovs, iovs_len);

    unsigned long wasi_out;
    if (native_out == -1) {
        wasi_out = errno;
    } else {
        memcpy(memory + n_written, &native_out, 4);
        wasi_out = 0;
    }

    stack_head[-1] = wasi_out;
}

int main(int argc, char **argv) {
    (void)argc;

    int fd = open(argv[1], O_RDONLY);
    int len = read(fd, module_bytes, sizeof(module_bytes));

    p = module_bytes + 8;
    while (p != module_bytes + len) {
        unsigned char section_type = *p++;
        unsigned byte_len = read_uint();
        printf("section of type %d of length %u\n", section_type, byte_len);

        if (section_type == 1) {
            // Type section
            unsigned n_functypes = read_uint();
            printf("%u types\n", n_functypes);
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
            printf("%u imports\n", n_imports);

            while (n_imports--) {
                unsigned mod_len = read_uint();
                p += mod_len;

                unsigned name_len = read_uint();
                printf("import %.*s\n", name_len, p);

                void (*func)() = NULL;
                if (name_len == 8 && memcmp(p, "fd_write", 8) == 0) {
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
            printf("%u function signatures\n", n_sigs);
            for (unsigned i = 0; i < n_sigs; i++) {
                funcs[n_funcs + i].typeidx = read_uint();
            }
        } else if (section_type == 6) {
            // Global section
            unsigned n_globals = read_uint();
            printf("%u globals\n", n_globals);

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
                    memcpy(&globals[i], p, 4);
                    p += 4;
                    break;
                case 0x7c:
                    // f64
                    memcpy(&globals[i], p, 8);
                    p += 8;
                    break;
                }
                p++; // end
            }
        } else if (section_type == 7) {
            // Export section
            unsigned n_exports = read_uint();
            printf("%u exports\n", n_exports);

            for (unsigned i = 0; i < n_exports; i++) {
                unsigned name_len = read_uint();
                printf("export %.*s\n", name_len, p);
                _Bool is_start = name_len == 6 && memcmp(p, "_start", 6) == 0;
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
                printf("%u..%u elems\n", offset, offset + n_funcidxs);
                while (n_funcidxs--) {
                    func_table[offset++] = read_uint();
                }
            }
        } else if (section_type == 10) {
            // Code section
            unsigned n_codes = read_uint();
            printf("%u codes\n", n_codes);

            while (n_codes--) {
                unsigned int size = read_uint();
                funcs[n_funcs++].func = p;
                p += size;
            }
        } else if (section_type == 11) {
            // Data section
            unsigned n_datas = read_uint();
            printf("%u datas\n", n_datas);

            for (unsigned i = 0; i < n_datas; i++) {
                p++; // 0x00 memidx
                p++; // i32.const
                unsigned offset = read_uint();
                p++; // end
                unsigned len = read_uint();
                printf("%u..%u bytes\n", offset, offset + len);
                memcpy(memory + offset, p, len);
                p += len;
            }
        } else {
            p += byte_len;
        }
    }

    if (start_funcidx != (unsigned)-1) {
        call_func(start_funcidx);
    }
    call_func(main_funcidx);
}
