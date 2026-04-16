OPTS := -Os -fno-align-functions -fno-align-jumps -fno-align-labels -fno-align-loops -msse4.2

interp: interp.c make-rwx
	$(CC) $< -o $@ $(OPTS) -Wall -Wextra -g -fno-stack-protector -nostdlib -static
	./make-rwx $@

interp-debug: interp.c make-rwx
	$(CC) $< -o $@ $(OPTS) -Wall -Wextra -g -fsanitize=address -nostartfiles
	./make-rwx $@

interp-small: interp.c
	$(CC) $< -o $@ $(OPTS) -Wall -Wextra -fno-stack-protector -nostdlib -static -fno-asynchronous-unwind-tables -T small.ld
	strip --strip-section-headers $@

make-rwx: make-rwx.c
	$(CC) $< -o $@ -O2
