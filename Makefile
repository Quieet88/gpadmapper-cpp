CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -pthread
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
EVDEV_CFLAGS := $(shell pkg-config --cflags libevdev)
EVDEV_LIBS := $(shell pkg-config --libs libevdev)

gpadmapper-cpp: gpadmapper-cpp.cpp
	$(CXX) $(CXXFLAGS) $(EVDEV_CFLAGS) -o $@ $< $(EVDEV_LIBS)

install: gpadmapper-cpp
	install -Dm755 gpadmapper-cpp $(DESTDIR)$(BINDIR)/gpadmapper-cpp

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/gpadmapper-cpp

test: gpadmapper-cpp
	sh tests/run.sh

clean:
	rm -f gpadmapper-cpp

.PHONY: install uninstall test clean
