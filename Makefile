CC := gcc
CPPFLAGS := -Iinclude -D_POSIX_C_SOURCE=200809L
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -g -O0 -pthread
LDFLAGS := -pthread
SOURCES := $(wildcard src/*.c)
OBJECTS := $(patsubst src/%.c,build/%.o,$(SOURCES))
TARGET := bin/blackout
CONFIG ?= config/tienda-d.conf

.PHONY: all run test clean
all: $(TARGET)

$(TARGET): $(OBJECTS) | bin
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

build/%.o: src/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

build bin:
	mkdir -p $@

run: $(TARGET)
	./$(TARGET) --config "$(CONFIG)"

build/fault_io.so: tests/fault_io.c | build
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -fPIC -shared $< -ldl -o $@

test: $(TARGET) build/fault_io.so
	python3 -m unittest discover -s tests -v

clean:
	$(RM) -r build bin

-include $(OBJECTS:.o=.d)
