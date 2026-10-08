#pragma once
#include "ast.h"

namespace co {

// Resolves names and types, annotates the AST, and inserts implicit
// borrows/reborrows (auto-ref for method receivers, builtins, string ops).
void analyze(Program &prog, TypeContext &tc, Diagnostics &diag);

} // namespace co
