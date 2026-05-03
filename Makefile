OPTS := -Os -fno-align-functions -fno-align-jumps -fno-align-labels -fno-align-loops -msse4.2 -fno-jump-tables -mno-red-zone -mpreferred-stack-boundary=3 -fomit-frame-pointer

all: autumn autumn.png

autumn-unwrapped: interp.c build/table.i build/make-rwx
	$(CC) $< -o $@ $(OPTS) -Wall -Wextra -g -fno-stack-protector -nostdlib -static -fno-pie
	build/make-rwx $@

autumn-asan: interp.c build/table.i build/make-rwx
	$(CC) $< -o $@ $(OPTS) -Wall -Wextra -g -fsanitize=address -nostartfiles
	build/make-rwx $@

build/autumn.bin: interp.c build/table.i small.ld
	$(CC) $< -o $@ $(OPTS) -Wall -Wextra -fno-stack-protector -nostdlib -static -fno-pie -fno-asynchronous-unwind-tables -T small.ld -D COMPRESSED

build/autumn.e8.bin: build/autumn.bin scripts/e8-encode.py
	python3 scripts/e8-encode.py <$< >$@

build/table.i: interp.c scripts/make-table.py
	python3 scripts/make-table.py

build/make-rwx: scripts/make-rwx.c
	$(CC) $< -o $@ -O2

build/compressed.bin: build/autumn.e8.bin scripts/compress/src/main.rs
	cd scripts/compress && cargo run --release $(abspath $<) $(abspath build/compressed.bin) $(abspath build/models.bin)

autumn: decoder.asm build/compressed.bin
	nasm $< -o $@ -D output_len=$(shell stat -c %s build/autumn.e8.bin)
	chmod +x $@

autumn.png: autumn
	qrencode -8 -r $< -o $@
