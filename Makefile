# OmaQuake -- Quake in characters.
#
# The engine is libretro/tyrquake built as a static archive (STATIC_LINKING=1)
# so the result is a single self-contained binary, not a dlopen'd core.

PREFIX      ?= /usr/local
CC          ?= cc
PKG_CONFIG  ?= pkg-config

BUILD       := build
BIN         := $(BUILD)/omaquake

TYRQUAKE    := third_party/tyrquake
TYRQUAKE_A  := $(TYRQUAKE)/tyrquake_libretro_unix.a

SRC         := src/oq_main.c src/oq_present.c src/oq_term.c
CFLAGS      ?= -O2 -g
CFLAGS      += -Wall -Wextra -std=gnu99
CPPFLAGS    += -I$(TYRQUAKE)/libretro-common/include
LDLIBS      += -lm

# Backends are optional: build whichever libraries are present.
HAVE_CHAFA  := $(shell $(PKG_CONFIG) --exists chafa && echo 1)
HAVE_CACA   := $(shell $(PKG_CONFIG) --exists caca && echo 1)

ifeq ($(HAVE_CHAFA),1)
  SRC      += src/oq_chafa.c
  CPPFLAGS += -DOQ_HAVE_CHAFA $(shell $(PKG_CONFIG) --cflags chafa)
  LDLIBS   += $(shell $(PKG_CONFIG) --libs chafa)
endif
ifeq ($(HAVE_CACA),1)
  SRC      += src/oq_caca.c
  CPPFLAGS += -DOQ_HAVE_CACA $(shell $(PKG_CONFIG) --cflags caca)
  LDLIBS   += $(shell $(PKG_CONFIG) --libs caca)
endif

OBJ         := $(SRC:src/%.c=$(BUILD)/%.o)

.PHONY: all clean engine install backends
all: $(BIN)

backends:
	@echo "chafa: $(if $(HAVE_CHAFA),yes,NO - install 'chafa')"
	@echo "caca:  $(if $(HAVE_CACA),yes,NO - install 'libcaca')"

$(BIN): $(OBJ) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(BUILD):
	@mkdir -p $(BUILD)

engine: $(TYRQUAKE_A)

$(TYRQUAKE_A):
	$(MAKE) -C $(TYRQUAKE) STATIC_LINKING=1

clean:
	rm -rf $(BUILD)
	-$(MAKE) -C $(TYRQUAKE) clean

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/omaquake
