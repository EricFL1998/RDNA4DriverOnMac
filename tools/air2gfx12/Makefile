LLVM_CONFIG ?= /opt/homebrew/opt/llvm/bin/llvm-config
CXX         ?= clang++

LLVM_CXXFLAGS := $(shell $(LLVM_CONFIG) --cxxflags)
LLVM_LDFLAGS  := $(shell $(LLVM_CONFIG) --ldflags)
LLVM_LIBS     := $(shell $(LLVM_CONFIG) --system-libs --libs core bitreader bitwriter linker ipo irreader transformutils target mc passes amdgpu asmparser all-targets)

CXXFLAGS := -std=c++17 -Wall -Wextra -Wno-unused-parameter -Isrc $(LLVM_CXXFLAGS)
LDFLAGS  := $(LLVM_LDFLAGS) $(LLVM_LIBS)

BIN_DIR := bin
OBJ_DIR := obj

SRCS := src/main.cpp src/MTLBUnpacker.cpp src/AIRLinker.cpp src/AIRGraphicsLowering.cpp src/GFX12Emitter.cpp src/ShaderMetadata.cpp src/PM4Builder.cpp
OBJS := $(patsubst src/%.cpp,$(OBJ_DIR)/%.o,$(SRCS))
TARGET := $(BIN_DIR)/air2gfx12

TEST_UNPACKER := $(BIN_DIR)/test_unpacker
TEST_LINKER   := $(BIN_DIR)/test_linker
TEST_EMITTER  := $(BIN_DIR)/test_emitter
TEST_METADATA := $(BIN_DIR)/test_metadata
TEST_PM4      := $(BIN_DIR)/test_pm4
TEST_GRAPHICS := $(BIN_DIR)/test_graphics_lowering

TEST_BINARIES := $(TEST_UNPACKER) $(TEST_LINKER) $(TEST_EMITTER) $(TEST_METADATA) $(TEST_PM4) $(TEST_GRAPHICS)

.PHONY: all clean test build_tests test_unpacker test_linker test_emitter test_metadata test_pm4 test_golden verify_phase1

all: $(TARGET)

build_tests: $(TEST_BINARIES)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CXX) $(OBJS) $(LDFLAGS) -o $@

$(OBJ_DIR)/%.o: src/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BIN_DIR) $(OBJ_DIR):
	mkdir -p $@

test_unpacker: $(TEST_UNPACKER)
	./$(TEST_UNPACKER)

$(TEST_UNPACKER): $(OBJ_DIR)/MTLBUnpacker.o tests/test_unpacker.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) tests/test_unpacker.cpp $(OBJ_DIR)/MTLBUnpacker.o -o $@

test_linker: $(TEST_LINKER)
	./$(TEST_LINKER)

$(TEST_LINKER): $(OBJ_DIR)/AIRLinker.o $(OBJ_DIR)/AIRGraphicsLowering.o $(OBJ_DIR)/MTLBUnpacker.o tests/test_linker.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) tests/test_linker.cpp $(OBJ_DIR)/AIRLinker.o $(OBJ_DIR)/AIRGraphicsLowering.o $(OBJ_DIR)/MTLBUnpacker.o $(LDFLAGS) -o $@

test_emitter: $(TEST_EMITTER)
	./$(TEST_EMITTER)

$(TEST_EMITTER): $(OBJ_DIR)/GFX12Emitter.o tests/test_emitter.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) tests/test_emitter.cpp $(OBJ_DIR)/GFX12Emitter.o $(LDFLAGS) -o $@

test_metadata: $(TEST_METADATA)
	./$(TEST_METADATA)

$(TEST_METADATA): $(OBJ_DIR)/ShaderMetadata.o tests/test_metadata.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) tests/test_metadata.cpp $(OBJ_DIR)/ShaderMetadata.o $(LDFLAGS) -o $@

test_pm4: $(TEST_PM4)
	./$(TEST_PM4)

$(TEST_PM4): $(OBJ_DIR)/PM4Builder.o tests/test_pm4.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) tests/test_pm4.cpp $(OBJ_DIR)/PM4Builder.o -o $@

test_golden: all
	bash tests/batch_golden_test.sh

test_graphics: $(TEST_GRAPHICS)
	./$(TEST_GRAPHICS)

$(TEST_GRAPHICS): tests/test_graphics_lowering.cpp $(OBJ_DIR)/AIRGraphicsLowering.o $(OBJ_DIR)/GFX12Emitter.o | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

verify_phase1: all build_tests
	bash tests/phase1_full_verification.sh

clean:
	rm -rf $(BIN_DIR) $(OBJ_DIR)

test: all test_unpacker test_linker test_emitter test_metadata test_pm4 test_graphics test_golden
	bash tests/test_cli.sh
	bash tests/e2e_test.sh
