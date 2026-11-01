#include "MTLBUnpacker.hpp"
#include "AIRLinker.hpp"
#include "GFX12Emitter.hpp"
#include "ShaderMetadata.hpp"

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>

// Symbols the AMDGPU object format always leaves for the final linker, not
// shader runtime calls: they do not need a GOT entry in the loaded .text.
static bool collectUnresolvedSymbols(const std::string& path, std::vector<std::string>& out) {
    auto buffer = llvm::MemoryBuffer::getFile(path);
    if (!buffer) return false;
    auto object = llvm::object::ObjectFile::createObjectFile((*buffer)->getMemBufferRef());
    if (!object) return false;
    for (const llvm::object::SymbolRef& symbol : (*object)->symbols()) {
        auto name = symbol.getName();
        if (!name) continue;
        auto flags = symbol.getFlags();
        if (!flags) {
            llvm::consumeError(flags.takeError());
            continue;
        }
        if (!(*flags & llvm::object::SymbolRef::SF_Undefined)) continue;
        if (*name == "__init_array_start" || *name == "__init_array_end") continue;
        out.push_back(name->str());
    }
    return true;
}

// Dumps the entry's AIR module exactly as packed in the metallib, before the
// runtime link and the graphics ABI rewrite. Used to inspect builtin
// signatures (air.sample_texture_2d and friends) while lowering them.
static bool dumpEntryIR(const std::vector<uint8_t>& bitcode, const std::string& path,
                        llvm::LLVMContext& context) {
    llvm::MemoryBufferRef ref(llvm::StringRef(reinterpret_cast<const char*>(bitcode.data()),
                                              bitcode.size()), "air-entry");
    auto module = llvm::parseBitcodeFile(ref, context);
    if (!module) return false;
    std::error_code ec;
    llvm::raw_fd_ostream out(path, ec, llvm::sys::fs::OF_None);
    if (ec) return false;
    module->get()->print(out, nullptr);
    return true;
}

void printUsage(const char*) {
    std::cout << "air2gfx12: RDNA4 Native Metal Shader Compiler (Phase 1.1)\n\n"
              << "Usage: air2gfx12 <input.metallib|input.air> [options]\n\n"
              << "Options:\n"
              << "  -o <path>             Output GFX12 assembly file (.s)\n"
              << "  --emit-obj <path>     Output ELF object file (.o)\n"
              << "  --emit-ir <path>      Dump the entry's pre-link AIR module as LLVM IR\n"
              << "  --emit-lowered-ir <path>  Dump the linked/lowered module as LLVM IR\n"
              << "  --require-resolved    Fail when the emitted object leaves shader symbols undefined\n"
              << "  --bindings            Print the graphics resource binding table\n"
              << "  --runtime <path>      Path to runtime.amdil.bc (auto-detected if omitted)\n"
              << "  --kernel <name>       Specific kernel name to compile\n"
              << "  --list                List all kernels inside the metallib and exit\n"
              << "  --stats               Print SGPR/VGPR/Scratch and GFX12 register metrics\n"
              << "  -v, --verbose         Verbose logging\n"
              << "  -h, --help            Show this help message\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    std::string inputPath;
    std::string outAsmPath;
    std::string outObjPath;
    std::string outIRPath;
    std::string outLoweredIRPath;
    std::string runtimePath;
    std::string targetKernel;
    bool listOnly = false;
    bool showStats = false;
    bool requireResolved = false;
    bool showBindings = false;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "-o" && i + 1 < argc) {
            outAsmPath = argv[++i];
        } else if (arg == "--emit-obj" && i + 1 < argc) {
            outObjPath = argv[++i];
        } else if (arg == "--emit-ir" && i + 1 < argc) {
            outIRPath = argv[++i];
        } else if (arg == "--emit-lowered-ir" && i + 1 < argc) {
            outLoweredIRPath = argv[++i];
        } else if (arg == "--runtime" && i + 1 < argc) {
            runtimePath = argv[++i];
        } else if (arg == "--kernel" && i + 1 < argc) {
            targetKernel = argv[++i];
        } else if (arg == "--require-resolved") {
            requireResolved = true;
        } else if (arg == "--bindings") {
            showBindings = true;
        } else if (arg == "--list") {
            listOnly = true;
        } else if (arg == "--stats") {
            showStats = true;
        } else if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        } else if (arg[0] != '-') {
            inputPath = arg;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return 1;
        }
    }

    if (inputPath.empty()) {
        std::cerr << "Error: No input file specified.\n";
        return 1;
    }

    if (requireResolved && outObjPath.empty()) {
        std::cerr << "Error: --require-resolved needs --emit-obj.\n";
        return 1;
    }

    // Auto-locate runtime if not specified
    if (runtimePath.empty()) {
        runtimePath = air2gfx12::RuntimeLocator::locate();
        if (verbose && !runtimePath.empty()) {
            std::cout << "Auto-detected AIR runtime: " << runtimePath << "\n";
        }
    }

    // Read input file
    std::ifstream inFile(inputPath, std::ios::binary);
    if (!inFile.is_open()) {
        std::cerr << "Error: Could not open input file: " << inputPath << "\n";
        return 1;
    }

    std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(inFile)), std::istreambuf_iterator<char>());
    if (verbose) {
        std::cout << "Loaded " << buffer.size() << " bytes from " << inputPath << "\n";
    }

    // Unpack entries
    std::vector<air2gfx12::FunctionEntry> entries;
    if (!air2gfx12::MTLBUnpacker::unpack(buffer.data(), buffer.size(), entries)) {
        std::cerr << "Error: Failed to parse input file as MTLB or AIR bitcode.\n";
        return 1;
    }

    if (listOnly) {
        std::cout << "Found " << entries.size() << " function(s) in " << inputPath << ":\n";
        for (size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            const char* typeStr = "Unknown";
            if (e.type == air2gfx12::FunctionType::Vertex) typeStr = "Vertex";
            else if (e.type == air2gfx12::FunctionType::Fragment) typeStr = "Fragment";
            else if (e.type == air2gfx12::FunctionType::Compute) typeStr = "Compute";
            std::cout << "  [" << i << "] " << e.name << " (" << typeStr << ", " << e.bitcode.size() << " bytes bitcode)\n";
        }
        return 0;
    }

    // Initialize compiler components
    llvm::LLVMContext context;
    air2gfx12::AIRLinker linker(context);

    if (!runtimePath.empty()) {
        if (!linker.loadRuntime(runtimePath)) {
            std::cerr << "Warning: Could not load runtime bitcode from " << runtimePath << "\n";
        } else if (verbose) {
            std::cout << "Loaded AIR runtime from " << runtimePath << "\n";
        }
    }

    // Determine which entries to compile
    bool compiledAny = false;
    for (const auto& entry : entries) {
        if (!targetKernel.empty() && entry.name != targetKernel) {
            continue;
        }

        air2gfx12::ShaderStage stage = air2gfx12::ShaderStage::Kernel;
        if (entry.type == air2gfx12::FunctionType::Vertex) {
            stage = air2gfx12::ShaderStage::Vertex;
        } else if (entry.type == air2gfx12::FunctionType::Fragment) {
            stage = air2gfx12::ShaderStage::Fragment;
        }
        air2gfx12::GFX12Emitter emitter(stage);

        if (verbose) {
            std::cout << "Compiling " << entry.name << " (" << entry.bitcode.size() << " bytes)...\n";
        }

        if (!outIRPath.empty()) {
            std::string irPath = outIRPath;
            if (entries.size() > 1) irPath += "." + entry.name + ".ll";
            if (!dumpEntryIR(entry.bitcode, irPath, context)) {
                std::cerr << "Error dumping AIR IR for " << entry.name << ": " << irPath << "\n";
                return 1;
            }
            std::cout << "Dumped AIR IR: " << irPath << "\n";
        }

        std::string errStr;
        auto mod = linker.loadAndLink(entry.bitcode.data(), entry.bitcode.size(), entry.name, stage, &errStr);
        if (!mod) {
            std::cerr << "Error linking " << entry.name << ": " << errStr << "\n";
            continue;
        }

        if (showBindings) {
            for (const air2gfx12::ShaderBinding& binding : linker.bindings()) {
                std::cout << "  binding kind=" << binding.kind
                          << " location=" << binding.locationIndex
                          << " userSgpr=" << binding.userSgpr
                          << " addressSpace=" << binding.addressSpace
                          << " name=" << binding.name << "\n";
            }
        }

        if (!outLoweredIRPath.empty()) {
            std::string irPath = outLoweredIRPath;
            if (entries.size() > 1) irPath += "." + entry.name + ".ll";
            std::error_code ec;
            llvm::raw_fd_ostream out(irPath, ec, llvm::sys::fs::OF_None);
            if (ec) {
                std::cerr << "Error dumping lowered IR for " << entry.name << ": " << ec.message() << "\n";
                return 1;
            }
            mod->print(out, nullptr);
            std::cout << "Dumped lowered IR: " << irPath << "\n";
        }

        std::unique_ptr<llvm::Module> modForObj;
        if (!outObjPath.empty() && !outAsmPath.empty()) {
            modForObj = llvm::CloneModule(*mod);
        }

        if (!outAsmPath.empty()) {
            if (!emitter.emitAssembly(*mod, outAsmPath, &errStr)) {
                std::cerr << "Error emitting assembly: " << errStr << "\n";
                return 1;
            }
            std::cout << "Generated GFX12 assembly: " << outAsmPath << "\n";

            if (showStats) {
                std::vector<air2gfx12::KernelStats> stats;
                if (air2gfx12::ShaderMetadata::parseFromAssemblyFile(outAsmPath, stats)) {
                    for (const auto& s : stats) {
                        std::cout << "  [Stats] Kernel: " << s.name 
                                  << " | SGPRs: " << s.sgprCount 
                                  << " | VGPRs: " << s.vgprCount 
                                  << " | Wave: " << s.wavefrontSize 
                                  << " | Scratch: " << s.scratchBytes << "B"
                                  << " | GFX12 RSRC1: 0x" << std::hex << s.computePgmRsrc1()
                                  << " RSRC2: 0x" << s.computePgmRsrc2() << std::dec << "\n";
                    }
                }
            }
        }

        if (!outObjPath.empty()) {
            llvm::Module& objMod = modForObj ? *modForObj : *mod;
            if (!emitter.emitObject(objMod, outObjPath, &errStr)) {
                std::cerr << "Error emitting ELF object: " << errStr << "\n";
                return 1;
            }
            std::cout << "Generated GFX12 ELF object: " << outObjPath << "\n";

            if (requireResolved) {
                std::vector<std::string> unresolved;
                if (!collectUnresolvedSymbols(outObjPath, unresolved)) {
                    std::cerr << "Error reading emitted object: " << outObjPath << "\n";
                    return 1;
                }
                if (!unresolved.empty()) {
                    std::cerr << "Unresolved shader symbols in " << entry.name << ":\n";
                    for (const std::string& symbol : unresolved)
                        std::cerr << "  " << symbol << "\n";
                    return 1;
                }
            }

            if (showStats && outAsmPath.empty()) {
                std::vector<air2gfx12::KernelStats> stats;
                if (air2gfx12::ShaderMetadata::parseFromELFFile(outObjPath, stats)) {
                    for (const auto& s : stats) {
                        std::cout << "  [Stats] Kernel: " << s.name 
                                  << " | SGPRs: " << s.sgprCount 
                                  << " | VGPRs: " << s.vgprCount 
                                  << " | Scratch: " << s.scratchBytes << "B"
                                  << " | GFX12 RSRC1: 0x" << std::hex << s.computePgmRsrc1()
                                  << " RSRC2: 0x" << s.computePgmRsrc2() << std::dec << "\n";
                    }
                }
            }
        }

        compiledAny = true;
        if (!targetKernel.empty()) break;
    }

    if (!compiledAny) {
        if (!targetKernel.empty()) {
            std::cerr << "Error: Kernel '" << targetKernel << "' not found in input.\n";
            return 1;
        } else if (outAsmPath.empty() && outObjPath.empty()) {
            std::cout << "Unpacked " << entries.size() << " kernel(s). Specify -o <out.s> or --emit-obj <out.o> to generate code.\n";
        }
    }

    return 0;
}
