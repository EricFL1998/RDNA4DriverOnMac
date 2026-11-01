LLVM_CONFIG ?= /opt/homebrew/opt/llvm/bin/llvm-config
CXX         ?= clang++
TARGET_ARCH ?= $(shell uname -m)

LLVM_CXXFLAGS := $(shell $(LLVM_CONFIG) --cxxflags)
LLVM_LDFLAGS  := $(shell $(LLVM_CONFIG) --ldflags)
LLVM_LIBS     := $(shell $(LLVM_CONFIG) --system-libs --libs core bitreader bitwriter linker ipo irreader transformutils target mc passes amdgpu asmparser all-targets)

CXXFLAGS := -std=c++17 -Wall -Wextra -Wno-unused-parameter -fPIC \
            -Iinclude -I../../tools/air2gfx12/src $(LLVM_CXXFLAGS) -fexceptions -arch $(TARGET_ARCH)
LDFLAGS  := -shared $(LLVM_LDFLAGS) $(LLVM_LIBS) -arch $(TARGET_ARCH)

AIR2GFX12_SRC := ../../tools/air2gfx12/src
AIR2GFX12_OBJS := $(AIR2GFX12_SRC)/MTLBUnpacker.o \
                  $(AIR2GFX12_SRC)/AIRLinker.o \
                  $(AIR2GFX12_SRC)/AIRGraphicsLowering.o \
                  $(AIR2GFX12_SRC)/AIRBuiltins.o \
                  $(AIR2GFX12_SRC)/GFX12Emitter.o \
                  $(AIR2GFX12_SRC)/ShaderMetadata.o

PLUGIN_SRC := src/airnt_plugin.cpp
PLUGIN_OBJ := src/airnt_plugin.o
TARGET := libAMDNTPlugin.dylib

TEST_SRC := tests/test_airnt_plugin.cpp
TEST_BIN := bin/test_airnt_plugin

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(PLUGIN_OBJ) $(AIR2GFX12_OBJS)
	$(CXX) $(PLUGIN_OBJ) $(AIR2GFX12_OBJS) $(LDFLAGS) -o $@

$(PLUGIN_OBJ): $(PLUGIN_SRC)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Build air2gfx12 objects if not already built
$(AIR2GFX12_OBJS): %.o: %.cpp $(wildcard $(AIR2GFX12_SRC)/*.hpp)
	$(CXX) $(CXXFLAGS) -c $< -o $@

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) $(TARGET) | bin
	$(CXX) $(CXXFLAGS) $(TEST_SRC) -L. -lAMDNTPlugin $(LLVM_LDFLAGS) $(LLVM_LIBS) -arch $(TARGET_ARCH) -o $@

bin:
	mkdir -p bin

clean:
	rm -f $(PLUGIN_OBJ) $(TARGET) $(TEST_BIN)
	rm -rf bin

install: $(TARGET)
	@echo "Installing libAMDNTPlugin.dylib..."
	@echo "NOTE: You need to create the Metal Bundle structure first"
	@echo "Target: /Library/GPUBundles/RDNA4Metal.bundle/Contents/MacOS/"
	@mkdir -p /tmp/RDNA4Metal.bundle/Contents/MacOS
	@cp $(TARGET) /tmp/RDNA4Metal.bundle/Contents/MacOS/
	@echo "Installed to /tmp/RDNA4Metal.bundle/ (move to /Library/GPUBundles/ when ready)"
