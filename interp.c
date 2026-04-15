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

int main(int argc, char **argv) {
    int fd = open(argv[1], O_RDONLY);
    int len = read(fd, module_bytes, sizeof(module_bytes));

    parse_p = module_bytes + 8;
    while (parse_p != module_bytes + len) {
        unsigned char section_type = *parse_p++;
        printf("section of type %d\n", section_type);
        unsigned len = read_uint();
        parse_p += len;
    }
}
