#pragma once
#include "ast.h"
#include "lexer.h"

namespace co {

// Parses one source file, adding its declarations to `prog` and its imports to `file`.
void parseFile(const std::vector<Token> &toks, Program &prog, SourceFile &file, Diagnostics &diag);

} // namespace co
