CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2
PREFIX  ?= /usr/local
TARGET   = maxpool_meta_server

all: $(TARGET)

$(TARGET): src/maxpool_meta_server.c
	$(CC) $(CFLAGS) $< -o $@

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	install -Dm644 config/servers.conf.example $(DESTDIR)/etc/maxpool/servers.conf.example
	install -Dm644 systemd/maxpool-meta-server.service \
		$(DESTDIR)/etc/systemd/system/maxpool-meta-server.service

clean:
	rm -f $(TARGET)

.PHONY: all install clean
