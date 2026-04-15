OPTS := -O2 -fno-align-functions -fno-align-jumps -fno-align-labels -fno-align-loops

interp: interp.c
	$(CC) $< -o $@ $(OPTS) -Wall -Wextra -g -fno-stack-protector -nostdlib -static

interp-debug: interp.c
	$(CC) $< -o $@ $(OPTS) -Wall -Wextra -g -fsanitize=address -nostartfiles

interp-small: interp.c
	$(CC) $< -o $@ $(OPTS) -Wall -Wextra -fno-stack-protector -nostdlib -static -fno-asynchronous-unwind-tables -T small.ld
	strip --strip-section-headers $@
