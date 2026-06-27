# Makefile for htmltext_viewer.so
#
# Prerequisites on the build machine:
#   - claws-mail development headers  (claws-mail-dev or built from source)
#   - pkg-config, gcc
#
# Typical usage:
#   make
#   make install          (copies .so to the Claws plugin directory)
#   make install PREFIX=/usr

PREFIX     ?= /usr/local
PLUGINDIR  ?= $(PREFIX)/lib/claws-mail/plugins

# --- pkg-config probes ---
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS   := $(shell pkg-config --libs   gtk+-3.0)
GLIB_CFLAGS:= $(shell pkg-config --cflags glib-2.0)
GLIB_LIBS  := $(shell pkg-config --libs   glib-2.0)

# Claws Mail headers: adjust CLAWS_SRCDIR if building against source tree,
# or rely on the installed headers (e.g. /usr/include/claws-mail).
CLAWS_INCLUDEDIR ?= $(shell pkg-config --variable=includedir claws-mail 2>/dev/null || echo /usr/include/claws-mail)

CFLAGS := -O2 -Wall -Wextra \
          -fPIC \
          -DHAVE_CONFIG_H \
          $(GTK_CFLAGS) $(GLIB_CFLAGS) \
          -I$(CLAWS_INCLUDEDIR)

# Optional: hard-code the path to the Rust companion binary at compile time.
# If unset, the plugin searches PATH at runtime.
# CFLAGS += -DHTMLTEXT_RENDER_BIN=\"/usr/lib/claws-mail/htmltext-render\"

LDFLAGS := -shared -Wl,-soname,htmltext_viewer.so \
           $(GTK_LIBS) $(GLIB_LIBS)

SRC := htmltext_viewer.c
OBJ := $(SRC:.c=.o)
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
