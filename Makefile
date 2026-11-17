CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11
DEBUG_CFLAGS = -g -O0 -Wall -Wextra -DDEBUG -std=gnu11
LDFLAGS = -lcjson -lpthread
TARGET = mcp-service-manager
SRC = server.c

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
SYSTEMD_DIR ?= /etc/systemd/system

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) $(LDFLAGS) -o $(TARGET)

debug:
	$(CC) $(DEBUG_CFLAGS) $(SRC) $(LDFLAGS) -o $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	@if [ -f mcp-service-manager.service ]; then \
		install -d $(DESTDIR)$(SYSTEMD_DIR); \
		install -m 644 mcp-service-manager.service $(DESTDIR)$(SYSTEMD_DIR)/; \
		echo "Installed systemd service unit to $(SYSTEMD_DIR)/mcp-service-manager.service"; \
	fi
	@echo "Installation complete: $(BINDIR)/$(TARGET)"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(SYSTEMD_DIR)/mcp-service-manager.service
	@echo "Uninstalled $(TARGET)"

clean:
	rm -f $(TARGET) *.o

help:
	@echo "MCP Linux Service Manager - Makefile Commands:"
	@echo "  make          - Biên dịch bản release tối ưu (-O2)"
	@echo "  make debug    - Biên dịch bản debug với gdb symbols (-g -O0)"
	@echo "  make install  - Cài đặt binary vào $(BINDIR)"
	@echo "  make uninstall- Gỡ bỏ binary khỏi $(BINDIR)"
	@echo "  make clean    - Xóa các file build cũ"

.PHONY: all debug install uninstall clean help
