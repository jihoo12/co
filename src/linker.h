#pragma once
#include <string>
#include <vector>

namespace co {

// How programs get linked on this host. Where possible coc links them itself
// with the lld library against the C library it is running with, so building
// needs no C toolchain. Otherwise (other platforms, coc built without lld, or
// $CO_CC / $CO_LDFLAGS set) it runs the system C compiler driver.
struct LinkPlan {
  bool builtin = false;
  std::string libc;     // builtin: path of libc.so.6
  std::string interp;   // builtin: the dynamic loader
  std::string startAsm; // builtin: module asm defining _start, to add to the program
};

LinkPlan planLink();

// C libraries to link with: `libs` (from `extern "lib"` blocks, as for -l),
// searched for in `dirs` first. Programs also find them there at run time.
struct LinkLibs {
  std::vector<std::string> libs;
  std::vector<std::string> dirs;
};

// Links object file `obj` into executable `out`.
bool link(const LinkPlan &plan, const std::string &obj, const std::string &out, const LinkLibs &libs,
          std::string &error);

} // namespace co
