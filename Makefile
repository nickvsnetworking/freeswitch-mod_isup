# mod_isup — out-of-tree FreeSWITCH ISUP/M3UA MGCF module
#
# This top-level Makefile builds and runs the dependency-free protocol
# unit tests. The FreeSWITCH/Osmocom-linked module proper is built by the
# autotools setup (added in a later phase); the codec layer here is fully
# testable on its own.

CC      ?= cc
CFLAGS  ?= -O2 -g -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter
SANFLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer

CODEC_SRCS = isup_codec.c isup_param.c
SM_SRCS    = isup_sm.c isup_codec.c isup_param.c
SEG_SRCS   = isup_segment.c isup_codec.c isup_param.c
MAP_SRCS   = isup_map.c isup_param.c
E2E_SRCS   = isup_sm.c isup_codec.c isup_param.c
CGM_SRCS   = isup_cgm.c isup_codec.c isup_param.c
TRACE_SRCS = isup_trace.c isup_codec.c isup_param.c isup_map.c
ALL_SRCS   = isup_codec.c isup_param.c isup_sm.c isup_cgm.c isup_segment.c isup_map.c isup_trace.c

.PHONY: all check test fuzz clean

all: check

# Unit tests, built with ASan + UBSan so the fuzz/truncation sweeps catch
# any out-of-bounds access.
check: test/test_codec test/test_sm test/test_segment test/test_map test/test_e2e test/test_cgm test/test_hardening test/test_trace test/test_conformance
	./test/test_codec
	./test/test_sm
	./test/test_segment
	./test/test_map
	./test/test_e2e
	./test/test_cgm
	./test/test_hardening
	./test/test_trace
	./test/test_conformance

test/test_codec: test/test_codec.c $(CODEC_SRCS) isup_codec.h isup_param.h isup_proto.h
	$(CC) $(CFLAGS) $(SANFLAGS) -o $@ test/test_codec.c $(CODEC_SRCS)

test/test_sm: test/test_sm.c $(SM_SRCS) isup_sm.h isup_codec.h isup_param.h isup_proto.h
	$(CC) $(CFLAGS) $(SANFLAGS) -o $@ test/test_sm.c $(SM_SRCS)

test/test_segment: test/test_segment.c $(SEG_SRCS) isup_segment.h isup_codec.h isup_param.h isup_proto.h
	$(CC) $(CFLAGS) $(SANFLAGS) -o $@ test/test_segment.c $(SEG_SRCS)

test/test_map: test/test_map.c $(MAP_SRCS) isup_map.h isup_param.h isup_proto.h
	$(CC) $(CFLAGS) $(SANFLAGS) -o $@ test/test_map.c $(MAP_SRCS)

test/test_e2e: test/test_e2e.c $(E2E_SRCS) isup_sm.h isup_codec.h isup_param.h isup_proto.h
	$(CC) $(CFLAGS) $(SANFLAGS) -o $@ test/test_e2e.c $(E2E_SRCS)

test/test_cgm: test/test_cgm.c $(CGM_SRCS) isup_cgm.h isup_codec.h isup_param.h isup_proto.h
	$(CC) $(CFLAGS) $(SANFLAGS) -o $@ test/test_cgm.c $(CGM_SRCS)

test/test_hardening: test/test_hardening.c $(CGM_SRCS) isup_cgm.h isup_codec.h isup_param.h isup_proto.h
	$(CC) $(CFLAGS) $(SANFLAGS) -o $@ test/test_hardening.c $(CGM_SRCS)

test/test_trace: test/test_trace.c $(TRACE_SRCS) isup_trace.h isup_codec.h isup_param.h isup_map.h isup_proto.h
	$(CC) $(CFLAGS) $(SANFLAGS) -o $@ test/test_trace.c $(TRACE_SRCS)

test/test_conformance: test/test_conformance.c isup_sm.c isup_cgm.c isup_codec.c isup_param.c isup_sm.h isup_cgm.h isup_codec.h isup_param.h isup_proto.h
	$(CC) $(CFLAGS) $(SANFLAGS) -o $@ test/test_conformance.c isup_sm.c isup_cgm.c isup_codec.c isup_param.c

# Plain (no-sanitizer) build, for environments without ASan.
test-plain: test/test_codec.c $(CODEC_SRCS)
	$(CC) $(CFLAGS) -o test/test_codec_plain test/test_codec.c $(CODEC_SRCS)
	./test/test_codec_plain

# Compile every module translation unit (protocol core + osmo M3UA binding +
# FreeSWITCH endpoint) to prove the whole module builds against the real
# FreeSWITCH and libosmo-sigtran headers. The final .so link additionally
# needs libfreeswitch (a built FreeSWITCH tree) and is done by the install.
FS_INC   ?= ../src/include
TT_INC   ?= ../libs/libteletone/src
OSMO_CF  := $(shell pkg-config --cflags libosmo-sigtran 2>/dev/null)
MOD_SRCS  = isup_codec.c isup_param.c isup_sm.c isup_cgm.c isup_segment.c \
            isup_map.c isup_trace.c isup_m3ua.c mod_isup.c
objects:
	@mkdir -p build
	@for f in $(MOD_SRCS); do \
	   echo "  CC $$f"; \
	   cc -c -O2 -std=gnu11 -Wall -I. -I$(FS_INC) -I$(TT_INC) $(OSMO_CF) \
	      $$f -o build/$${f%.c}.o || exit 1; \
	 done
	@echo "OK: all $(words $(MOD_SRCS)) module objects compiled"

# Coverage-guided fuzzing of the entire hostile-input path (clang only).
# Runs bounded so it can be part of CI; raise -max_total_time for soak.
FUZZ_TIME ?= 25
fuzz: test/isup_fuzz
	mkdir -p test/corpus
	./test/isup_fuzz -max_total_time=$(FUZZ_TIME) -print_final_stats=1 test/corpus

test/isup_fuzz: test/isup_fuzz.c $(ALL_SRCS)
	clang $(CFLAGS) -fsanitize=fuzzer,address,undefined -o $@ test/isup_fuzz.c $(ALL_SRCS)

clean:
	rm -f test/test_codec test/test_codec_plain test/test_sm test/test_segment \
	      test/test_map test/test_e2e test/test_cgm test/isup_fuzz test/test_hardening

# ------------------------------------------------------------------
# Build the loadable module against an INSTALLED FreeSWITCH
# (pkg-config freeswitch) + libosmo-sigtran. Drop mod_isup.so into the
# FreeSWITCH module dir and `load mod_isup`.
# ------------------------------------------------------------------
FS_CFLAGS   ?= $(shell pkg-config --cflags freeswitch 2>/dev/null)
FS_LIBS     ?= $(shell pkg-config --libs freeswitch 2>/dev/null)
OSMO_CFLAGS ?= $(shell pkg-config --cflags libosmo-sigtran 2>/dev/null)
OSMO_LIBS   ?= $(shell pkg-config --libs libosmo-sigtran 2>/dev/null) -losmovty -losmocore -ltalloc
MODULE_SRCS  = isup_codec.c isup_param.c isup_sm.c isup_cgm.c isup_segment.c \
               isup_map.c isup_trace.c isup_m3ua.c bearer_mgcp.c bearer_megaco.c mod_isup.c

.PHONY: module
module: mod_isup.so
mod_isup.so: $(MODULE_SRCS)
	$(CC) -shared -fPIC -O2 -std=gnu11 -Wall -Wno-unused-parameter \
	    $(FS_CFLAGS) $(OSMO_CFLAGS) -o $@ $(MODULE_SRCS) $(FS_LIBS) $(OSMO_LIBS)
