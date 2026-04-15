#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

unsigned char module_bytes[1024 * 1024];
unsigned char *parse_p;

unsigned long read_uint() {
    int shift = 0;
    unsigned long out = 0;
    do {
        out |= (unsigned long)(*parse_p & 0x7f) << shift;
        shift += 7;
    } while (*parse_p++ & 0x80);
    return out;
}

unsigned char* declared_types[1024];
unsigned long globals[1024];
unsigned main_funcidx;
unsigned start_funcidx = -1;
unsigned long func_table[1024];
unsigned char* funcs[1024];
unsigned n_funcs;
unsigned func_types[1024];
unsigned char memory[2 * 1024 * 1024];
unsigned long stack[1024];
unsigned long* stack_head = stack;
unsigned long* locals;

unsigned broken_blocks;

void eval_until(unsigned char terminator);
void call_func(unsigned funcidx);

void eval_instr() {
#define PARSED if (broken_blocks) break;

    unsigned char opcode = *parse_p++;
    switch (opcode) {
    case 0x00:
        // unreachable
        PARSED
        __builtin_trap();
    case 0x01:
        // nop
        break;
    case 0x02: {
        // block
        parse_p++; // blocktype
        _Bool executed = broken_blocks == 0;
        eval_until(0x0b);
        broken_blocks -= executed && broken_blocks > 0;
        break;
    }
    case 0x03: {
        // loop
        parse_p++; // blocktype
        unsigned char *p = parse_p;
        _Bool executed = broken_blocks == 0;
        do {
            parse_p = p;
            eval_until(0x0b);
            broken_blocks -= executed && broken_blocks > 0;
        } while (broken_blocks == 0);
        break;
    }
    case 0x04: {
        // if..end
        parse_p++; // blocktype
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
    case 0x1b: {
        PARSED;
        unsigned long b = *--stack_head;
        unsigned long a = *--stack_head;
        stack_head[-1] = stack_head[-1] ? a : b;
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
    case 0x28:
    case 0x2d: {
        // 0x28 i32.load
        // 0x2d i32.load8_u
        read_uint(); // align
        unsigned offset = read_uint();
        PARSED;
        unsigned address = offset + stack_head[-1];
        unsigned value = 0;
        unsigned len = opcode == 0x28 ? 4 : opcode == 0x2d ? 1 : -1U;
        memcpy(&value, memory + address, len);
        stack_head[-1] = value;
        break;
    }
    case 0x36: {
        // i32.store
        read_uint(); // align
        unsigned offset = read_uint();
        PARSED;
        unsigned value = *--stack_head;
        unsigned address = offset + *--stack_head;
        memcpy(memory + address, &value, 4);
        break;
    }
    case 0x41: {
        // i32.const
        unsigned c = read_uint();
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
    case 0x49:
    case 0x4b: {
        // 0x46 i32.eq
        // 0x47 i32.ne
        // 0x49 i32.lt_u
        // 0x4b i32.gt_u
        PARSED;
        unsigned long b = *--stack_head;
        unsigned long a = stack_head[-1];
        _Bool cond = (
            opcode == 0x46 ? a == b :
            opcode == 0x47 ? a != b :
            opcode == 0x49 ? a < b :
            opcode == 0x4b ? a > b :
            0
        );
        stack_head[-1] = cond;
        break;
    }
    case 0x6a: {
        // i32.add
        PARSED;
        unsigned long b = *--stack_head;
        stack_head[-1] = (unsigned)(stack_head[-1] + b);
        break;
    }
    case 0x6b: {
        // i32.sub
        PARSED;
        unsigned long b = *--stack_head;
        stack_head[-1] = (unsigned)(stack_head[-1] - b);
        break;
    }
    case 0x80: {
        // i64.div_u
        PARSED;
        unsigned long b = *--stack_head;
        stack_head[-1] /= b;
        break;
    }
    case 0x81: {
        // i64.rem_s
        PARSED;
        long b = *--stack_head;
        stack_head[-1] = (long)stack_head[-1] % b;
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
    while (*parse_p != terminator) {
        eval_instr();
    }
    parse_p++;
}

void call_func(unsigned funcidx) {
    parse_p = funcs[funcidx];

    unsigned n_local_groups = read_uint();
    unsigned n_locals = 0;
    while (n_local_groups--) {
        n_locals += read_uint(); // n
        parse_p++; // valtype
    }

    unsigned long this_locals[n_locals];
    unsigned long *prev_locals = locals;
    locals = this_locals;

    unsigned char *p = parse_p;
    parse_p = declared_types[func_types[funcidx]];
    unsigned n_args = read_uint();
    memcpy(locals, stack_head - n_args, n_args * 8);
    stack_head -= n_args;
    parse_p = p;

    eval_until(0x0b);
    broken_blocks = 0;

    locals = prev_locals;
}

int main(int argc, char **argv) {
    (void)argc;

    int fd = open(argv[1], O_RDONLY);
    int len = read(fd, module_bytes, sizeof(module_bytes));

    parse_p = module_bytes + 8;
    while (parse_p != module_bytes + len) {
        unsigned char section_type = *parse_p++;
        unsigned byte_len = read_uint();
        printf("section of type %d of length %u\n", section_type, byte_len);

        if (section_type == 1) {
            // Type section
            unsigned n_functypes = read_uint();
            printf("%u types\n", n_functypes);
            for (unsigned i = 0; i < n_functypes; i++) {
                parse_p++; // 0x60
                declared_types[i] = parse_p;
                for (int j = 0; j < 2; j++) {
                    unsigned n_valtypes = read_uint();
                    parse_p += n_valtypes; // valtype is single-byte
                }
            }
        } else if (section_type == 2) {
            // Import section
            unsigned n_imports = read_uint();
            printf("%u imports\n", n_imports);

            while (n_imports--) {
                unsigned mod_len = read_uint();
                parse_p += mod_len;

                unsigned name_len = read_uint();
                printf("import %.*s\n", name_len, parse_p);
                parse_p += name_len;

                n_funcs++; // TODO: populate funcs

                parse_p++; // 0x00
                read_uint();
            }
        } else if (section_type == 3) {
            // Function section
            unsigned n_sigs = read_uint();
            printf("%u function signatures\n", n_sigs);
            for (unsigned i = 0; i < n_sigs; i++) {
                func_types[i] = read_uint();
            }
        } else if (section_type == 6) {
            // Global section
            unsigned n_globals = read_uint();
            printf("%u globals\n", n_globals);

            for (unsigned i = 0; i < n_globals; i++) {
                unsigned char valtype = *parse_p++;
                parse_p++; // mut
                parse_p++; // t.const
                switch (valtype) {
                case 0x7f:
                case 0x7e:
                    // i32/i64
                    globals[i] = read_uint();
                    break;
                case 0x7d:
                    // f32
                    memcpy(&globals[i], parse_p, 4);
                    parse_p += 4;
                    break;
                case 0x7c:
                    // f64
                    memcpy(&globals[i], parse_p, 8);
                    parse_p += 8;
                    break;
                }
                parse_p++; // end
            }
        } else if (section_type == 7) {
            // Export section
            unsigned n_exports = read_uint();
            printf("%u exports\n", n_exports);

            for (unsigned i = 0; i < n_exports; i++) {
                unsigned name_len = read_uint();
                printf("export %.*s\n", name_len, parse_p);
                _Bool is_start = name_len == 6 && memcmp(parse_p, "_start", 6) == 0;
                parse_p += name_len;
                parse_p++; // exportdesc variant
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
                parse_p++; // 0x00 tableidx
                parse_p++; // i32.const
                unsigned offset = read_uint();
                parse_p++; // end
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
                funcs[n_funcs++] = parse_p;
                parse_p += size;
            }
        } else if (section_type == 11) {
            // Data section
            unsigned n_datas = read_uint();
            printf("%u datas\n", n_datas);

            for (unsigned i = 0; i < n_datas; i++) {
                parse_p++; // 0x00 memidx
                parse_p++; // i32.const
                unsigned offset = read_uint();
                parse_p++; // end
                unsigned len = read_uint();
                printf("%u..%u bytes\n", offset, offset + len);
                memcpy(memory + offset, parse_p, len);
                parse_p += len;
            }
        } else {
            parse_p += byte_len;
        }
    }

    if (start_funcidx != (unsigned)-1) {
        call_func(start_funcidx);
    }
    call_func(main_funcidx);
}
