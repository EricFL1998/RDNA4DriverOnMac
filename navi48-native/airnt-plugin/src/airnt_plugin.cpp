#include "airnt_plugin.h"
#include <string>
#include <memory>
#include <sstream>
#include <fstream>
#include <atomic>
#include <cstdlib>
#include <unistd.h>

// Per-process temporary files: several compiler instances (or a root-owned
// leftover from an earlier run) must not collide on one fixed /tmp name.
static std::string tempPath(const char* suffix) {
    static std::atomic<unsigned> counter{0};
    return "/tmp/airnt-" + std::to_string(getpid()) + "-" +
           std::to_string(counter.fetch_add(1)) + suffix;
}

// Import air2gfx12 core components
#include "../../../tools/air2gfx12/src/MTLBUnpacker.hpp"
#include <llvm/Transforms/Utils/Cloning.h>
#include "../../../tools/air2gfx12/src/AIRLinker.hpp"
#include "../../../tools/air2gfx12/src/GFX12Emitter.hpp"
#include "../../../tools/air2gfx12/src/ShaderMetadata.hpp"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Support/Error.h>

// Internal context structure
struct AIRNTContext_t {
    uint32_t apiVersion;
    llvm::LLVMContext llvmContext;
    std::unique_ptr<air2gfx12::AIRLinker> linker;
    
    AIRNTContext_t(uint32_t ver) : apiVersion(ver) {
        linker = std::make_unique<air2gfx12::AIRLinker>(llvmContext);
    }
};

// Internal executable structure
struct AIRNTExecutable_t {
    std::vector<uint8_t> machineCode;
    std::string assembly;
    AIRNTShaderMetadata metadata;
    std::vector<AIRNTBinding> bindings;
};

//==============================================================================
// Plugin Lifecycle
//==============================================================================

AIRNTStatus AIRNTInit(AIRNTContext* outContext, uint32_t apiVersion) {
    if (!outContext) return AIRNT_ERROR_INVALID_INPUT;
    if (apiVersion != AIRNT_API_VERSION) return AIRNT_ERROR_INVALID_INPUT;
    
    try {
        *outContext = new AIRNTContext_t(apiVersion);
        return AIRNT_SUCCESS;
    } catch (...) {
        return AIRNT_ERROR_OUT_OF_MEMORY;
    }
}

void AIRNTDestroy(AIRNTContext context) {
    delete context;
}

uint32_t AIRNTGetVersion(void) {
    return AIRNT_API_VERSION;
}

//==============================================================================
// Compilation API
//==============================================================================

AIRNTStatus AIRNTCompile(AIRNTContext context,
                         const void* airBitcode,
                         size_t bitcodeSize,
                         const AIRNTCompileOptions* options,
                         AIRNTExecutable* outExecutable,
                         char** outErrorMessage) {
    if (!context || !airBitcode || !options || !outExecutable) {
        return AIRNT_ERROR_INVALID_INPUT;
    }
    
    try {
        // Load runtime library if not already loaded
        if (options->runtimeLibraryPath) {
            if (!context->linker->loadRuntime(options->runtimeLibraryPath)) {
                if (outErrorMessage) {
                    *outErrorMessage = strdup("Failed to load runtime library");
                }
                return AIRNT_ERROR_LINK_FAILED;
            }
        } else {
            // Auto-detect runtime
            std::string runtimePath = air2gfx12::RuntimeLocator::locate();
            if (!runtimePath.empty()) {
                context->linker->loadRuntime(runtimePath);
            }
        }
        
        air2gfx12::ShaderStage stage = air2gfx12::ShaderStage::Kernel;
        if (options->functionType == AIRNT_FUNCTION_TYPE_VERTEX) {
            stage = air2gfx12::ShaderStage::Vertex;
        } else if (options->functionType == AIRNT_FUNCTION_TYPE_FRAGMENT) {
            stage = air2gfx12::ShaderStage::Fragment;
        }
        std::string errStr;
        auto module = context->linker->loadAndLink(
            reinterpret_cast<const uint8_t*>(airBitcode),
            bitcodeSize,
            options->entryPointName,
            stage,
            &errStr
        );
        
        if (!module) {
            if (outErrorMessage) {
                *outErrorMessage = strdup(errStr.c_str());
            }
            return AIRNT_ERROR_LINK_FAILED;
        }
        
        auto exec = new AIRNTExecutable_t();
        for (const air2gfx12::ShaderBinding& binding : context->linker->bindings()) {
            AIRNTBinding out = {};
            out.userSgpr = binding.userSgpr;
            out.locationIndex = binding.locationIndex;
            out.kind = binding.kind;
            out.addressSpace = binding.addressSpace;
            exec->bindings.push_back(out);
        }
        // Emission runs the whole codegen pipeline, which rewrites the module
        // (SIAnnotateControlFlow leaves amdgcn.if behind, for one). A second
        // emission of the same module therefore reaches instruction selection
        // with codegen intrinsics in it and aborts, so each emission gets a
        // private copy.
        std::string asmPath = tempPath(".s");
        std::string objPath = tempPath(".o");
        struct TempCleanup {
            const std::string& assemblyPath;
            const std::string& objectPath;
            ~TempCleanup() {
                unlink(assemblyPath.c_str());
                unlink(objectPath.c_str());
            }
        } tempCleanup{asmPath, objPath};
        {
            std::unique_ptr<llvm::Module> assemblyModule = llvm::CloneModule(*module);
            air2gfx12::GFX12Emitter assemblyEmitter(stage);
            if (!assemblyEmitter.emitAssembly(*assemblyModule, asmPath, &errStr)) {
                delete exec;
                if (outErrorMessage) {
                    *outErrorMessage = strdup(errStr.c_str());
                }
                return AIRNT_ERROR_CODEGEN_FAILED;
            }
        }
        air2gfx12::GFX12Emitter emitter(stage);
        
        // Read assembly
        std::ifstream asmFile(asmPath);
        if (asmFile.is_open()) {
            exec->assembly = std::string(
                (std::istreambuf_iterator<char>(asmFile)),
                std::istreambuf_iterator<char>()
            );
        }
        
        // Generate ELF object
        if (!emitter.emitObject(*module, objPath, &errStr)) {
            delete exec;
            if (outErrorMessage) {
                *outErrorMessage = strdup(errStr.c_str());
            }
            return AIRNT_ERROR_CODEGEN_FAILED;
        }
        
        std::ifstream objFile(objPath, std::ios::binary);
        std::vector<uint8_t> elfBytes((std::istreambuf_iterator<char>(objFile)),
                                      std::istreambuf_iterator<char>());
        auto object = llvm::object::ELFObjectFile<llvm::object::ELF64LE>::create(
            llvm::MemoryBufferRef(llvm::StringRef(reinterpret_cast<const char*>(elfBytes.data()),
                                                  elfBytes.size()), "airnt-shader"));
        if (!object) {
            delete exec;
            if (outErrorMessage) *outErrorMessage = strdup(llvm::toString(object.takeError()).c_str());
            return AIRNT_ERROR_CODEGEN_FAILED;
        }

        // The loader below copies .text as one blob and the bundle programs the
        // shader at its base address, so the entry must be the first function and
        // nothing may need a relocation. The compiler's inlining and constant
        // folding passes guarantee that; refuse a shader that slipped through
        // instead of silently executing the wrong bytes.
        bool entryFound = false;
        uint64_t entryOffset = 0;
        for (const llvm::object::SymbolRef& symbol : object->symbols()) {
            auto name = symbol.getName();
            if (!name || *name != options->entryPointName) continue;
            auto sectionOrErr = symbol.getSection();
            if (!sectionOrErr || *sectionOrErr == object->section_end()) continue;
            llvm::object::SectionRef section = **sectionOrErr;
            auto sectionName = section.getName();
            if (!sectionName || *sectionName != ".text") continue;
            auto value = symbol.getValue();
            if (!value) {
                delete exec;
                if (outErrorMessage) *outErrorMessage = strdup(llvm::toString(value.takeError()).c_str());
                return AIRNT_ERROR_CODEGEN_FAILED;
            }
            entryFound = true;
            entryOffset = *value;
            break;
        }
        if (!entryFound || entryOffset != 0) {
            delete exec;
            if (outErrorMessage) {
                *outErrorMessage = strdup("entry is not at .text offset 0 (inlining/constant folding failed)");
            }
            return AIRNT_ERROR_CODEGEN_FAILED;
        }
        for (const llvm::object::SectionRef& section : object->sections()) {
            auto name = section.getName();
            if (!name || *name != ".rel.text") continue;
            if (section.getSize() != 0) {
                delete exec;
                if (outErrorMessage) {
                    *outErrorMessage = strdup(".text still has relocations (shader is not self-contained)");
                }
                return AIRNT_ERROR_CODEGEN_FAILED;
            }
        }

        for (const llvm::object::SectionRef& section : object->sections()) {
            auto name = section.getName();
            if (!name || *name != ".text") continue;
            auto contents = section.getContents();
            if (!contents) continue;
            exec->machineCode.assign(contents->begin(), contents->end());
        }
        if (exec->machineCode.empty()) {
            delete exec;
            if (outErrorMessage) *outErrorMessage = strdup("ELF has no executable .text section");
            return AIRNT_ERROR_CODEGEN_FAILED;
        }
        
        air2gfx12::KernelStats s;
        bool hasStats = false;
        if (stage == air2gfx12::ShaderStage::Kernel) {
            std::vector<air2gfx12::KernelStats> stats;
            if (air2gfx12::ShaderMetadata::parseFromAssembly(exec->assembly, stats)) {
                for (const auto& candidate : stats) {
                    if (candidate.name == options->entryPointName) {
                        s = candidate;
                        hasStats = true;
                        break;
                    }
                }
            }
        } else {
            hasStats = air2gfx12::ShaderMetadata::parseGraphicsFromAssembly(
                exec->assembly, options->entryPointName, s);
        }
        if (stage != air2gfx12::ShaderStage::Kernel && !hasStats) {
            delete exec;
            if (outErrorMessage) *outErrorMessage = strdup("Graphics shader metadata is unavailable");
            return AIRNT_ERROR_CODEGEN_FAILED;
        }
        if (hasStats) {
            exec->metadata.sgprCount = s.sgprCount;            exec->metadata.vgprCount = s.vgprCount;
            exec->metadata.wavefrontSize = s.wavefrontSize;
            exec->metadata.scratchBytes = s.scratchBytes;
            exec->metadata.kernargBytes = s.kernargBytes;
            exec->metadata.ldsBytes = s.ldsBytes;
            if (stage == air2gfx12::ShaderStage::Kernel) {
                exec->metadata.computePgmRsrc1 = s.computePgmRsrc1();
                exec->metadata.computePgmRsrc2 = s.computePgmRsrc2();
            } else {
                exec->metadata.pgmRsrc1 = s.graphicsPgmRsrc1();
                const uint32_t resourceCount = (uint32_t)exec->bindings.size();
                if (stage == air2gfx12::ShaderStage::Vertex) {
                    // Resources live in the reserved s12..s15 pairs (first two)
                    // and s19+ (the rest), all inside the 11 user SGPRs the
                    // driver's GS block delivers.
                    exec->metadata.pgmRsrc2 = 11u << 1;
                } else if (stage == air2gfx12::ShaderStage::Fragment) {
                    // Fragment system slots are s2..s4 and resources start at
                    // s5; a shader with no resources keeps the historical 0.
                    exec->metadata.pgmRsrc2 = resourceCount
                        ? (5u + 2u * resourceCount) << 1
                        : 0u;
                } else {
                    exec->metadata.pgmRsrc2 = 0;
                }
                if (stage == air2gfx12::ShaderStage::Vertex) {
                    llvm::NamedMDNode* stages = module->getNamedMetadata("air.vertex");
                    if (stages && !stages->operands().empty()) {
                        auto* outputs = llvm::dyn_cast_or_null<llvm::MDNode>(
                            stages->getOperand(0)->getOperand(1));
                        if (outputs) {
                            for (const llvm::MDOperand& output : outputs->operands()) {
                                auto* node = llvm::dyn_cast_or_null<llvm::MDNode>(output);
                                auto* kind = node && node->getNumOperands() > 0
                                    ? llvm::dyn_cast_or_null<llvm::MDString>(node->getOperand(0)) : nullptr;
                                if (kind && kind->getString() == "air.vertex_output") {
                                    ++exec->metadata.vertexParameterCount;
                                }
                            }
                        }
                    }
                }
            }
        }
        
        if (const char* value = getenv("N48_VS_RSRC2")) {
            if (stage == air2gfx12::ShaderStage::Vertex) {
                exec->metadata.pgmRsrc2 = (uint32_t)strtoul(value, nullptr, 0);
            }
        }
        if (const char* value = getenv("N48_PS_RSRC2")) {
            if (stage == air2gfx12::ShaderStage::Fragment) {
                exec->metadata.pgmRsrc2 = (uint32_t)strtoul(value, nullptr, 0);
            }
        }

        *outExecutable = exec;
        return AIRNT_SUCCESS;
        
    } catch (const std::exception& e) {
        if (outErrorMessage) {
            *outErrorMessage = strdup(e.what());
        }
        return AIRNT_ERROR_CODEGEN_FAILED;
    }
}

AIRNTStatus AIRNTGetMachineCode(AIRNTExecutable executable,
                                const void** outCode,
                                size_t* outSize) {
    if (!executable || !outCode || !outSize) {
        return AIRNT_ERROR_INVALID_INPUT;
    }
    
    *outCode = executable->machineCode.data();
    *outSize = executable->machineCode.size();
    return AIRNT_SUCCESS;
}

AIRNTStatus AIRNTGetMetadata(AIRNTExecutable executable,
                             AIRNTShaderMetadata* outMetadata) {
    if (!executable || !outMetadata) {
        return AIRNT_ERROR_INVALID_INPUT;
    }
    
    *outMetadata = executable->metadata;
    return AIRNT_SUCCESS;
}

AIRNTStatus AIRNTGetBindings(AIRNTExecutable executable,
                             const AIRNTBinding** outBindings,
                             size_t* outCount) {
    if (!executable || !outBindings || !outCount) {
        return AIRNT_ERROR_INVALID_INPUT;
    }

    *outBindings = executable->bindings.empty() ? nullptr : executable->bindings.data();
    *outCount = executable->bindings.size();
    return AIRNT_SUCCESS;
}

AIRNTStatus AIRNTGetAssembly(AIRNTExecutable executable, char** outAssembly) {
    if (!executable || !outAssembly) {
        return AIRNT_ERROR_INVALID_INPUT;
    }
    
    *outAssembly = strdup(executable->assembly.c_str());
    return AIRNT_SUCCESS;
}

void AIRNTFreeExecutable(AIRNTExecutable executable) {
    delete executable;
}

void AIRNTFreeString(char* str) {
    free(str);
}

//==============================================================================
// Pipeline Compilation (FlatBuffers - stub for now)
//==============================================================================

AIRNTStatus AIRNTCompilePipeline(AIRNTContext context,
                                 const void* flatbufferData,
                                 size_t dataSize,
                                 AIRNTExecutable* outExecutable,
                                 char** outErrorMessage) {
    // TODO: Parse FlatBuffers descriptor and extract AIR bitcode
    // This requires reverse-engineering Metal's internal pipeline format
    if (outErrorMessage) {
        *outErrorMessage = strdup("FlatBuffers pipeline compilation not yet implemented");
    }
    return AIRNT_ERROR_NOT_IMPLEMENTED;
}
