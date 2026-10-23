CC := gcc

WARNFLAGS := -std=c23 -Wall -Wextra -Werror -pedantic
CFLAGS_COMMON := $(WARNFLAGS) -Iinclude -MMD -MP
CFLAGS_PLAIN := $(CFLAGS_COMMON) -O0 -g
CFLAGS_ASAN := $(CFLAGS_COMMON) -O1 -g -fsanitize=address,undefined \
	-fno-sanitize-recover=undefined -fno-omit-frame-pointer
LDFLAGS_ASAN := -fsanitize=address,undefined

BUILD := build
SRC := src
TEST := tests

LIB_SRC := $(filter-out $(SRC)/main.c,$(wildcard $(SRC)/*.c))
LIB_NAMES := $(notdir $(LIB_SRC:.c=.o))

# Plain (uninstrumented) and ASan/UBSan builds live in separate trees and are never mixed.
PLAIN := $(BUILD)/plain
ASAN := $(BUILD)/asan

PLAIN_LIB_OBJ := $(addprefix $(PLAIN)/,$(LIB_NAMES))
ASAN_LIB_OBJ := $(addprefix $(ASAN)/,$(LIB_NAMES))

.PHONY: all test asan memcheck run soakcheck clean
.DEFAULT_GOAL := all

all: $(PLAIN)/openchair

test: $(PLAIN)/test_smoke $(PLAIN)/openchair
	./$(PLAIN)/test_smoke

asan: $(ASAN)/test_smoke $(ASAN)/openchair
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		./$(ASAN)/test_smoke

memcheck: $(PLAIN)/test_smoke $(PLAIN)/openchair
	@command -v valgrind >/dev/null || { echo "memcheck: valgrind not installed" >&2; exit 1; }
	valgrind --error-exitcode=1 --leak-check=full --errors-for-leak-kinds=definite,possible \
		--trace-children=yes --log-file=$(BUILD)/valgrind-%p.log ./$(PLAIN)/test_smoke
	@echo "memcheck: no errors; logs in $(BUILD)/valgrind-*.log"

run:
	@echo "NOT AVAILABLE: no application exists yet (only a scaffold CLI)." >&2; exit 1

soakcheck:
	@echo "NOT AVAILABLE: no soak test exists; requires live hardware and a real application." >&2; exit 1

clean:
	rm -rf $(BUILD)

# --- plain build ---
$(PLAIN)/%.o: $(SRC)/%.c | $(PLAIN)
	$(CC) $(CFLAGS_PLAIN) -c $< -o $@

$(PLAIN)/openchair: $(PLAIN)/main.o $(PLAIN_LIB_OBJ)
	$(CC) $^ -o $@

$(PLAIN)/test_smoke: $(TEST)/test_smoke.c $(PLAIN_LIB_OBJ) | $(PLAIN)
	$(CC) $(CFLAGS_PLAIN) -DOPENCHAIR_BIN='"./$(PLAIN)/openchair"' $< $(PLAIN_LIB_OBJ) -o $@

# --- ASan/UBSan build ---
$(ASAN)/%.o: $(SRC)/%.c | $(ASAN)
	$(CC) $(CFLAGS_ASAN) -c $< -o $@

$(ASAN)/openchair: $(ASAN)/main.o $(ASAN_LIB_OBJ)
	$(CC) $(LDFLAGS_ASAN) $^ -o $@

$(ASAN)/test_smoke: $(TEST)/test_smoke.c $(ASAN_LIB_OBJ) | $(ASAN)
	$(CC) $(CFLAGS_ASAN) $(LDFLAGS_ASAN) -DOPENCHAIR_BIN='"./$(ASAN)/openchair"' $< $(ASAN_LIB_OBJ) -o $@

$(PLAIN) $(ASAN):
	mkdir -p $@

-include $(wildcard $(PLAIN)/*.d $(ASAN)/*.d)
