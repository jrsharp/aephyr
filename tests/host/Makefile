# SPDX-License-Identifier: MIT
#
# Host-side HONR tests. These compile only the pure header (honr.h) and so run
# anywhere with a C compiler, under strict hardening flags. Run: `make` (from
# this directory). For the on-target ztest suite use Twister:
#   west twister -T tests/subsys/honr -p native_sim

CC      ?= cc
INC     := ../../include/zephyr/net
CFLAGS  := -std=c11 -O1 -I$(INC) \
           -Wall -Wextra -Werror \
           -Wconversion -Wsign-conversion -Wshadow -Wundef -Wpointer-arith
SANFLAGS := -fsanitize=undefined,address -g

BINS := honr_sim honr_mesh_sim honr_validity

.PHONY: all run clean
all: run

# Build (strict) and run each test, with sanitizers on.
run:
	@set -e; for t in $(BINS); do \
	  echo "== build+run $$t (strict + UBSan/ASan) =="; \
	  $(CC) $(CFLAGS) $(SANFLAGS) $$t.c -o /tmp/aephyr_$$t; \
	  /tmp/aephyr_$$t; \
	  rm -f /tmp/aephyr_$$t; \
	done
	@echo "== all host tests passed under -Werror + UBSan/ASan =="

clean:
	rm -f /tmp/aephyr_honr_sim /tmp/aephyr_honr_mesh_sim /tmp/aephyr_honr_validity
