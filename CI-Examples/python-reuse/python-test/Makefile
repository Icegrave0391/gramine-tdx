# Copyright (C) 2023 Gramine contributors
# SPDX-License-Identifier: BSD-3-Clause

ARCH_LIBDIR ?= /lib/$(shell $(CC) -dumpmachine)

ifeq ($(DEBUG),1)
GRAMINE_LOG_LEVEL = debug
else
GRAMINE_LOG_LEVEL = error
endif

.PHONY: all
all: python.manifest libsyscall.so

RA_TYPE ?= none
RA_CLIENT_SPID ?=
RA_CLIENT_LINKABLE ?= 0

python.manifest: python.manifest.template
	gramine-manifest \
		-Dlog_level=$(GRAMINE_LOG_LEVEL) \
		-Darch_libdir=$(ARCH_LIBDIR) \
		-Dentrypoint=$(realpath $(shell sh -c "command -v python3")) \
		-Dra_type=$(RA_TYPE) \
		-Dra_client_spid=$(RA_CLIENT_SPID) \
		-Dra_client_linkable=$(RA_CLIENT_LINKABLE) \
		$< >$@

# Make libsyscall.so if it doesn't exist yet.
libsyscall.so: libsyscall.c 
	gcc -O2 -fPIC -shared -o $@ $< -ldl

# Make on Ubuntu <= 20.04 doesn't support "Rules with Grouped Targets" (`&:`),
# see the helloworld example for details on this workaround.
python.manifest.sgx python.sig: sgx_sign
	@:

.INTERMEDIATE: sgx_sign
sgx_sign: python.manifest
	gramine-sgx-sign \
		--manifest $< \
		--output $<.sgx

.PHONY: clean
clean:
	$(RM) *.manifest *.manifest.sgx *.token *.sig OUTPUT* *.PID TEST_STDOUT TEST_STDERR *.so
	$(RM) -r scripts/__pycache__

.PHONY: distclean
distclean: clean
