#include <elf.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/mman.h>

int main(int argc, char **argv) {
	(void)argc;
	int fd = open(argv[1], O_RDWR);
	void *p = mmap(NULL, 1024 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	Elf64_Ehdr *elf = p;
	for (int i = 0; i < elf->e_phnum; i++) {
		Elf64_Phdr *phdr = (void *)((char *)p + elf->e_phoff + elf->e_phentsize * i);
		if (phdr->p_type == PT_LOAD && (phdr->p_flags & PF_X)) {
			phdr->p_flags |= PF_W;
		}
	}
}
