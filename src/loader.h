#pragma once
#include "ast.h"
#include "diag.h"

#include <memory>
#include <string>

namespace co {

// Reads and parses the program rooted at `input`: the main package (a single
// .co file, or every .co file in a directory) and, transitively, every package
// it imports. `import "a/b"` names the directory a/b under the project root:
// the nearest directory, starting at the main package's, that contains a
// co.mod file, or else the main package's directory.
std::unique_ptr<Program> loadProgram(const std::string &input, Diagnostics &diag);

} // namespace co
