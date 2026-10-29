# RDNA4Accel - Phase 2 IOAccelerator stub
# Build with MacKernelSDK (same as RDNA4Driver); no KDK is needed to compile.

MKSDK ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../navi48/MacKernelSDK)
ARCH = x86_64

CC = clang
CXX = clang++

CFLAGS = -Wall -O2 \
	-target $(ARCH)-apple-macos11.0 \
	-mkernel -nostdlib -fno-builtin -fno-common -fno-stack-protector \
	-arch $(ARCH) \
	-DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
	-I$(MKSDK)/Headers \
	-Isrc

CXXFLAGS = -Wall -Wextra -std=c++17 -O2 \
	-target $(ARCH)-apple-macos11.0 \
	-mkernel -nostdlib -fno-builtin -fno-exceptions -fno-rtti \
	-fno-common -fno-stack-protector -fcheck-new -fno-c++-static-destructors \
	-arch $(ARCH) \
	-DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
	-I$(MKSDK)/Headers \
	-Isrc

LDFLAGS = -arch $(ARCH) -target $(ARCH)-apple-macos11.0 \
	-isysroot $(shell xcrun --sdk macosx --show-sdk-path) \
	-nostdlib -Xlinker -kext \
	-L$(MKSDK)/Library/$(ARCH) \
	-lkmodc++ -lkmod -lcc_kext \
	-Wl,-no_deduplicate

CXX_SOURCES = src/RDNA4Accel.cpp
C_SOURCES = src/kmod_info.c
OBJECTS = build/RDNA4Accel.o build/kmod_info.o

KEXT_NAME = RDNA4Accel.kext
KEXT_BUNDLE = build/$(KEXT_NAME)
KEXT_BINARY = $(KEXT_BUNDLE)/Contents/MacOS/RDNA4Accel

.PHONY: all clean info test

all: $(KEXT_BUNDLE)

build/RDNA4Accel.o: $(CXX_SOURCES) src/RDNA4Accel.hpp
	@mkdir -p $(dir $@)
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/kmod_info.o: $(C_SOURCES)
	@mkdir -p $(dir $@)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

$(KEXT_BUNDLE): $(OBJECTS) Info.plist
	@mkdir -p $(KEXT_BUNDLE)/Contents/MacOS $(KEXT_BUNDLE)/Contents/Resources
	$(CXX) $(LDFLAGS) $(OBJECTS) -o $(KEXT_BINARY)
	cp Info.plist $(KEXT_BUNDLE)/Contents/Info.plist
	chmod -R 755 $(KEXT_BUNDLE)
	chmod 644 $(KEXT_BUNDLE)/Contents/Info.plist
	@echo "Built: $(KEXT_BUNDLE)"

clean:
	rm -rf build

info:
	@echo "MKSDK: $(MKSDK)"
	@echo "Arch:  $(ARCH)"
	@echo "macOS: $(shell sw_vers -productVersion)"
	@ls -ld $(MKSDK) 2>/dev/null || echo "MKSDK not found"

test: $(KEXT_BUNDLE)
	@echo "Diagnosing $(KEXT_BUNDLE)..."
	@kmutil print-diagnostics -p $(KEXT_BUNDLE) -a x86_64 -Z -z 2>&1 | \
		grep -v "Invalid ownership" || true
