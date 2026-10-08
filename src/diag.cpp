#include "diag.h"

#include <cstdio>
#include <unistd.h>

namespace co {

static bool useColor() { return isatty(fileno(stderr)); }

int Diagnostics::addFile(std::string filename, const std::string &source) {
  File f{std::move(filename), {}};
  std::string cur;
  for (char c : source) {
    if (c == '\n') {
      f.lines.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  f.lines.push_back(cur);
  files_.push_back(std::move(f));
  return (int)files_.size() - 1;
}

const Diagnostics::File *Diagnostics::file(SourceLoc loc) const {
  return loc.file >= 0 && loc.file < (int)files_.size() ? &files_[loc.file] : nullptr;
}

void Diagnostics::error(SourceLoc loc, std::string message, std::vector<Note> notes) {
  // Avoid repeating the exact same error at the same location.
  for (auto &d : diags_)
    if (d.loc.line == loc.line && d.loc.col == loc.col && d.loc.file == loc.file && d.message == message)
      return;
  diags_.push_back({loc, std::move(message), std::move(notes)});
}

void Diagnostics::printExcerpt(SourceLoc loc) const {
  const File *f = file(loc);
  if (!f || loc.line <= 0 || loc.line > (int)f->lines.size())
    return;
  const std::string &text = f->lines[loc.line - 1];
  fprintf(stderr, "%5d | %s\n", loc.line, text.c_str());
  std::string caret;
  for (int i = 1; i < loc.col && i <= (int)text.size(); i++)
    caret += text[i - 1] == '\t' ? '\t' : ' ';
  fprintf(stderr, "      | %s%s^%s\n", caret.c_str(), useColor() ? "\x1b[1;32m" : "",
          useColor() ? "\x1b[0m" : "");
}

void Diagnostics::print() const {
  bool color = useColor();
  auto name = [&](SourceLoc l) { return file(l) ? file(l)->name.c_str() : "<unknown>"; };
  for (auto &d : diags_) {
    fprintf(stderr, "%s%s:%d:%d: %serror:%s %s%s\n", color ? "\x1b[1m" : "", name(d.loc),
            d.loc.line, d.loc.col, color ? "\x1b[1;31m" : "", color ? "\x1b[0;1m" : "",
            d.message.c_str(), color ? "\x1b[0m" : "");
    printExcerpt(d.loc);
    for (auto &n : d.notes) {
      fprintf(stderr, "%s:%d:%d: %snote:%s %s\n", name(n.loc), n.loc.line, n.loc.col,
              color ? "\x1b[1;36m" : "", color ? "\x1b[0m" : "", n.message.c_str());
      printExcerpt(n.loc);
    }
  }
  if (!diags_.empty())
    fprintf(stderr, "%zu error%s generated.\n", diags_.size(), diags_.size() == 1 ? "" : "s");
}

} // namespace co
