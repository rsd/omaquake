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
               src/oq_input.c src/oq_retro.c

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

OBJ         := $(SRC:src/%.c=$(BUILD)/%.o) $(LRC_OBJ)

# Which backends are available changes CPPFLAGS, and make does not track
# flag changes -- without this stamp a newly installed backend leaves stale
# objects behind and the binary silently lacks it.
FLAGS_STAMP := $(BUILD)/.flags

.PHONY: all clean engine install backends
.DELETE_ON_ERROR:
all: $(BIN)

backends:
	@echo "chafa: $(if $(HAVE_CHAFA),yes,NO - install 'chafa')"
	@echo "caca:  $(if $(HAVE_CACA),yes,NO - install 'libcaca')"

$(BIN): $(OBJ) $(TYRQUAKE_A) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(TYRQUAKE_A) $(LDLIBS)

$(BUILD)/%.o: src/%.c $(FLAGS_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

# libretro-common is third-party: build it quietly rather than drowning our
# own warnings in it.
$(BUILD)/lrc/%.o: $(LRC)/%.c | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -I$(LRC)/include -c -o $@ $<

$(FLAGS_STAMP): | $(BUILD)
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
