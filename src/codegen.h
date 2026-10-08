#pragma once
#include "mir.h"

#include <string>

namespace co {

struct CodegenOptions {
  int optLevel = 2;
  std::string llvmIrPath; // if set, also write textual LLVM IR here
};

// Lowers MIR to LLVM IR and writes a native object file.
bool emitObject(const mir::Module &m, const std::string &objPath, const CodegenOptions &opts,
                std::string &error);

} // namespace co
