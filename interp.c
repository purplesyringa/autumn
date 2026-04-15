#include <fcntl.h>
#include <stdio.h>
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
        } else {
            parse_p += byte_len;
        }
    }
}
