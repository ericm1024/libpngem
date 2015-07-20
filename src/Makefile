CC=clang
CFLAGS=-Wall -Wextra -pedantic -std=c99

png: png.o chunk.o error.o
	$(CC) $(CFLAGS) -o $@ $^

png.o: png.c
	$(CC) $(CFLAGS) -c $< -o $@

chunk.o: chunk.c chunk.h int.h util.h
	$(CC) $(CFLAGS) -c $< -o $@

error.o: error.c error.h
	$(CC) $(CFLAGS) -c $< -o $@
