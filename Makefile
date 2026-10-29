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

# oc_store tests (docs/oc_store_tests.md). Run by test/asan/memcheck, and on
# their own by test-store/asan-store/memcheck-store.
STORE_TESTS := test_store test_store_faults test_store_kill
# Fault-injection seam: the faults binary routes oc_store's write/sync calls
# through __wrap_* functions in tests/test_store_faults.c.
STORE_WRAP := -Wl,--wrap=write,--wrap=pwrite,--wrap=writev,--wrap=pwritev,--wrap=fsync,--wrap=fdatasync
# Deadline multipliers for the multiprocess suites (loose waits, never tight assertions).
ASAN_SCALE := 3
VALGRIND_SCALE := 8
ASAN_ENV := ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1

.PHONY: all test asan memcheck run soakcheck clean test-store asan-store memcheck-store \
	test-roles asan-roles test-multiproc asan-multiproc memcheck-multiproc \
	test-stress asan-stress test-capture asan-capture memcheck-capture test-m3a asan-m3a memcheck-m3a
.DEFAULT_GOAL := all

all: $(PLAIN)/openchair

test: $(PLAIN)/test_smoke $(PLAIN)/test_sample $(PLAIN)/test_light $(PLAIN)/test_i2c_linux \
		$(PLAIN)/test_ipc_msg $(PLAIN)/test_ipc $(PLAIN)/test_hub_seq $(PLAIN)/test_hub \
		$(PLAIN)/test_backoff $(PLAIN)/test_role_args $(PLAIN)/test_multiproc \
		$(PLAIN)/test_capture_fmt $(PLAIN)/test_capture $(PLAIN)/test_capture_faults \
		$(PLAIN)/test_distance $(PLAIN)/distance_probe $(PLAIN)/openchair test-store
	./$(PLAIN)/test_smoke
	./$(PLAIN)/test_sample
	./$(PLAIN)/test_light
	./$(PLAIN)/test_i2c_linux
	./$(PLAIN)/test_ipc_msg
	./$(PLAIN)/test_ipc
	./$(PLAIN)/test_hub_seq
	./$(PLAIN)/test_hub
	./$(PLAIN)/test_backoff
	./$(PLAIN)/test_role_args
	./$(PLAIN)/test_multiproc
	./$(PLAIN)/test_capture_fmt
	./$(PLAIN)/test_capture
	./$(PLAIN)/test_capture_faults
	./$(PLAIN)/test_distance

asan: $(ASAN)/test_smoke $(ASAN)/test_sample $(ASAN)/test_light $(ASAN)/test_i2c_linux \
		$(ASAN)/test_ipc_msg $(ASAN)/test_ipc $(ASAN)/test_hub_seq $(ASAN)/test_hub \
		$(ASAN)/test_backoff $(ASAN)/test_role_args $(ASAN)/test_multiproc \
		$(ASAN)/test_capture_fmt $(ASAN)/test_capture $(ASAN)/test_capture_faults \
		$(ASAN)/test_distance $(ASAN)/openchair asan-store
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		./$(ASAN)/test_smoke
	$(ASAN_ENV) ./$(ASAN)/test_sample
	$(ASAN_ENV) ./$(ASAN)/test_light
	$(ASAN_ENV) ./$(ASAN)/test_i2c_linux
	$(ASAN_ENV) ./$(ASAN)/test_ipc_msg
	$(ASAN_ENV) ./$(ASAN)/test_ipc
	$(ASAN_ENV) ./$(ASAN)/test_hub_seq
	$(ASAN_ENV) ./$(ASAN)/test_hub
	$(ASAN_ENV) ./$(ASAN)/test_backoff
	$(ASAN_ENV) ./$(ASAN)/test_role_args
	$(ASAN_ENV) OC_TEST_TIME_SCALE=$(ASAN_SCALE) ./$(ASAN)/test_multiproc
	$(ASAN_ENV) ./$(ASAN)/test_capture_fmt
	$(ASAN_ENV) ./$(ASAN)/test_capture
	$(ASAN_ENV) ./$(ASAN)/test_capture_faults
	$(ASAN_ENV) ./$(ASAN)/test_distance



memcheck: $(PLAIN)/test_smoke $(PLAIN)/openchair memcheck-store
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

# Hub receiver tests: pure sequence tracker, and the hub over REAL AF_UNIX
# SOCK_SEQPACKET sockets with SIMULATED samples (no hardware). test_hub wraps
# send() to inject one ACK send failure (default: call the real send).
.PHONY: test-hub asan-hub memcheck-hub
$(PLAIN)/test_hub $(ASAN)/test_hub: LDWRAP := -Wl,--wrap=send
HUB_TESTS := test_hub_seq test_hub

test-hub: $(addprefix $(PLAIN)/,$(HUB_TESTS))
	@rc=0; for t in $^; do ./$$t || rc=1; done; exit $$rc

asan-hub: $(addprefix $(ASAN)/,$(HUB_TESTS))
	@rc=0; for t in $^; do $(ASAN_ENV) ./$$t || rc=1; done; exit $$rc

memcheck-hub: $(addprefix $(PLAIN)/,$(HUB_TESTS))
	@command -v valgrind >/dev/null || { echo "memcheck-hub: valgrind not installed" >&2; exit 1; }
	@rc=0; for t in $^; do valgrind -q --error-exitcode=99 --leak-check=full \
		--errors-for-leak-kinds=definite,possible ./$$t || rc=1; done; exit $$rc

# S3C role units (backoff, simulated source, option parsing) and the multiprocess
# suites. Every sample is SIMULATED. The CORE suite is part of `make test` / `make asan`;
# the STRESS suite (descriptor exhaustion, same-instance respawn) is separate and
# heavier. timeout(1) is the outer hard limit on top of the suites' own deadlines.
ROLE_TESTS := test_backoff test_role_args
MP_TIMEOUT := 300

test-roles: $(addprefix $(PLAIN)/,$(ROLE_TESTS))
	@rc=0; for t in $^; do ./$$t || rc=1; done; exit $$rc

asan-roles: $(addprefix $(ASAN)/,$(ROLE_TESTS))
	@rc=0; for t in $^; do $(ASAN_ENV) ./$$t || rc=1; done; exit $$rc

test-multiproc: $(PLAIN)/test_multiproc
	timeout $(MP_TIMEOUT) ./$<

asan-multiproc: $(ASAN)/test_multiproc
	$(ASAN_ENV) OC_TEST_TIME_SCALE=$(ASAN_SCALE) timeout $(MP_TIMEOUT) ./$<

test-stress: $(PLAIN)/test_multiproc_stress
	timeout $(MP_TIMEOUT) ./$<

asan-stress: $(ASAN)/test_multiproc_stress
	$(ASAN_ENV) OC_TEST_TIME_SCALE=$(ASAN_SCALE) timeout $(MP_TIMEOUT) ./$<

# Valgrind: role units, then the core multiprocess suite with every exec'd child
# traced (per-pid logs in build/valgrind-mp-*.log). Instrumentation-sensitive
# assertions (CPU time, exact descriptor counts) are skipped via
# OC_TEST_UNDER_VALGRIND; deadlines are scaled by VALGRIND_SCALE.
memcheck-multiproc: $(addprefix $(PLAIN)/,$(ROLE_TESTS)) $(PLAIN)/test_multiproc $(PLAIN)/openchair
	@command -v valgrind >/dev/null || { echo "memcheck-multiproc: valgrind not installed" >&2; exit 1; }
	@rc=0; for t in $(addprefix $(PLAIN)/,$(ROLE_TESTS)); do valgrind -q --error-exitcode=99 \
		--leak-check=full --errors-for-leak-kinds=definite,possible ./$$t || rc=1; done; \
	[ $$rc -eq 0 ] || exit $$rc
	rm -f $(BUILD)/valgrind-mp-*.log
	OC_TEST_UNDER_VALGRIND=1 OC_TEST_TIME_SCALE=$(VALGRIND_SCALE) timeout $$(( $(MP_TIMEOUT) * 4 )) \
		valgrind --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=definite,possible \
		--trace-children=yes --child-silent-after-fork=no --log-file=$(BUILD)/valgrind-mp-%p.log \
		./$(PLAIN)/test_multiproc
	@echo "memcheck-multiproc: no errors; logs in $(BUILD)/valgrind-mp-*.log"

# M3-A: hub-to-oc_store integration (SIMULATED samples; software-only, NOT M3 evidence).
# Units/in-process (capture format, capture over a real oc_store, injected storage
# failures) are part of `make test` / `make asan`. The multiprocess suite (real hub
# with capture + two reader processes, EFBIG, hub SIGKILL recovery) is dedicated:
# test-m3a / asan-m3a / memcheck-m3a. The capture directory is persistent evidence
# and is kept on failure; only directories the harness created are ever deleted.
CAPTURE_TESTS := test_capture_fmt test_capture test_capture_faults

test-capture: $(addprefix $(PLAIN)/,$(CAPTURE_TESTS))
	@rc=0; for t in $^; do ./$$t || rc=1; done; exit $$rc

asan-capture: $(addprefix $(ASAN)/,$(CAPTURE_TESTS))
	@rc=0; for t in $^; do $(ASAN_ENV) ./$$t || rc=1; done; exit $$rc

memcheck-capture: $(addprefix $(PLAIN)/,$(CAPTURE_TESTS))
	@command -v valgrind >/dev/null || { echo "memcheck-capture: valgrind not installed" >&2; exit 1; }
	@rc=0; for t in $^; do valgrind -q --error-exitcode=99 --leak-check=full \
		--errors-for-leak-kinds=definite,possible ./$$t || rc=1; done; exit $$rc

test-m3a: $(PLAIN)/test_multiproc_capture
	timeout $(MP_TIMEOUT) ./$<

asan-m3a: $(ASAN)/test_multiproc_capture
	$(ASAN_ENV) OC_TEST_TIME_SCALE=$(ASAN_SCALE) timeout $(MP_TIMEOUT) ./$<

# CPU time and exact descriptor counts are skipped under Valgrind; deadlines are scaled.
# Per-pid logs for the test and every exec'd child: build/valgrind-m3a-*.log.
memcheck-m3a: $(PLAIN)/test_multiproc_capture $(PLAIN)/openchair
	@command -v valgrind >/dev/null || { echo "memcheck-m3a: valgrind not installed" >&2; exit 1; }
	rm -f $(BUILD)/valgrind-m3a-*.log
	OC_TEST_UNDER_VALGRIND=1 OC_TEST_TIME_SCALE=$(VALGRIND_SCALE) timeout $$(( $(MP_TIMEOUT) * 4 )) \
		valgrind --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=definite,possible \
		--trace-children=yes --child-silent-after-fork=no --log-file=$(BUILD)/valgrind-m3a-%p.log \
		./$(PLAIN)/test_multiproc_capture
	@echo "memcheck-m3a: no errors; logs in $(BUILD)/valgrind-m3a-*.log"

# VL6180X distance reader tests (SIMULATED I2C only; no hardware).
.PHONY: test-distance asan-distance memcheck-distance

test-distance: $(PLAIN)/test_distance
	./$<

asan-distance: $(ASAN)/test_distance
	$(ASAN_ENV) ./$<

memcheck-distance: $(PLAIN)/test_distance
	@command -v valgrind >/dev/null || { echo "memcheck-distance: valgrind not installed" >&2; exit 1; }
	valgrind -q --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=definite,possible ./$<

# Live VL6180X probe (docs/vl6180x.md). Builds anywhere; reads hardware only
# on a Pi: ./build/plain/distance_probe /dev/i2c-1 50 100
.PHONY: probe
probe: $(PLAIN)/distance_probe

$(PLAIN)/distance_probe: tools/distance_probe.c $(PLAIN_LIB_OBJ) | $(PLAIN)
	$(CC) $(CFLAGS_PLAIN) $< $(PLAIN_LIB_OBJ) -o $@

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

# S3C multiprocess suites exec the real openchair binary (OPENCHAIR_BIN) and
# are given the openchair target as a prerequisite so it is always current.
$(PLAIN)/test_multiproc $(PLAIN)/test_multiproc_stress $(PLAIN)/test_multiproc_capture: $(PLAIN)/test_%: $(TEST)/test_%.c $(TEST)/mp_harness.h $(PLAIN)/openchair $(PLAIN_LIB_OBJ) | $(PLAIN)
	$(CC) $(CFLAGS_PLAIN) -DOPENCHAIR_BIN='"./$(PLAIN)/openchair"' $< $(PLAIN_LIB_OBJ) -o $@

$(PLAIN)/test_smoke: $(TEST)/test_smoke.c $(PLAIN_LIB_OBJ) | $(PLAIN)
	$(CC) $(CFLAGS_PLAIN) -DOPENCHAIR_BIN='"./$(PLAIN)/openchair"' $< $(PLAIN_LIB_OBJ) -o $@

# --- ASan/UBSan build ---
$(ASAN)/%.o: $(SRC)/%.c | $(ASAN)
	$(CC) $(CFLAGS_ASAN) -c $< -o $@

$(ASAN)/openchair: $(ASAN)/main.o $(ASAN_LIB_OBJ)
	$(CC) $(LDFLAGS_ASAN) $^ -o $@

$(ASAN)/test_smoke: $(TEST)/test_smoke.c $(ASAN_LIB_OBJ) | $(ASAN)
	$(CC) $(CFLAGS_ASAN) $(LDFLAGS_ASAN) -DOPENCHAIR_BIN='"./$(ASAN)/openchair"' $< $(ASAN_LIB_OBJ) -o $@

$(ASAN)/test_multiproc $(ASAN)/test_multiproc_stress $(ASAN)/test_multiproc_capture: $(ASAN)/test_%: $(TEST)/test_%.c $(TEST)/mp_harness.h $(ASAN)/openchair $(ASAN_LIB_OBJ) | $(ASAN)
	$(CC) $(CFLAGS_ASAN) $(LDFLAGS_ASAN) -DOPENCHAIR_BIN='"./$(ASAN)/openchair"' $< $(ASAN_LIB_OBJ) -o $@

# --- oc_store test binaries (LDWRAP is set per target for fault injection) ---
$(PLAIN)/test_store_faults $(ASAN)/test_store_faults: LDWRAP := $(STORE_WRAP)
$(PLAIN)/test_capture_faults $(ASAN)/test_capture_faults: LDWRAP := $(STORE_WRAP)

$(PLAIN)/test_%: $(TEST)/test_%.c $(PLAIN_LIB_OBJ) | $(PLAIN)
	$(CC) $(CFLAGS_PLAIN) $< $(PLAIN_LIB_OBJ) $(LDWRAP) -o $@

$(ASAN)/test_%: $(TEST)/test_%.c $(ASAN_LIB_OBJ) | $(ASAN)
	$(CC) $(CFLAGS_ASAN) $(LDFLAGS_ASAN) $< $(ASAN_LIB_OBJ) $(LDWRAP) -o $@

$(PLAIN) $(ASAN):
	mkdir -p $@

-include $(wildcard $(PLAIN)/*.d $(ASAN)/*.d)
