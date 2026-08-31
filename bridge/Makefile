# Prism - native Linux capture bridge (PrismCapture.dll)
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This builds a Winelib module: an ELF shared object carrying a PE-style export
# table. Wine's loader recognises it when Prism.exe calls LoadLibraryW(), which
# is what lets a Windows process call libpipewire and GIO directly.
#
# Requires: winegcc/winebuild (wine-devel), libpipewire-0.3-dev, libglib2.0-dev.

PKGS      := libpipewire-0.3 gio-unix-2.0
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS   := $(shell pkg-config --libs $(PKGS))

WINEGCC ?= winegcc
BUILDDIR ?= ../build/bridge
OUT      := $(BUILDDIR)/PrismCapture.dll

SOURCES := prism_bridge.c screencast.c pipewire.c portal.c
HEADERS := prism_log.h screencast.h pipewire.h portal.h ../include/PrismCapture.h

CFLAGS_COMMON := -m64 -fPIC -Wall -Wextra -Wno-unused-parameter -Wno-attributes \
                 -I. -I../include $(PKG_CFLAGS)
CFLAGS_REL    := -O3 -DNDEBUG
CFLAGS_DBG    := -O0 -g -DPRISM_DEBUG

.PHONY: all debug clean

all: CFLAGS_EXTRA := $(CFLAGS_REL)
all: $(OUT)

debug: CFLAGS_EXTRA := $(CFLAGS_DBG)
debug: clean $(OUT)

$(OUT): $(SOURCES) $(HEADERS)
	@mkdir -p $(BUILDDIR)
	$(WINEGCC) -shared PrismCapture.spec $(SOURCES) \
		$(CFLAGS_COMMON) $(CFLAGS_EXTRA) \
		$(PKG_LIBS) -lm -lpthread \
		-o $(BUILDDIR)/PrismCapture
	@# winegcc emits <name>.dll.so; Prism.exe loads it under its PE name.
	@mv -f $(BUILDDIR)/PrismCapture.dll.so $(OUT)
	@echo "built $(OUT)"

clean:
	rm -f $(BUILDDIR)/PrismCapture.dll $(BUILDDIR)/PrismCapture.dll.so
