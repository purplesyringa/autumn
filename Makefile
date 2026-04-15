all: interp

interp: interp.c
	$(CC) $< -o $@ -O2 -Wall -Wextra -g -fsanitize=address -nostartfiles

interp-small: interp.c
	$(CC) $< -o $@ -O2 -Wall -Wextra -fno-align-functions -fno-stack-protector -nostdlib -static
