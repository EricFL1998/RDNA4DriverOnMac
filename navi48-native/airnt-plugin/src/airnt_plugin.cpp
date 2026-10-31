#include "airnt_plugin.h"
#include <string>
#include <memory>
#include <sstream>
#include <fstream>

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
        // Emission runs the whole codegen pipeline, which rewrites the module
        // (SIAnnotateControlFlow leaves amdgcn.if behind, for one). A second
        // emission of the same module therefore reaches instruction selection
        // with codegen intrinsics in it and aborts, so each emission gets a
        // private copy.
        std::string asmPath = "/tmp/airnt_temp.s";
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
        std::string objPath = "/tmp/airnt_temp.o";
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
            exec->metadata.sgprCount = s.sgprCount;
            exec->metadata.vgprCount = s.vgprCount;
            exec->metadata.wavefrontSize = s.wavefrontSize;
            exec->metadata.scratchBytes = s.scratchBytes;
            exec->metadata.kernargBytes = s.kernargBytes;
            exec->metadata.ldsBytes = s.ldsBytes;
            if (stage == air2gfx12::ShaderStage::Kernel) {
                exec->metadata.computePgmRsrc1 = s.computePgmRsrc1();
                exec->metadata.computePgmRsrc2 = s.computePgmRsrc2();
            } else {
                exec->metadata.pgmRsrc1 = s.graphicsPgmRsrc1();
                exec->metadata.pgmRsrc2 = stage == air2gfx12::ShaderStage::Vertex
                    ? (11u << 1) : 0; // s8..s18: descriptor, reserved, vertex base and stride
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
