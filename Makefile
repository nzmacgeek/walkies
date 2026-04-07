# walkies — network configuration daemon for BlueyOS
# "Let's go for a walkies!" — Bluey
#
# Targets:
#   make              - static i386 ELF build against musl (default)
#   make static       - same as above, explicit
#   make dynamic      - dynamically linked i386 ELF build against musl
#   make musl         - clone nzmacgeek/musl-blueyos and build for i386 into $(MUSL_PREFIX)
#   make package      - build walkies-<version>-i386.dpk for dimsim
#   make clean        - remove build artefacts
#
# Variables (override on command line):
#   MUSL_PREFIX       - path to an installed musl-blueyos sysroot
#                       Defaults to /opt/blueyos-sysroot when that directory
#                       exists, otherwise falls back to build/musl.
#   BUILD_DIR         - output directory (default: build)
#   DEBUG=1           - enable debug flags (-g -O0 -DDEBUG)
#
# Quick start on a BlueyOS build host (sysroot at /opt/blueyos-sysroot):
#   make
#
# Quick start on a fresh host:
#   make musl                         # clones musl-blueyos and builds into build/musl/
#   make                              # builds walkies (static i386 ELF)
#
# Or with a custom musl sysroot:
#   make MUSL_PREFIX=/path/to/sysroot
#
# ⚠️  VIBE CODED RESEARCH PROJECT — NOT FOR PRODUCTION USE ⚠️

# ---------------------------------------------------------------------------
# Directories and tool paths
# ---------------------------------------------------------------------------
BUILD_DIR ?= build

BLUEYOS_SYSROOT ?= /opt/blueyos-sysroot
ifeq ($(shell [ -d $(BLUEYOS_SYSROOT) ] && echo yes),yes)
  MUSL_PREFIX ?= $(BLUEYOS_SYSROOT)
else
  MUSL_PREFIX ?= $(BUILD_DIR)/musl
endif

MUSL_INCLUDE := $(MUSL_PREFIX)/include
MUSL_LIB     := $(MUSL_PREFIX)/lib

# ---------------------------------------------------------------------------
# Source / output
# ---------------------------------------------------------------------------
SRC_DIR := user
SRC     := $(SRC_DIR)/walkies.c
TARGET  := $(BUILD_DIR)/walkies

# ---------------------------------------------------------------------------
# Toolchain
# ---------------------------------------------------------------------------
CC := gcc

# ---------------------------------------------------------------------------
# Base compiler flags — i386 ELF, strict warnings, no stack protector
# ---------------------------------------------------------------------------
BASE_CFLAGS := \
    -m32 \
    -std=gnu11 \
    -Wall \
    -Wextra \
    -Wno-unused-parameter \
    -fno-stack-protector \
    -I$(SRC_DIR)

ifeq ($(DEBUG),1)
  BASE_CFLAGS += -g -O0 -DDEBUG
else
  BASE_CFLAGS += -O2
endif

# ---------------------------------------------------------------------------
# Linker flags common to both static and dynamic builds
# ---------------------------------------------------------------------------
BASE_LDFLAGS := \
    -Wl,-m,elf_i386 \
    -Wl,-Ttext,0x00400000

# ---------------------------------------------------------------------------
# Static build flags (default)
# ---------------------------------------------------------------------------
STATIC_CFLAGS  := $(BASE_CFLAGS) -fno-pic -isystem $(MUSL_INCLUDE)
STATIC_LDFLAGS := $(BASE_LDFLAGS) -static -no-pie -L$(MUSL_LIB)

# ---------------------------------------------------------------------------
# Dynamic build flags
# ---------------------------------------------------------------------------
DYNAMIC_CFLAGS  := $(BASE_CFLAGS) -fPIC -isystem $(MUSL_INCLUDE)
DYNAMIC_LDFLAGS := $(BASE_LDFLAGS) -no-pie -L$(MUSL_LIB)

# ---------------------------------------------------------------------------
# Phony targets
# ---------------------------------------------------------------------------
.PHONY: all static dynamic musl musl-check package clean help

.DEFAULT_GOAL := all

# ---------------------------------------------------------------------------
# Helper: verify musl is present before trying to build
# ---------------------------------------------------------------------------
define check_musl
	@if [ ! -d "$(MUSL_INCLUDE)" ] || [ ! -f "$(MUSL_LIB)/libc.a" ]; then \
		echo ""; \
		echo "  [MUSL] musl sysroot not found under $(MUSL_PREFIX)"; \
		echo "         expected:"; \
		echo "           $(MUSL_INCLUDE)/  (headers)"; \
		echo "           $(MUSL_LIB)/libc.a  (static library)"; \
		echo ""; \
		echo "  To build musl for BlueyOS:"; \
		echo "    ./tools/build-musl.sh --prefix=$(MUSL_PREFIX)"; \
		echo "  Or point at an existing sysroot:"; \
		echo "    make MUSL_PREFIX=/path/to/musl-sysroot"; \
		echo ""; \
		exit 1; \
	fi
endef

# ---------------------------------------------------------------------------
# all / static — static i386 ELF linked against musl
# ---------------------------------------------------------------------------
all: static

static: $(BUILD_DIR) musl-check
	$(CC) $(STATIC_CFLAGS) $(SRC) $(STATIC_LDFLAGS) -lc -o $(TARGET)
	@echo ""
	@echo "  [LD]  $(TARGET) (i386 ELF, static musl)"
	@echo ""

# ---------------------------------------------------------------------------
# dynamic — dynamically linked i386 ELF against musl
# ---------------------------------------------------------------------------
dynamic: $(BUILD_DIR) musl-check
	$(CC) $(DYNAMIC_CFLAGS) $(SRC) $(DYNAMIC_LDFLAGS) -lc -o $(TARGET)-dynamic
	@echo ""
	@echo "  [LD]  $(TARGET)-dynamic (i386 ELF, dynamic musl)"
	@echo ""

# ---------------------------------------------------------------------------
# musl — clone nzmacgeek/musl-blueyos and build for i386
# ---------------------------------------------------------------------------
musl:
	@bash tools/build-musl.sh --prefix=$(MUSL_PREFIX)

# ---------------------------------------------------------------------------
# musl-check — internal target that runs the check macro
# ---------------------------------------------------------------------------
.PHONY: musl-check
musl-check:
	$(call check_musl)

# ---------------------------------------------------------------------------
# Build output directory
# ---------------------------------------------------------------------------
$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# ---------------------------------------------------------------------------
# package — build the walkies .dpk for installation into a BlueyOS sysroot
# ---------------------------------------------------------------------------
PKG_DIR := pkg
PKG_BIN := $(PKG_DIR)/payload/sbin/walkies

package: static
	@command -v dpkbuild >/dev/null 2>&1 || { \
		echo ""; \
		echo "  [PKG]  dpkbuild not found — install the dimsim tools first."; \
		echo "         See: https://github.com/nzmacgeek/dimsim"; \
		echo ""; \
		exit 1; \
	}
	@mkdir -p $(PKG_DIR)/payload/sbin
	@cp $(TARGET) $(PKG_BIN)
	@chmod 0755 $(PKG_BIN)
	dpkbuild build $(PKG_DIR)/
	@echo ""
	@echo "  [PKG]  walkies package built."
	@echo ""

# ---------------------------------------------------------------------------
# clean
# ---------------------------------------------------------------------------
clean:
	@if [ -z "$(BUILD_DIR)" ] || [ "$(BUILD_DIR)" = "/" ] || [ "$(BUILD_DIR)" = "." ]; then \
		echo "  [CLEAN] Refusing to remove unsafe BUILD_DIR='$(BUILD_DIR)'"; exit 1; \
	fi
	rm -rf -- "$(BUILD_DIR)"
	rm -f -- "$(PKG_BIN)" walkies-*.dpk
	@echo "  [CLEAN] Build artefacts removed from $(BUILD_DIR)."

# ---------------------------------------------------------------------------
# help
# ---------------------------------------------------------------------------
help:
	@echo "walkies — network configuration daemon for BlueyOS"
	@echo ""
	@echo "  make              build static i386 ELF (default)"
	@echo "  make musl         clone musl-blueyos and build for i386 (into MUSL_PREFIX)"
	@echo "  make static       same as above, explicit"
	@echo "  make dynamic      build dynamically linked i386 ELF"
	@echo "  make package      build walkies-<version>-i386.dpk for dimsim (requires dpkbuild)"
	@echo "  make clean        remove build artefacts"
	@echo ""
	@echo "Variables:"
	@echo "  MUSL_PREFIX=...   path to musl sysroot (default: $(BUILD_DIR)/musl)"
	@echo "  BUILD_DIR=...     output directory      (default: build)"
	@echo "  DEBUG=1           enable debug build"
	@echo ""
	@echo "Example:"
	@echo "  make MUSL_PREFIX=/opt/blueyos-sysroot"
