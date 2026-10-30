# RDNA4Metal Bundle Makefile
# Phase 3.3 Complete Implementation

# Bundle info
BUNDLE_NAME = RDNA4Metal
BUNDLE_ID = com.rdna4.metal
VERSION = 1.0.0

# Directories
SRC_DIR = src
BUILD_DIR = build
BUNDLE_DIR = $(BUILD_DIR)/$(BUNDLE_NAME).bundle
CONTENTS = $(BUNDLE_DIR)/Contents
MACOS = $(CONTENTS)/MacOS
RESOURCES = $(CONTENTS)/Resources

# Source files
OBJC_SOURCES = \
	$(SRC_DIR)/RDNA4Device_Complete.m \
	$(SRC_DIR)/RDNA4Library.m \
	$(SRC_DIR)/RDNA4Function.m \
	$(SRC_DIR)/RDNA4ComputePipelineState.m \
	$(SRC_DIR)/RDNA4Buffer.m \
	$(SRC_DIR)/RDNA4UserClient.m \
	$(SRC_DIR)/RDNA4CommandQueue.m \
	$(SRC_DIR)/RDNA4CommandBuffer.m \
	$(SRC_DIR)/RDNA4ComputeCommandEncoder.m

# Object files
OBJC_OBJECTS = $(OBJC_SOURCES:$(SRC_DIR)/%.m=$(BUILD_DIR)/%.o)

# Compiler
CC = clang
ARCH ?= x86_64

CFLAGS = -Wall -Wextra -std=c11 -O2 -arch $(ARCH) \
	-framework Foundation \
	-framework Metal \
	-framework IOKit \
	-dynamiclib \
	-install_name /Library/GPUBundles/$(BUNDLE_NAME).bundle/Contents/MacOS/$(BUNDLE_NAME) \
	-fobjc-arc \
	-I$(SRC_DIR)

# AIRNT plugin dependency
AIRNT_PLUGIN = ../airnt-plugin/libAMDNTPlugin.dylib

# Default target
all: bundle

# Create bundle structure
bundle: $(BUNDLE_DIR) $(MACOS)/$(BUNDLE_NAME) $(CONTENTS)/Info.plist $(MACOS)/libAMDNTPlugin.dylib

# Build main library
$(MACOS)/$(BUNDLE_NAME): $(OBJC_OBJECTS)
	@echo "Linking $(BUNDLE_NAME)..."
	$(CC) $(CFLAGS) -o $@ $(OBJC_OBJECTS)
	@echo "✅ Built $(BUNDLE_NAME)"

# Compile Objective-C sources
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.m
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Create bundle directory
$(BUNDLE_DIR):
	@mkdir -p $(MACOS)
	@mkdir -p $(RESOURCES)

# Copy Info.plist
$(CONTENTS)/Info.plist: Info.plist
	@echo "Copying Info.plist..."
	@cp Info.plist $(CONTENTS)/Info.plist
	@plutil -lint $(CONTENTS)/Info.plist || (echo "❌ Invalid Info.plist"; exit 1)

# Copy AIRNT plugin
$(MACOS)/libAMDNTPlugin.dylib: $(AIRNT_PLUGIN)
	@echo "Copying libAMDNTPlugin.dylib..."
	@cp $(AIRNT_PLUGIN) $(MACOS)/
	@echo "✅ Copied AIRNT plugin"

# Build AIRNT plugin if needed
$(AIRNT_PLUGIN):
	@echo "Building AIRNT plugin..."
	$(MAKE) -C ../airnt-plugin
	@echo "✅ Built AIRNT plugin"

# Install to system
install: bundle
	@echo "Installing to /Library/GPUBundles/..."
	sudo cp -R $(BUNDLE_DIR) /Library/GPUBundles/
	sudo chown -R root:wheel /Library/GPUBundles/$(BUNDLE_NAME).bundle
	@echo "✅ Installed $(BUNDLE_NAME).bundle"

# Uninstall from system
uninstall:
	@echo "Uninstalling from /Library/GPUBundles/..."
	sudo rm -rf /Library/GPUBundles/$(BUNDLE_NAME).bundle
	@echo "✅ Uninstalled $(BUNDLE_NAME).bundle"

# Build test application
test: bundle
	@echo "Building test application..."
	@mkdir -p $(BUILD_DIR)/test
	clang -framework Foundation -framework Metal \
		-o $(BUILD_DIR)/test/RDNA4MetalTest \
		test/RDNA4MetalTest.m
	@echo "✅ Built test application"
	@echo ""
	@echo "Run with: $(BUILD_DIR)/test/RDNA4MetalTest"

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR)
	@echo "✅ Cleaned build artifacts"

# Show info
info:
	@echo "RDNA4Metal Bundle Build System"
	@echo "=============================="
	@echo "Bundle: $(BUNDLE_NAME) v$(VERSION)"
	@echo "Bundle ID: $(BUNDLE_ID)"
	@echo "Sources: $(words $(OBJC_SOURCES)) files"
	@echo ""
	@echo "Targets:"
	@echo "  make all      - Build bundle"
	@echo "  make install  - Install to system"
	@echo "  make test     - Build test app"
	@echo "  make clean    - Clean build"

.PHONY: all bundle install uninstall test clean info
