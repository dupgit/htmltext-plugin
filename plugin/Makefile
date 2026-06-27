# Makefile for htmltext_viewer.so
#
# Prerequisites:
#   claws-mail-dev    (headers under /usr/include/claws-mail)
#   pkg-config, gcc, libgtk-3-dev
#
# Usage:
#   make
#   sudo make install          # → /usr/local/lib/claws-mail/plugins/
#   sudo make install PREFIX=/usr

PREFIX    ?= /usr/local
PLUGINDIR ?= $(PREFIX)/lib/claws-mail/plugins

# --- GTK / GLib ---
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 glib-2.0)
GTK_LIBS   := $(shell pkg-config --libs   gtk+-3.0 glib-2.0)

# --- Claws Mail headers ---
# Try pkg-config first (not always present); fall back to installed path.
CLAWS_CFLAGS := $(shell pkg-config --cflags claws-mail 2>/dev/null)
ifeq ($(CLAWS_CFLAGS),)
  CLAWS_INCLUDEDIR ?= /usr/include/claws-mail
  CLAWS_CFLAGS      = -I$(CLAWS_INCLUDEDIR)
endif

CC      ?= gcc
CFLAGS  := -O2 -Wall -Wextra -fPIC \
           -UHAVE_CONFIG_H \
           $(GTK_CFLAGS) \
           $(CLAWS_CFLAGS) \
           $(CFLAGS_EXTRA)

LDFLAGS := -shared -Wl,-soname,htmltext_viewer.so \
           $(GTK_LIBS)

SRC    := htmltext_viewer.c
OBJ    := $(SRC:.c=.o)
TARGET := htmltext_viewer.so

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	install -D -m 755 $(TARGET) $(DESTDIR)$(PLUGINDIR)/$(TARGET)

clean:
	rm -f $(OBJ) $(TARGET)
