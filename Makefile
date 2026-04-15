interp: interp.c
	$(CC) $< -o $@ -O2 -Wall -Wextra -g -fno-align-functions -fno-stack-protector -nostdlib -static

interp-debug: interp.c
	$(CC) $< -o $@ -O2 -Wall -Wextra -g -fsanitize=address -nostartfiles

interp-small: interp.c
	$(CC) $< -o $@ -O2 -Wall -Wextra -fno-align-functions -fno-stack-protector -nostdlib -static -fno-asynchronous-unwind-tables -T small.ld
	strip --strip-section-headers $@
