# GNU Make fixture (used with the MinGW toolchain on Windows).
#
# Exercises the classic "make -> sh -> compiler" chain on Windows, which is
# what a lot of C/C++ projects still use when they are built from a shell
# rather than from an IDE.

CC      ?= gcc
CXX     ?= g++
CFLAGS  ?= -Wall -O2
CXXFLAGS ?= -Wall -O2
LDFLAGS ?=

TARGET  := demo
SRCS    := main.cpp

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
