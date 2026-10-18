# fPKGEx - standalone PS5 payload build.
#
# Requires PS5_PAYLOAD_SDK to point at an extracted ps5-payload-sdk and
# clang-18 / lld-18 on PATH (see .github/workflows/build.yml for CI).

SHELL := bash

ifeq ($(strip $(PS5_PAYLOAD_SDK)),)
$(error PS5_PAYLOAD_SDK is required, e.g. export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk)
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

PYTHON ?= python3
BIN     := fpkgex.elf
BUILD_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)

C_SRCS := src/main.c
C_SRCS += src/httpd.c
C_SRCS += src/api.c
C_SRCS += src/jobs.c
C_SRCS += src/fsops.c
C_SRCS += src/fpkg.c
C_SRCS += src/innerfs.c
C_SRCS += src/inflate.c
C_SRCS += src/jsonutil.c

GEN_C   := gen/assets.c
OBJS    := $(patsubst %.c,build/%.o,$(C_SRCS) $(GEN_C))

# Web UI port. Override with `make FPKEX_PORT=6000`.
FPKGEX_PORT ?= 5911

CFLAGS_COMMON := -Os -Wall -Werror -Isrc
CFLAGS_COMMON += -ffunction-sections -fdata-sections -flto
CFLAGS_COMMON += -DFPKEX_VERSION=\"$(BUILD_VERSION)\"
CFLAGS_COMMON += -DFPKEX_PORT=$(FPKGEX_PORT)
CFLAGS_COMMON += -DNDEBUG

LDFLAGS_COMMON := -Wl,--gc-sections -flto

all: $(BIN)

gen/assets.c: web/index.html web/app.js web/style.css tools/embed_assets.py
	mkdir -p gen
	$(PYTHON) tools/embed_assets.py $@ web

# Object files land in build/<path>/, so each directory must exist first.
build/%.o: %.c Makefile
	mkdir -p $(dir $@)
	"$(CC)" $(CFLAGS_COMMON) -c $< -o $@

$(BIN): $(OBJS)
	"$(CC)" $(CFLAGS_COMMON) $(LDFLAGS_COMMON) -o $@ $(OBJS)
	"$(STRIP)" --strip-all $@

clean:
	rm -rf build gen $(BIN)

.PHONY: all clean
