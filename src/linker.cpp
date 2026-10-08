#include "linker.h"

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdlib>
#include <sstream>
#include <vector>

#if CO_HAVE_LLD && defined(__linux__) && defined(__x86_64__)
#define CO_BUILTIN_LINKER 1
#include <elf.h>
#include <link.h>
#include <lld/Common/Driver.h>
LLD_HAS_DRIVER(elf)
#endif

namespace co {

#if CO_BUILTIN_LINKER

// Replaces crt1.o: hands main to glibc's __libc_start_main, as glibc's own
// x86-64 start.S does. init/fini are null (glibc 2.34+ runs constructors itself).
static const char *startAsm = R"(
	.text
	.globl _start
	.type _start,@function
_start:
	xorl %ebp, %ebp
	movq %rdx, %r9
	popq %rsi
	movq %rsp, %rdx
	andq $-16, %rsp
	pushq %rax
	pushq %rsp
	xorl %r8d, %r8d
	xorl %ecx, %ecx
	movq main@GOTPCREL(%rip), %rdi
	call *__libc_start_main@GOTPCREL(%rip)
	hlt
	.size _start, .-_start
)";

// coc is itself dynamically linked against the system C library, so the
// loader already knows where it is: programs use the same libc.so.6 and the
// same dynamic loader (coc's PT_INTERP).
static int findLibc(struct dl_phdr_info *info, size_t, void *data) {
  auto *plan = static_cast<LinkPlan *>(data);
  for (int i = 0; i < info->dlpi_phnum; i++)
    if (info->dlpi_phdr[i].p_type == PT_INTERP && plan->interp.empty())
      plan->interp = reinterpret_cast<const char *>(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
  llvm::StringRef name(info->dlpi_name ? info->dlpi_name : "");
  if (llvm::sys::path::filename(name) == "libc.so.6" && llvm::sys::path::is_absolute(name))
    plan->libc = name.str();
  return 0;
}

#endif

LinkPlan planLink() {
  LinkPlan plan;
#if CO_BUILTIN_LINKER
  if (getenv("CO_CC") || getenv("CO_LDFLAGS"))
    return plan;
  dl_iterate_phdr(findLibc, &plan);
  if (!plan.libc.empty() && !plan.interp.empty() && llvm::sys::fs::exists(plan.libc) &&
      llvm::sys::fs::exists(plan.interp)) {
    plan.builtin = true;
    plan.startAsm = startAsm;
  }
#endif
  return plan;
}

static bool linkWithCC(const std::string &obj, const std::string &out, std::string &error) {
  const char *ccEnv = getenv("CO_CC");
  std::string ccName = ccEnv ? ccEnv : CO_DEFAULT_CC;
  auto cc = llvm::sys::findProgramByName(ccName);
  if (!cc) {
    error = "cannot find C compiler '" + ccName + "' for linking (set CO_CC)";
    return false;
  }
  std::vector<std::string> args = {*cc, obj, "-o", out, "-lm"};
  if (const char *extra = getenv("CO_LDFLAGS")) {
    std::istringstream flags(extra);
    for (std::string f; flags >> f;)
      args.push_back(f);
  }
  std::vector<llvm::StringRef> refs(args.begin(), args.end());
  std::string err;
  int rc = llvm::sys::ExecuteAndWait(*cc, refs, std::nullopt, {}, 0, 0, &err);
  if (rc != 0) {
    error = rc < 0 ? "failed to run " + *cc + ": " + err : "linking failed";
    return false;
  }
  return true;
}

bool link(const LinkPlan &plan, const std::string &obj, const std::string &out, std::string &error) {
  if (!plan.builtin)
    return linkWithCC(obj, out, error);
#if CO_BUILTIN_LINKER
  std::vector<const char *> args = {"ld.lld", "-pie", "-z", "relro", "-z", "now", "--eh-frame-hdr",
                                    "--hash-style=gnu", "--as-needed", "--dynamic-linker", plan.interp.c_str(),
                                    "-o", out.c_str(), obj.c_str(), plan.libc.c_str()};
  std::string msgs;
  llvm::raw_string_ostream os(msgs);
  lld::Result r = lld::lldMain(args, os, os, {{lld::Gnu, &lld::elf::link}});
  if (r.retCode != 0) {
    error = "linking failed:\n" + msgs;
    return false;
  }
  return true;
#else
  return false;
#endif
}

} // namespace co
