#pragma once
#include "ast.h"
#include "lexer.h"

namespace co {

std::unique_ptr<Program> parse(const std::vector<Token> &toks, Diagnostics &diag);

} // namespace co
