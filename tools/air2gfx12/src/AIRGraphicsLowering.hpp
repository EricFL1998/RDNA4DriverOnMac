#pragma once

#include "GFX12Emitter.hpp"
#include <llvm/IR/Function.h>
#include <string>

namespace air2gfx12 {

llvm::Function* lowerGraphicsFunction(llvm::Function& function, ShaderStage stage,
                                      std::string* outError);

}
