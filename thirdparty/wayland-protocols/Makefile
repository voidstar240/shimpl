.PHONY = all clean

PKG_CONFIG ?= pkg-config

WL_PROTOCOLS_DIR := $(shell $(PKG_CONFIG) wayland-protocols --variable=pkgdatadir)
WL_SCANNER := $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)

all: xdg-shell.h xdg-decoration.h cursor-shape.h pointer-constraints.h

xdg-shell.h: $(wildcard $(WL_PROTOCOLS_DIR)/*/*/xdg-shell.xml)
xdg-decoration.h: $(wildcard $(WL_PROTOCOLS_DIR)/*/*/xdg-decoration-unstable-v1.xml)
cursor-shape.h: $(wildcard $(WL_PROTOCOLS_DIR)/*/*/cursor-shape-v1.xml)
pointer-constraints.h: $(wildcard $(WL_PROTOCOLS_DIR)/*/*/pointer-constraints-unstable-v1.xml)
%.h:
	@mkdir -p $(@D)
	$(WL_SCANNER) client-header < $< > $@
	@printf "\n\n#ifdef WAYLAND_PROTOCOLS_IMPLEMENTATION\n\n" >> $@
	$(WL_SCANNER) private-code < $< >> $@
	@printf "#endif" >> $@

clean:
	rm -f *.h
