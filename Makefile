all: interp

interp: interp.c
	$(CC) $< -o $@ -O2 -Wall
