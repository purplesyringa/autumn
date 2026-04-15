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
unsigned char memory[2 * 1024 * 1024];

int main(int argc, char **argv) {
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

            for (unsigned i = 0; i < n_imports; i++) {
                unsigned mod_len = read_uint();
                parse_p += mod_len;

                unsigned name_len = read_uint();
                printf("import %.*s\n", name_len, parse_p);
                parse_p += name_len;

                parse_p++; // 0x00
                read_uint();
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

            for (unsigned i = 0; i < n_codes; i++) {
                unsigned int size = read_uint();
                funcs[i] = parse_p;
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
}
