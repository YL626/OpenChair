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

# oc_store tests. Written before the implementation: until src/oc_store.c is
# real these targets are EXPECTED TO FAIL. They move into `make test` once green.
STORE_TESTS := test_store test_store_faults test_store_kill
# Fault-injection seam: the faults binary routes oc_store's write/sync calls
# through __wrap_* functions in tests/test_store_faults.c.
STORE_WRAP := -Wl,--wrap=write,--wrap=pwrite,--wrap=writev,--wrap=pwritev,--wrap=fsync,--wrap=fdatasync
ASAN_ENV := ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1

.PHONY: all test asan memcheck run soakcheck clean test-store asan-store memcheck-store
.DEFAULT_GOAL := all

all: $(PLAIN)/openchair

test: $(PLAIN)/test_smoke $(PLAIN)/test_sample $(PLAIN)/test_light $(PLAIN)/test_i2c_linux \
		$(PLAIN)/test_ipc_msg $(PLAIN)/test_ipc $(PLAIN)/openchair
	./$(PLAIN)/test_smoke
	./$(PLAIN)/test_sample
	./$(PLAIN)/test_light
	./$(PLAIN)/test_i2c_linux
	./$(PLAIN)/test_ipc_msg
	./$(PLAIN)/test_ipc

asan: $(ASAN)/test_smoke $(ASAN)/test_sample $(ASAN)/test_light $(ASAN)/test_i2c_linux \
		$(ASAN)/test_ipc_msg $(ASAN)/test_ipc $(ASAN)/openchair
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		./$(ASAN)/test_smoke
	$(ASAN_ENV) ./$(ASAN)/test_sample
	$(ASAN_ENV) ./$(ASAN)/test_light
	$(ASAN_ENV) ./$(ASAN)/test_i2c_linux
	$(ASAN_ENV) ./$(ASAN)/test_ipc_msg
	$(ASAN_ENV) ./$(ASAN)/test_ipc

memcheck: $(PLAIN)/test_smoke $(PLAIN)/openchair
	@command -v valgrind >/dev/null || { echo "memcheck: valgrind not installed" >&2; exit 1; }
	valgrind --error-exitcode=1 --leak-check=full --errors-for-leak-kinds=definite,possible \
		--trace-children=yes --log-file=$(BUILD)/valgrind-%p.log ./$(PLAIN)/test_smoke
	@echo "memcheck: no errors; logs in $(BUILD)/valgrind-*.log"

test-store: $(addprefix $(PLAIN)/,$(STORE_TESTS))
	@rc=0; for t in $^; do ./$$t || rc=1; done; exit $$rc

asan-store: $(addprefix $(ASAN)/,$(STORE_TESTS))
	@rc=0; for t in $^; do $(ASAN_ENV) ./$$t || rc=1; done; exit $$rc

# Valgrind errors exit 99 so they are reported separately from test failures.
memcheck-store: $(addprefix $(PLAIN)/,$(STORE_TESTS))
	@command -v valgrind >/dev/null || { echo "memcheck-store: valgrind not installed" >&2; exit 1; }
	@verr=0; tfail=0; for t in $^; do \
		valgrind -q --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=definite,possible \
			--log-file=$(BUILD)/valgrind-store-%p.log ./$$t; st=$$?; \
		if [ $$st -eq 99 ]; then verr=1; elif [ $$st -ne 0 ]; then tfail=1; fi; \
	done; \
	echo "memcheck-store: valgrind errors: $$verr, test failures: $$tfail; logs in $(BUILD)/valgrind-store-*.log"; \
	[ $$verr -eq 0 ] && [ $$tfail -eq 0 ]

# oc_sample codec tests (no hardware, no storage dependency).
.PHONY: test-sample asan-sample memcheck-sample

test-sample: $(PLAIN)/test_sample
	./$<

asan-sample: $(ASAN)/test_sample
	$(ASAN_ENV) ./$<

memcheck-sample: $(PLAIN)/test_sample
	@command -v valgrind >/dev/null || { echo "memcheck-sample: valgrind not installed" >&2; exit 1; }
	valgrind -q --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=definite,possible ./$<

# ADS1115 light reader tests (SIMULATED I2C only; no hardware. Sources use enum oc_origin from oc_store.h and
# every test links all of src/, so oc_store must compile).
# test_i2c_linux wraps ioctl to script I2C_FUNCS / I2C_RDWR.
.PHONY: test-light asan-light memcheck-light
$(PLAIN)/test_i2c_linux $(ASAN)/test_i2c_linux: LDWRAP := -Wl,--wrap=ioctl
LIGHT_TESTS := test_light test_i2c_linux

test-light: $(addprefix $(PLAIN)/,$(LIGHT_TESTS))
	@rc=0; for t in $^; do ./$$t || rc=1; done; exit $$rc

asan-light: $(addprefix $(ASAN)/,$(LIGHT_TESTS))
	@rc=0; for t in $^; do $(ASAN_ENV) ./$$t || rc=1; done; exit $$rc

memcheck-light: $(addprefix $(PLAIN)/,$(LIGHT_TESTS))
	@command -v valgrind >/dev/null || { echo "memcheck-light: valgrind not installed" >&2; exit 1; }
	@rc=0; for t in $^; do valgrind -q --error-exitcode=99 --leak-check=full \
		--errors-for-leak-kinds=definite,possible ./$$t || rc=1; done; exit $$rc

# IPC tests: control-message codec (pure) and the AF_UNIX SOCK_SEQPACKET
# transport over REAL sockets in a private tmp dir. Sensor data is SIMULATED;
# no hardware. test_ipc wraps connect/send/recvmsg as fault-injection seams
# (default mode: call the real function).
.PHONY: test-ipc asan-ipc memcheck-ipc
$(PLAIN)/test_ipc $(ASAN)/test_ipc: LDWRAP := -Wl,--wrap=connect,--wrap=send,--wrap=recvmsg
IPC_TESTS := test_ipc_msg test_ipc

test-ipc: $(addprefix $(PLAIN)/,$(IPC_TESTS))
	@rc=0; for t in $^; do ./$$t || rc=1; done; exit $$rc

asan-ipc: $(addprefix $(ASAN)/,$(IPC_TESTS))
	@rc=0; for t in $^; do $(ASAN_ENV) ./$$t || rc=1; done; exit $$rc

memcheck-ipc: $(addprefix $(PLAIN)/,$(IPC_TESTS))
	@command -v valgrind >/dev/null || { echo "memcheck-ipc: valgrind not installed" >&2; exit 1; }
	@rc=0; for t in $^; do valgrind -q --error-exitcode=99 --leak-check=full \
		--errors-for-leak-kinds=definite,possible ./$$t || rc=1; done; exit $$rc

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

# --- oc_store test binaries (LDWRAP is set per target for fault injection) ---
$(PLAIN)/test_store_faults $(ASAN)/test_store_faults: LDWRAP := $(STORE_WRAP)

$(PLAIN)/test_%: $(TEST)/test_%.c $(PLAIN_LIB_OBJ) | $(PLAIN)
	$(CC) $(CFLAGS_PLAIN) $< $(PLAIN_LIB_OBJ) $(LDWRAP) -o $@

$(ASAN)/test_%: $(TEST)/test_%.c $(ASAN_LIB_OBJ) | $(ASAN)
	$(CC) $(CFLAGS_ASAN) $(LDFLAGS_ASAN) $< $(ASAN_LIB_OBJ) $(LDWRAP) -o $@

$(PLAIN) $(ASAN):
	mkdir -p $@

-include $(wildcard $(PLAIN)/*.d $(ASAN)/*.d)
