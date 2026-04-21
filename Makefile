OPTS := -Os -fno-align-functions -fno-align-jumps -fno-align-labels -fno-align-loops -msse4.2 -fno-jump-tables -mno-red-zone

interp: interp.c table.i make-rwx
	$(CC) $< -o $@ $(OPTS) -Wall -Wextra -g -fno-stack-protector -nostdlib -static -fno-pie
	./make-rwx $@

interp-debug: interp.c table.i make-rwx
	$(CC) $< -o $@ $(OPTS) -Wall -Wextra -g -fsanitize=address -nostartfiles
	./make-rwx $@

interp-small.bin: interp.c table.i small.ld
	$(CC) $< -o $@ $(OPTS) -Wall -Wextra -fno-stack-protector -nostdlib -static -fno-pie -fno-asynchronous-unwind-tables -T small.ld

table.i: interp.c make-table.py
	python3 make-table.py

make-rwx: make-rwx.c
	$(CC) $< -o $@ -O2

compressed.bin models.bin initial.txt: interp-small.bin compress/src/main.rs
	cd compress && cargo run --release

interp-small: decoder.asm compressed.bin models.bin initial.txt
	nasm $< -o $@ -D output_len=$(shell stat -c %s interp-small.bin) -D initial=$(file <initial.txt)
	chmod +x $@
