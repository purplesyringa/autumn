all: interp

interp: interp.c
	$(CC) $< -o $@ -O2
