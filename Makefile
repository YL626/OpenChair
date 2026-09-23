CC := gcc

CFLAGS := -std=c23 -Wall -Wextra -Werror -pedantic
DBGFLAGS := -g
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

BUILD := build
SRC := src
TEST := tests

.PHONY: all clean test asan memcheck run soakcheck

all:
	@echo "No production targets implemented yet."

test:
	@echo "No tests implemented yet."

asan:
	@echo "No sanitizer targets implemented yet."

memcheck:
	@echo "No valgrind targets implemented yet."

run:
	@echo "Application not implemented yet."

soakcheck:
	@echo "Soak test not implemented yet."

clean:
	rm -rf $(BUILD)
	