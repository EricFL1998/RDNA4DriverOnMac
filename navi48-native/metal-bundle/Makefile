# RDNA4Metal Bundle Makefile
# Phase 3.3 Complete Implementation

# Bundle info
BUNDLE_NAME = RDNA4Metal
BUNDLE_ID = com.rdna4.metal
VERSION = 1.0.2

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
	$(SRC_DIR)/RDNA4MetalPlugin.m \
	$(SRC_DIR)/RDNA4Function.m \
	$(SRC_DIR)/RDNA4ComputePipelineState.m \
	$(SRC_DIR)/RDNA4Buffer.m \
	$(SRC_DIR)/RDNA4UserClient.m \
	$(SRC_DIR)/RDNA4CommandQueue.m \
	$(SRC_DIR)/RDNA4CommandBuffer.m \
	$(SRC_DIR)/RDNA4ComputeCommandEncoder.m

# Graphics (render) sources: the encoder now shares the driver's PM4 encoder.
GFX_ENCODER_DIR = $(abspath $(SRC_DIR)/../../../RDNA4Driver/src/hardware)

OBJC_GRAPHICS_SOURCES = \
	$(SRC_DIR)/graphics/RDNA4RenderCommandEncoder.m \
	$(SRC_DIR)/graphics/RDNA4Texture.m \
	$(SRC_DIR)/graphics/RDNA4RenderPipelineState.m \
	$(SRC_DIR)/graphics/RDNA4RenderPipelineDescriptor.m \
	$(SRC_DIR)/graphics/RDNA4RenderPassDescriptor.m \
	$(SRC_DIR)/graphics/RDNA4VertexDescriptor.m

# Object files
OBJC_OBJECTS = $(OBJC_SOURCES:$(SRC_DIR)/%.m=$(BUILD_DIR)/%.o)
GRAPHICS_OBJECTS = $(OBJC_GRAPHICS_SOURCES:$(SRC_DIR)/%.m=$(BUILD_DIR)/%.o)
CXX_OBJECTS = $(BUILD_DIR)/RDNA4Library.o $(BUILD_DIR)/graphics/RDNA4RenderPM4Builder.o \
	$(BUILD_DIR)/gfx/GraphicsPM4Builder.o $(BUILD_DIR)/gfx/MTLBUnpacker.o

# Compiler
CC = clang
ARCH ?= x86_64

CFLAGS = -Wall -Wextra -std=c11 -O2 -arch $(ARCH) \
	-mmacosx-version-min=13.0 -fobjc-arc -fvisibility=default -MMD -MP \
	-I$(SRC_DIR) -I$(SRC_DIR)/graphics -I$(GFX_ENCODER_DIR)

LDFLAGS = -arch $(ARCH) -mmacosx-version-min=13.0 \
	-framework Foundation \
	-framework Metal \
	-framework IOKit \
	-framework IOSurface \
	-dynamiclib \
	-lc++ \
	-install_name /Library/GPUBundles/$(BUNDLE_NAME).bundle/Contents/MacOS/$(BUNDLE_NAME)

# AIRNT plugin dependency
AIRNT_PLUGIN = ../airnt-plugin/libAMDNTPlugin.dylib

# Default target
all: bundle

# Create bundle structure
bundle: $(BUNDLE_DIR) $(MACOS)/$(BUNDLE_NAME) $(CONTENTS)/Info.plist

# Build main library
$(MACOS)/$(BUNDLE_NAME): $(OBJC_OBJECTS) $(GRAPHICS_OBJECTS) $(CXX_OBJECTS) Makefile | $(BUNDLE_DIR)
	@echo "Linking $(BUNDLE_NAME)..."
	$(CC) $(LDFLAGS) -o $@ $(OBJC_OBJECTS) $(GRAPHICS_OBJECTS) $(CXX_OBJECTS)
	@echo "✅ Built $(BUNDLE_NAME)"

# Compile Objective-C sources
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.m Makefile
	@mkdir -p $(dir $@)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Objective-C++ and C++ objects (the shared PM4 encoder)
CXXFLAGS_BUNDLE = -Wall -O2 -arch $(ARCH) -fobjc-arc -std=c++17 \
	-mmacosx-version-min=13.0 -MMD -MP \
	-fno-exceptions -fno-rtti \
	-I$(SRC_DIR) -I$(SRC_DIR)/graphics -I$(GFX_ENCODER_DIR)

$(BUILD_DIR)/RDNA4Library.o: $(SRC_DIR)/RDNA4Library.mm Makefile
	@mkdir -p $(dir $@)
	@echo "Compiling $<..."
	$(CC) $(CXXFLAGS_BUNDLE) -c $< -o $@

$(BUILD_DIR)/gfx/MTLBUnpacker.o: ../../tools/air2gfx12/src/MTLBUnpacker.cpp Makefile
	@mkdir -p $(dir $@)
	@echo "Compiling $<..."
	$(CC) $(CXXFLAGS_BUNDLE) -c $< -o $@

$(BUILD_DIR)/graphics/RDNA4RenderPM4Builder.o: $(SRC_DIR)/graphics/RDNA4RenderPM4Builder.mm Makefile
	@mkdir -p $(dir $@)
	@echo "Compiling $<..."
	$(CC) $(CXXFLAGS_BUNDLE) -c $< -o $@

$(BUILD_DIR)/gfx/GraphicsPM4Builder.o: $(GFX_ENCODER_DIR)/GraphicsPM4Builder.cpp Makefile
	@mkdir -p $(dir $@)
	@echo "Compiling shared encoder $<..."
	$(CC) $(CXXFLAGS_BUNDLE) -c $< -o $@

# Create bundle directory
$(BUNDLE_DIR):
	@mkdir -p $(MACOS)
	@mkdir -p $(RESOURCES)

# Copy Info.plist
$(CONTENTS)/Info.plist: Info.plist | $(BUNDLE_DIR)
	@echo "Copying Info.plist..."
	@cp Info.plist $(CONTENTS)/Info.plist
	@plutil -lint $(CONTENTS)/Info.plist || (echo "❌ Invalid Info.plist"; exit 1)

# The AIRNT plugin embeds LLVM and must be built for the GPU host (x86_64).
# This build host cannot produce it, and a stale or truncated copy here would
# silently ship and break shader compilation, so it is installed separately on
# the target (see dist notes) rather than copied by this Makefile.
install-plugin:
	@test -f $(AIRNT_PLUGIN) || (echo "❌ $(AIRNT_PLUGIN) missing"; exit 1)
	@file $(AIRNT_PLUGIN) | grep -q x86_64 || (echo "❌ $(AIRNT_PLUGIN) is not x86_64"; exit 1)
	@mkdir -p $(MACOS)
	@cp $(AIRNT_PLUGIN) $(MACOS)/
	@echo "✅ Installed AIRNT plugin into the bundle"

# Build AIRNT plugin if needed
$(AIRNT_PLUGIN):
	@echo "Building AIRNT plugin..."
	$(MAKE) -C ../airnt-plugin
	@echo "✅ Built AIRNT plugin"

# Install to system
install: bundle
	@echo "Installing to /Library/GPUBundles/..."
	sudo mkdir -p /Library/GPUBundles
	sudo cp -R $(BUNDLE_DIR) /Library/GPUBundles/
	sudo chown -R root:wheel /Library/GPUBundles/$(BUNDLE_NAME).bundle
	@echo "✅ Installed $(BUNDLE_NAME).bundle"

# Uninstall from system
uninstall:
	@echo "Uninstalling from /Library/GPUBundles/..."
	sudo rm -rf /Library/GPUBundles/$(BUNDLE_NAME).bundle
	@echo "✅ Uninstalled $(BUNDLE_NAME).bundle"

# Build test application
test-registration: $(BUILD_DIR)/test/DeviceRegistrationTest

test-shaders: $(BUILD_DIR)/test/ShaderCompilationTest

test-render-pipeline: $(BUILD_DIR)/test/RenderPipelineTest

$(BUILD_DIR)/test/RenderPipelineTest: test/RenderPipelineTest.m Makefile
	@mkdir -p $(dir $@)
	$(CC) -arch $(ARCH) -mmacosx-version-min=13.0 -fobjc-arc -Wall -Wextra -Werror \
		-Isrc -framework Foundation -framework Metal -framework IOKit -o $@ $<

$(BUILD_DIR)/test/DeviceRegistrationTest: test/DeviceRegistrationTest.m Makefile
	@mkdir -p $(dir $@)
	$(CC) -arch $(ARCH) -mmacosx-version-min=13.0 -fobjc-arc -Wall -Wextra -Werror \
		-framework Foundation -framework Metal -framework IOKit -o $@ $<

$(BUILD_DIR)/test/ShaderCompilationTest: test/ShaderCompilationTest.m Makefile
	@mkdir -p $(dir $@)
	$(CC) -arch $(ARCH) -mmacosx-version-min=13.0 -fobjc-arc -Wall -Wextra -Werror \
		-framework Foundation -framework Metal -o $@ $<

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

-include $(OBJC_OBJECTS:.o=.d) $(GRAPHICS_OBJECTS:.o=.d) $(CXX_OBJECTS:.o=.d)

.PHONY: all bundle install uninstall test test-registration clean info
