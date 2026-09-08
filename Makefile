PREFIX  ?= /usr/local
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra
LDLIBS  ?= -lhidapi-hidraw

# 60-, not 99-: systemd's 73-seat-late.rules is what converts TAG+="uaccess"
# into an ACL, and udev reads rule files in lexical order. A 99- file tags the
# device after that rule has run, so no ACL is ever applied.
UDEV_RULES = 60-glorious.rules
UDEV_RULES_LEGACY = 99-glorious.rules

all: gloriousctl

gloriousctl: gloriousctl.c
	$(CC) $(CFLAGS) $(LDFLAGS) gloriousctl.c $(LDLIBS) -o $@

install: install-bin install-udev install-gui

# Standalone copy of the rules, for packaging or cross-compiling (where running
# the freshly built binary is not an option -- commit the output then).
# install-udev does not use this target; it writes to the destination directly.
$(UDEV_RULES): gloriousctl
	./gloriousctl --udev-rules > $@

install-bin: gloriousctl
	install -Dm755 gloriousctl $(DESTDIR)$(PREFIX)/bin/gloriousctl

# The GUI is a PyGObject/GTK3 script that shells out to the CLI, so it needs no
# build step and no GTK development headers -- only the runtime bindings that
# desktop distributions ship (python3-gi + gir1.2-gtk-3.0 / python3-gobject).
install-gui:
	install -Dm755 gloriousctl-gui.py $(DESTDIR)$(PREFIX)/bin/gloriousctl-gui
	install -Dm644 gloriousctl.desktop $(DESTDIR)$(PREFIX)/share/applications/gloriousctl.desktop

# Writes the rules straight to the destination instead of via the source tree:
# 'sudo make install' would otherwise leave a root-owned 99-glorious.rules in
# the working copy that a later unprivileged 'make clean' cannot remove.
install-udev: gloriousctl
	install -d $(DESTDIR)/etc/udev/rules.d
	./gloriousctl --udev-rules > $(DESTDIR)/etc/udev/rules.d/$(UDEV_RULES)
	chmod 644 $(DESTDIR)/etc/udev/rules.d/$(UDEV_RULES)
	# Older installs shipped a 99- file that tagged too late to work.
	rm -f $(DESTDIR)/etc/udev/rules.d/$(UDEV_RULES_LEGACY)
ifeq ($(DESTDIR),)
	-udevadm control --reload
	-udevadm trigger --subsystem-match=hidraw --action=add
	@echo "Replug the mouse for the new rule to take effect."
endif

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/gloriousctl
	rm -f $(DESTDIR)$(PREFIX)/bin/gloriousctl-gui
	rm -f $(DESTDIR)$(PREFIX)/share/applications/gloriousctl.desktop
	rm -f $(DESTDIR)/etc/udev/rules.d/$(UDEV_RULES)
	rm -f $(DESTDIR)/etc/udev/rules.d/$(UDEV_RULES_LEGACY)

clean:
	rm -f gloriousctl $(UDEV_RULES)

.PHONY: all install install-bin install-udev install-gui uninstall clean
