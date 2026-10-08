// coc: the co compiler driver.
#include "borrowck.h"
#include "codegen.h"
#include "diag.h"
#include "lexer.h"
#include "linker.h"
#include "mir_build.h"
#include "parser.h"
#include "sema.h"

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace co;

static void usage() {
  fprintf(stderr,
          "usage: coc <command> <file.co> [options]\n"
          "\n"
          "commands:\n"
          "  build   compile to an executable\n"
          "  run     compile and run (extra arguments after the file are ignored for now)\n"
          "  check   type-check and borrow-check only\n"
          "\n"
          "options:\n"
          "  -o <path>      output executable (default: file name without .co)\n"
          "  -O0 .. -O3     optimization level (default -O2)\n"
          "  --emit-llvm    also write <output>.ll\n"
          "  --emit-mir     print the MIR to stdout\n");
}

static int runProgram(const std::string &prog, const std::vector<std::string> &args) {
  std::vector<llvm::StringRef> refs;
  refs.push_back(prog);
  for (auto &a : args)
    refs.push_back(a);
  std::string err;
  int rc = llvm::sys::ExecuteAndWait(prog, refs, std::nullopt, {}, 0, 0, &err);
  if (rc < 0)
    fprintf(stderr, "coc: failed to run %s: %s\n", prog.c_str(), err.c_str());
  return rc;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    usage();
    return 2;
  }
  std::string cmd = argv[1];
  if (cmd != "build" && cmd != "run" && cmd != "check") {
    usage();
    return 2;
  }
  std::string input = argv[2];
  std::string output;
  CodegenOptions opts;
  bool emitLLVM = false, emitMir = false;
  for (int i = 3; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-o" && i + 1 < argc)
      output = argv[++i];
    else if (a.size() == 3 && a[0] == '-' && a[1] == 'O' && a[2] >= '0' && a[2] <= '3')
      opts.optLevel = a[2] - '0';
    else if (a == "--emit-llvm")
      emitLLVM = true;
    else if (a == "--emit-mir")
      emitMir = true;
    else {
      fprintf(stderr, "coc: unknown option '%s'\n", a.c_str());
      return 2;
    }
  }

  std::ifstream in(input, std::ios::binary);
  if (!in) {
    fprintf(stderr, "coc: cannot open '%s'\n", input.c_str());
    return 1;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  std::string src = ss.str();

  Diagnostics diag(input, src);
  auto toks = lex(src, diag);
  auto prog = parse(toks, diag);
  if (diag.hasErrors()) {
    diag.print();
    return 1;
  }
  TypeContext tc;
  analyze(*prog, tc, diag);
  if (diag.hasErrors()) {
    diag.print();
    return 1;
  }
  mir::Module mod = buildMir(*prog, tc, diag);
  if (!diag.hasErrors())
    for (auto &f : mod.funcs)
      borrowCheck(f, diag);
  if (emitMir) {
    std::string out;
    for (auto &f : mod.funcs)
      mir::print(f, out);
    fputs(out.c_str(), stdout);
  }
  if (diag.hasErrors()) {
    diag.print();
    return 1;
  }
  if (cmd == "check")
    return 0;

  bool temporaryExe = false;
  if (output.empty()) {
    if (cmd == "run") {
      llvm::SmallString<128> tmp;
      if (llvm::sys::fs::createTemporaryFile("co-run", "", tmp)) {
        fprintf(stderr, "coc: cannot create temporary file\n");
        return 1;
      }
      output = std::string(tmp);
      temporaryExe = true;
    } else {
      llvm::SmallString<128> p(input);
      llvm::sys::path::replace_extension(p, "");
      output = std::string(p);
      if (output == input)
        output += ".out";
    }
  }
  if (emitLLVM)
    opts.llvmIrPath = output + ".ll";
  LinkPlan plan = planLink();
  opts.startAsm = plan.startAsm;

  llvm::SmallString<128> obj;
  if (llvm::sys::fs::createTemporaryFile("co", "o", obj)) {
    fprintf(stderr, "coc: cannot create temporary file\n");
    return 1;
  }
  std::string err;
  if (!emitObject(mod, std::string(obj), opts, err)) {
    fprintf(stderr, "coc: %s\n", err.c_str());
    llvm::sys::fs::remove(obj);
    return 1;
  }

  bool linked = link(plan, std::string(obj), output, err);
  llvm::sys::fs::remove(obj);
  if (!linked) {
    fprintf(stderr, "coc: %s\n", err.c_str());
    return 1;
  }

  if (cmd == "run") {
    int code = runProgram(output, {});
    if (temporaryExe)
      llvm::sys::fs::remove(output);
    return code;
  }
  return 0;
}
