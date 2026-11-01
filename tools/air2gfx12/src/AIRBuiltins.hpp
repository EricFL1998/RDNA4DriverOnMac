#pragma once

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Module.h>
#include <string>

namespace air2gfx12 {

// Lowers the AIR runtime builtins Apple's Metal compiler leaves as external
// calls: texture sampling (air.sample_texture_*) and the __il_* math library.
// The emitted object must not keep a relocation for any of them, because the
// AIRNT loader copies only .text and would leave the call target unbound.
bool lowerRuntimeBuiltins(llvm::Module& module, std::string* outError = nullptr);

// Inlines every helper and builtin into the entry function. The AIRNT loader
// copies a shader as one .text blob and cannot relocate calls between
// functions; after this pass the entry is the only .text function.
bool inlineHelperFunctions(llvm::Module& module, llvm::StringRef entryName);

// Folds the air.function_constants metadata into the loads of the globals it
// describes, so a shader does not need air.fc_initializer at run time.
bool foldFunctionConstants(llvm::Module& module);

// Propagates constant stores (the static initialiser's function-constant copies)
// into loads of the written globals.
bool propagateStoredConstants(llvm::Module& module);

// Drops llvm.global_ctors, the static initialiser and the now-unused data
// globals, so the entry cannot reference anything outside .text.
bool pruneStaticInit(llvm::Module& module);

} // namespace air2gfx12
