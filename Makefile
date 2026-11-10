# hyprpm supplies PKG_CONFIG_PATH for its ABI-matched Hyprland headers.
CXX ?= c++
PKG_CONFIG ?= pkg-config
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags hyprland pixman-1 libdrm libinput libudev wayland-server xkbcommon)
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++23 -fPIC -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-narrowing
LDFLAGS += -shared

all: hypr-edgesnap.so

hypr-edgesnap.so: main.cpp geometry.hpp compat.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $< -o $@

# Checks that need no running compositor.
test: hypr-edgesnap.so tests/geometry
	./tests/geometry
	python3 tests/maintenance.py

tests/geometry: tests/geometry.test.cpp geometry.hpp
	$(CXX) -std=c++23 -O1 -Wall -Wextra -I. $< -o $@

# Nested-compositor harness helper; needs a compositor to run, so it is not
# part of `all` or `test`.
NESTED_PROTO = tests/nested/wlr-virtual-pointer-unstable-v1.xml

tests/nested/vptr: tests/nested/vptr.c $(NESTED_PROTO)
	wayland-scanner client-header $(NESTED_PROTO) tests/nested/wlr-virtual-pointer-unstable-v1-client-protocol.h
	wayland-scanner private-code $(NESTED_PROTO) tests/nested/wlr-virtual-pointer-unstable-v1-protocol.c
	$(CC) -std=c11 -O1 -Wall -Wextra -Itests/nested $< tests/nested/wlr-virtual-pointer-unstable-v1-protocol.c $(shell $(PKG_CONFIG) --cflags --libs wayland-client) -o $@

# Drag and keyboard behaviour in a throwaway nested Hyprland. Needs a running
# Hyprland session; see README. Not part of `test` (CI has no compositor).
nested-test: hypr-edgesnap.so tests/nested/vptr
	python3 tests/nested/drag.py

clean:
	rm -f hypr-edgesnap.so tests/geometry tests/nested/vptr tests/nested/*-protocol.[ch]

.PHONY: all test nested-test clean
