CC      = gcc
CFLAGS  = -Wall -Wextra -Wpedantic -std=c11 -O2 -g -Iinclude
LDFLAGS = -lz -lm -ljpeg

SRC		= src/main.c src/image.c src/png.c src/jpeg.c src/lsb.c src/embed_lsb.c src/embed_ss.c
TARGET  = bin/imgpoison

all:
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

run: all
	$(TARGET)

valgrind: all
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all run valgrind clean

