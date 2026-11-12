# Native DE10-Nano build: user applications plus external kernel modules.
# The kernel build system enters this branch when compiling external modules.
ifneq ($(KERNELRELEASE),)
obj-m := video.o key_sw.o
else
# Override KDIR only with the configured tree matching the running kernel.
KDIR ?= /lib/modules/$(shell uname -r)/build
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu99
PROGRAMS := part1 part2 part3 part4 part5 part6
.PHONY: all user module clean
all: user module
# User programs can also be built on a Linux development host with make user.
user: $(PROGRAMS)
$(PROGRAMS): %: %.c
	$(CC) $(CFLAGS) -o $@ $< -lrt
# Delegate architecture and module ABI rules to the target kernel build tree.
module:
	@test -d "$(KDIR)" || { echo "Missing kernel build tree: $(KDIR). Set KDIR to the configured source tree matching uname -r."; exit 1; }
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" modules
clean:
	$(RM) $(PROGRAMS)
	@if test -d "$(KDIR)"; then $(MAKE) -C "$(KDIR)" M="$(CURDIR)" clean; fi
endif
