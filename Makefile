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

SRC         := src/oq_main.c src/oq_present.c src/oq_term.c \
               src/oq_input.c src/oq_mouse.c src/oq_retro.c \
               src/oq_render.c src/oq_audio.c

# With STATIC_LINKING=1 the core deliberately drops libretro-common (see
# tyrquake/Makefile.common:119) and expects the frontend to provide it.
LRC         := $(TYRQUAKE)/libretro-common
LRC_SRC     := \
  $(LRC)/file/retro_dirent.c $(LRC)/encodings/encoding_utf.c \
  $(LRC)/string/stdstring.c $(LRC)/streams/file_stream.c \
  $(LRC)/streams/file_stream_transforms.c $(LRC)/vfs/vfs_implementation.c \
  $(LRC)/file/file_path.c $(LRC)/file/file_path_io.c \
  $(LRC)/features/features_cpu.c $(LRC)/compat/fopen_utf8.c \
  $(LRC)/compat/compat_strl.c $(LRC)/compat/compat_posix_string.c \
  $(LRC)/compat/compat_strcasestr.c $(LRC)/compat/compat_snprintf.c \
  $(LRC)/time/rtime.c \
  $(LRC)/net/net_compat.c $(LRC)/net/net_socket.c
LRC_OBJ     := $(LRC_SRC:$(LRC)/%.c=$(BUILD)/lrc/%.o)
CFLAGS      ?= -O2 -g
CFLAGS      += -Wall -Wextra -std=gnu99
CPPFLAGS    += -I$(TYRQUAKE)/libretro-common/include
LDLIBS      += -lm -pthread
CFLAGS      += -pthread

# Backends are optional: build whichever libraries are present.
HAVE_CHAFA  := $(shell $(PKG_CONFIG) --exists chafa && echo 1)
HAVE_CACA   := $(shell $(PKG_CONFIG) --exists caca && echo 1)
HAVE_ALSA   := $(shell $(PKG_CONFIG) --exists alsa && echo 1)

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
# Audio is one sink rather than a choice of backends, so src/oq_audio.c is
# always compiled and stubs itself out; only the flags are conditional.
ifeq ($(HAVE_ALSA),1)
  CPPFLAGS += -DOQ_HAVE_ALSA $(shell $(PKG_CONFIG) --cflags alsa)
  LDLIBS   += $(shell $(PKG_CONFIG) --libs alsa)
endif

OBJ         := $(SRC:src/%.c=$(BUILD)/%.o) $(LRC_OBJ)

# Which backends are available changes CPPFLAGS, and make does not track
# flag changes -- without this stamp a newly installed backend leaves stale
# objects behind and the binary silently lacks it.
FLAGS_STAMP := $(BUILD)/.flags

.DEFAULT_GOAL := all
.PHONY: all clean engine install backends FORCE
.DELETE_ON_ERROR:
all: $(BIN)

backends:
	@echo "chafa: $(if $(HAVE_CHAFA),yes,NO - install 'chafa')"
	@echo "caca:  $(if $(HAVE_CACA),yes,NO - install 'libcaca')"
	@echo "alsa:  $(if $(HAVE_ALSA),yes,NO - install 'alsa-lib'; sound off)"

$(BIN): $(OBJ) $(TYRQUAKE_A) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(TYRQUAKE_A) $(LDLIBS)

$(BUILD)/%.o: src/%.c $(FLAGS_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

# libretro-common is third-party: build it quietly rather than drowning our
# own warnings in it.
$(BUILD)/lrc/%.o: $(LRC)/%.c | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -I$(LRC)/include -c -o $@ $<

$(FLAGS_STAMP): FORCE | $(BUILD)
	@echo '$(CC) $(CFLAGS) $(CPPFLAGS) $(LDLIBS)' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

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

# An always-out-of-date prerequisite. FLAGS_STAMP needs this because an
# order-only prerequisite alone never re-runs the recipe: once build/.flags
# exists make considers it final, the stamp goes stale, and the flag-change
# detection it exists for stops working. Keep this rule LAST -- defined before
# `all` it becomes the default goal and `make` silently builds nothing.
FORCE:
