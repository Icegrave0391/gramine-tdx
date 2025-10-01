# Copyright (C) 2023 Gramine contributors
# SPDX-License-Identifier: BSD-3-Clause

CFLAGS = -Wall -Wextra


GRAMINE_LOG_LEVEL = debug
CFLAGS += -g


.PHONY: all
all: reuse reuse.manifest

reuse: reuse.o

reuse.manifest: reuse.manifest.template
	gramine-manifest \
		-Dlog_level=$(GRAMINE_LOG_LEVEL) \
		$< $@
	gramine-manifest-check $@

