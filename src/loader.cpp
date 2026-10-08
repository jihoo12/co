#include "loader.h"

#include "lexer.h"
#include "parser.h"

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace co {
namespace {

namespace fs = llvm::sys::fs;
namespace path = llvm::sys::path;

// The .co files directly inside `dir`, sorted so builds are deterministic.
std::vector<std::string> sourceFiles(const std::string &dir) {
  std::vector<std::string> files;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec))
    if (path::extension(it->path()) == ".co" && fs::is_regular_file(it->path()))
      files.push_back(it->path());
  std::sort(files.begin(), files.end());
  return files;
}

// "geom", "net/http": slash-separated identifiers.
bool validImportPath(const std::string &p) {
  if (p.empty() || p.front() == '/' || p.back() == '/')
    return false;
  char prev = '/';
  for (char c : p) {
    bool ident = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (c >= '0' && c <= '9' && prev != '/');
    if (!ident && !(c == '/' && prev != '/'))
      return false;
    prev = c;
  }
  return true;
}

class Loader {
public:
  Loader(Program &prog, Diagnostics &diag) : prog_(prog), diag_(diag) {}

  bool loadMain(const std::string &input) {
    auto main = std::make_unique<Package>();
    main->name = "main";
    Package *pkg = main.get();
    prog_.packages.push_back(std::move(main));

    std::vector<std::string> files;
    std::string dir;
    if (fs::is_directory(input)) {
      dir = input;
      files = sourceFiles(dir);
      if (files.empty())
        return fatal("no .co files in '" + input + "'");
    } else {
      dir = path::parent_path(input).str();
      if (dir.empty())
        dir = ".";
      files.push_back(input);
    }
    root_ = findRoot(dir);
    for (auto &f : files)
      if (!loadFile(f, pkg))
        return false;
    return true;
  }

private:
  Program &prog_;
  Diagnostics &diag_;
  std::string root_;
  std::unordered_map<std::string, Package *> byPath_;
  std::unordered_set<Package *> loading_; // packages whose imports are being loaded

  bool fatal(const std::string &msg) {
    fprintf(stderr, "coc: %s\n", msg.c_str());
    return false;
  }

  static std::string findRoot(const std::string &dir) {
    llvm::SmallString<256> abs(dir);
    fs::make_absolute(abs);
    for (llvm::StringRef d = abs; !d.empty(); d = path::parent_path(d)) {
      llvm::SmallString<256> mod(d);
      path::append(mod, "co.mod");
      if (fs::exists(mod))
        return d.str();
    }
    return dir;
  }

  bool loadFile(const std::string &file, Package *pkg) {
    auto buf = llvm::MemoryBuffer::getFile(file, /*IsText=*/true);
    if (!buf)
      return fatal("cannot open '" + file + "': " + buf.getError().message());
    std::string src = (*buf)->getBuffer().str();
    int id = diag_.addFile(file, src);
    prog_.files.push_back(std::make_unique<SourceFile>());
    SourceFile *sf = prog_.files.back().get();
    sf->path = file;
    sf->pkg = pkg;
    parseFile(lex(src, id, diag_), prog_, *sf, diag_);

    std::unordered_set<std::string> names;
    for (auto &imp : sf->imports) {
      if (!validImportPath(imp.path)) {
        diag_.error(imp.loc, "invalid import path \"" + imp.path + "\" (expected names separated by '/', like \"geom\")");
        continue;
      }
      if (imp.name.empty())
        imp.name = path::filename(imp.path, path::Style::posix).str();
      if (!names.insert(imp.name).second) {
        diag_.error(imp.loc, "'" + imp.name + "' is imported twice");
        continue;
      }
      imp.pkg = loadPackage(imp);
    }
    return true;
  }

  Package *loadPackage(const Import &imp) {
    auto it = byPath_.find(imp.path);
    if (it != byPath_.end()) {
      if (loading_.count(it->second)) {
        diag_.error(imp.loc, "import cycle: package \"" + imp.path + "\" imports itself (directly or indirectly)");
        return nullptr;
      }
      return it->second;
    }
    llvm::SmallString<256> dir(root_);
    path::append(dir, path::Style::native, imp.path);
    std::vector<std::string> files;
    if (fs::is_directory(dir))
      files = sourceFiles(std::string(dir));
    if (files.empty()) {
      diag_.error(imp.loc, "cannot find package \"" + imp.path + "\" (no .co files in " + std::string(dir) + ")");
      byPath_[imp.path] = nullptr;
      return nullptr;
    }
    auto owned = std::make_unique<Package>();
    Package *pkg = owned.get();
    pkg->path = imp.path;
    pkg->name = path::filename(imp.path, path::Style::posix).str();
    prog_.packages.push_back(std::move(owned));
    byPath_[imp.path] = pkg;
    loading_.insert(pkg);
    for (auto &f : files)
      if (!loadFile(f, pkg))
        break;
    loading_.erase(pkg);
    return pkg;
  }
};

} // namespace

std::unique_ptr<Program> loadProgram(const std::string &input, Diagnostics &diag) {
  auto prog = std::make_unique<Program>();
  Loader loader(*prog, diag);
  if (!loader.loadMain(input))
    return nullptr;
  return prog;
}

} // namespace co
