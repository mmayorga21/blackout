CC := gcc
CPPFLAGS := -Iinclude -D_POSIX_C_SOURCE=200809L
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -g -O0 -pthread
LDFLAGS := -pthread
SOURCES := $(wildcard src/*.c)
OBJECTS := $(patsubst src/%.c,build/%.o,$(SOURCES))
TARGET := bin/blackout
CONFIG ?= config/tienda-d.conf

.PHONY: all run test race-check clean
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

build/test_work_queue: tests/test_work_queue.c src/work_queue.c include/work_queue.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_work_queue.c src/work_queue.c $(LDFLAGS) -o $@

test: $(TARGET) build/fault_io.so build/test_work_queue
	python3 -m unittest discover -s tests -v

build/tsan:
	mkdir -p $@

build/tsan/blackout: $(SOURCES) $(wildcard include/*.h) | build/tsan
	$(CC) $(CPPFLAGS) $(CFLAGS) -O1 -fsanitize=thread -fno-pie -no-pie $(SOURCES) $(LDFLAGS) -o $@

build/tsan/test_work_queue: tests/test_work_queue.c src/work_queue.c include/work_queue.h | build/tsan
	$(CC) $(CPPFLAGS) $(CFLAGS) -O1 -fsanitize=thread -fno-pie -no-pie tests/test_work_queue.c src/work_queue.c $(LDFLAGS) -o $@

race-check: build/tsan/blackout build/tsan/test_work_queue
	python3 tests/check_races.py

clean:
	$(RM) -r build bin

-include $(OBJECTS:.o=.d)
