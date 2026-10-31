# RDNA4 Driver Makefile
# Phase 1: Basic driver build

# Kernel SDK path (relative to this Makefile, not the caller's cwd).
# Build it eagerly: the -include dependency files below append to
# MAKEFILE_LIST, so a lazy $(lastword ...) would resolve against a .d file.
MKSDK := $(abspath $(dir $(firstword $(MAKEFILE_LIST)))/../navi48/MacKernelSDK)

# Project info
KEXT_NAME = RDNA4Driver
BUNDLE_ID = com.rdna4.driver
VERSION = 0.2.0

# Directories
SRC_DIR = src
BUILD_DIR = build
KEXT_DIR = $(BUILD_DIR)/$(KEXT_NAME).kext
CONTENTS = $(KEXT_DIR)/Contents
MACOS = $(CONTENTS)/MacOS
RESOURCES = $(CONTENTS)/Resources

# Source files
CPP_SOURCES = $(SRC_DIR)/core/RDNA4Driver.cpp \
	$(SRC_DIR)/core/ComputeSubmission.cpp \
	$(SRC_DIR)/core/RDNA4DriverUserClient.cpp \
	$(SRC_DIR)/hardware/IPDiscovery.cpp \
		$(SRC_DIR)/hardware/PSPLoader.cpp \
		$(SRC_DIR)/hardware/SMUMailbox.cpp \
		$(SRC_DIR)/hardware/GFXFirmware.cpp \
		$(SRC_DIR)/hardware/GMCInit.cpp \
		$(SRC_DIR)/hardware/GARTUtils.cpp \
		$(SRC_DIR)/hardware/NBIODoorbell.cpp \
		$(SRC_DIR)/hardware/ComputeRing.cpp \
		$(SRC_DIR)/hardware/ComputeQueue.cpp \
		$(SRC_DIR)/hardware/MES.cpp \
		$(SRC_DIR)/hardware/PM4BuilderKernel.cpp \
		$(SRC_DIR)/hardware/GraphicsPM4Builder.cpp \
		$(SRC_DIR)/hardware/GraphicsRing.cpp \
		$(SRC_DIR)/hardware/GraphicsQueue.cpp
C_SOURCES = $(SRC_DIR)/kmod_info.c

# Object files
CPP_OBJECTS = $(CPP_SOURCES:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o)
C_OBJECTS = $(C_SOURCES:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
OBJECTS = $(CPP_OBJECTS) $(C_OBJECTS)

# Target architecture
ARCH = x86_64

# Compiler and flags
CC = clang
CXX = clang++
CFLAGS = -Wall -O2 -MMD -MP \
	-target $(ARCH)-apple-macos11.0 \
	-mkernel -nostdlib -fno-builtin -fno-common -fno-stack-protector \
	-arch $(ARCH) \
	-DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
	-I$(MKSDK)/Headers \
	-I$(SRC_DIR)

CXXFLAGS = -Wall -Wextra -std=c++17 -O2 -MMD -MP \
	-target $(ARCH)-apple-macos11.0 \
	-mkernel -nostdlib -fno-builtin -fno-exceptions -fno-rtti \
	-fno-common -fno-stack-protector -fcheck-new -fno-c++-static-destructors \
	-arch $(ARCH) \
	-DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
	-I$(MKSDK)/Headers \
	-I$(SRC_DIR)

LDFLAGS = -arch $(ARCH) -target $(ARCH)-apple-macos11.0 \
	-isysroot $(shell xcrun --sdk macosx --show-sdk-path) \
	-nostdlib -Xlinker -kext \
	-L$(MKSDK)/Library/$(ARCH) \
	-lkmodc++ -lkmod -lcc_kext \
	-Wl,-no_deduplicate

# Targets
.PHONY: all clean install unload status test-pm4 test-user-client

all: $(KEXT_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

$(KEXT_DIR): $(OBJECTS) Info.plist
	@echo "Building kext..."
	@mkdir -p $(MACOS) $(RESOURCES)

	# Link binary
	$(CXX) $(LDFLAGS) $(OBJECTS) -o $(MACOS)/$(KEXT_NAME)

	# Copy Info.plist
	cp Info.plist $(CONTENTS)/

	# Set permissions
	chmod -R 755 $(KEXT_DIR)
	chmod 644 $(CONTENTS)/Info.plist

	@echo "✓ Kext built successfully: $(KEXT_DIR)"

# Header dependencies: any header edit rebuilds the objects that include it,
# so a class layout change can never leave stale objects behind.
-include $(OBJECTS:.o=.d)

clean:
	@echo "Cleaning build files..."
	rm -rf $(BUILD_DIR)

# Host-side checks: PM4 encoder bytes, MES API frame layout, GFX ring
# doorbell. No hardware, no IOKit (tests/host-shim stands in for IOLib).
test-pm4: test-user-client
	@mkdir -p $(BUILD_DIR)/tests
	$(CXX) -std=c++17 -Wall -Wextra -I$(SRC_DIR)/hardware \
		$(SRC_DIR)/hardware/GraphicsPM4Builder.cpp tests/test_graphics_pm4.cpp \
		-o $(BUILD_DIR)/tests/test_graphics_pm4
	./$(BUILD_DIR)/tests/test_graphics_pm4
	$(CXX) -std=c++17 -Wall -Wextra -I$(SRC_DIR)/hardware \
		tests/test_mes_api.cpp -o $(BUILD_DIR)/tests/test_mes_api
	./$(BUILD_DIR)/tests/test_mes_api
	$(CXX) -std=c++17 -Wall -Wextra -Itests/host-shim -I$(SRC_DIR)/hardware \
		$(SRC_DIR)/hardware/GraphicsRing.cpp tests/test_graphics_ring.cpp \
		-o $(BUILD_DIR)/tests/test_graphics_ring
	./$(BUILD_DIR)/tests/test_graphics_ring
	$(CXX) -std=c++17 -Wall -Wextra -Itests/host-shim -I$(SRC_DIR)/hardware \
		$(SRC_DIR)/hardware/ComputeRing.cpp tests/test_compute_ring.cpp \
		-o $(BUILD_DIR)/tests/test_compute_ring
	./$(BUILD_DIR)/tests/test_compute_ring
	$(CXX) -std=c++17 -Wall -Wextra -Itests/host-shim -I$(SRC_DIR)/hardware \
		$(SRC_DIR)/hardware/GARTUtils.cpp tests/test_gart_utils.cpp \
		-o $(BUILD_DIR)/tests/test_gart_utils
	./$(BUILD_DIR)/tests/test_gart_utils

test-user-client:
	@mkdir -p $(BUILD_DIR)/tests
	$(CXX) -std=c++17 -Wall -Wextra -Werror -Itests/host-shim \
		tests/test_user_client_lifetime.cpp $(SRC_DIR)/hardware/PM4BuilderKernel.cpp \
		-o $(BUILD_DIR)/tests/test_user_client_lifetime
	./$(BUILD_DIR)/tests/test_user_client_lifetime
	python3 tests/run_vram_allocator_test.py

install: $(KEXT_DIR)
	@echo "Installing kext to /Library/Extensions..."
	sudo rm -rf /Library/Extensions/$(KEXT_NAME).kext
	sudo cp -R $(KEXT_DIR) /Library/Extensions/
	sudo chown -R root:wheel /Library/Extensions/$(KEXT_NAME).kext
	@echo "✓ Kext installed"
	@echo ""
	@echo "Register with: sudo kmutil load -p /Library/Extensions/$(KEXT_NAME).kext"

unload:
	@echo "Unloading kext..."
	-sudo kmutil unload -b $(BUNDLE_ID) 2>/dev/null || true
	@echo "✓ Kext unloaded"

status:
	@echo "=== Kext Status ==="
	@kmutil showloaded --list-only | grep -i rdna4 || echo "Kext not loaded"
	@echo ""
	@echo "=== Recent Logs ==="
	@log show --predicate 'processImagePath contains "kernel"' --last 1m | grep RDNA4 || echo "No recent logs"

help:
	@echo "RDNA4 Driver Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all        - Build the kext (default)"
	@echo "  clean      - Remove build files"
	@echo "  install    - Install kext to /Library/Extensions"
	@echo "  unload     - Unload currently loaded kext"
	@echo "  status     - Show kext load status and recent logs"
	@echo ""
	@echo "Environment:"
	@echo "  MKSDK      - Path to MacKernelSDK (default: <repo>/navi48/MacKernelSDK)"
	@echo ""
	@echo "Usage:"
	@echo "  1. Build:    make"
	@echo "  2. Install:  make install"
	@echo "  3. Register: sudo kmutil load -p /Library/Extensions/$(KEXT_NAME).kext"
	@echo "  4. Reboot when requested"
	@echo "  5. Check:    make status"
