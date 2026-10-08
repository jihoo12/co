#include "diag.h"

#include <cstdio>
#include <unistd.h>

namespace co {

static bool useColor() { return isatty(fileno(stderr)); }

Diagnostics::Diagnostics(std::string filename, const std::string &source)
    : filename_(std::move(filename)) {
  std::string cur;
  for (char c : source) {
    if (c == '\n') {
      lines_.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  lines_.push_back(cur);
}

void Diagnostics::error(SourceLoc loc, std::string message, std::vector<Note> notes) {
  // Avoid repeating the exact same error at the same location.
  for (auto &d : diags_)
    if (d.loc.line == loc.line && d.loc.col == loc.col && d.message == message)
      return;
  diags_.push_back({loc, std::move(message), std::move(notes)});
}

void Diagnostics::printExcerpt(SourceLoc loc) const {
  if (loc.line <= 0 || loc.line > (int)lines_.size())
    return;
  const std::string &text = lines_[loc.line - 1];
  fprintf(stderr, "%5d | %s\n", loc.line, text.c_str());
  std::string caret;
  for (int i = 1; i < loc.col && i <= (int)text.size(); i++)
    caret += text[i - 1] == '\t' ? '\t' : ' ';
  fprintf(stderr, "      | %s%s^%s\n", caret.c_str(), useColor() ? "\x1b[1;32m" : "",
          useColor() ? "\x1b[0m" : "");
}

void Diagnostics::print() const {
  bool color = useColor();
  for (auto &d : diags_) {
    fprintf(stderr, "%s%s:%d:%d: %serror:%s %s%s\n", color ? "\x1b[1m" : "", filename_.c_str(),
            d.loc.line, d.loc.col, color ? "\x1b[1;31m" : "", color ? "\x1b[0;1m" : "",
            d.message.c_str(), color ? "\x1b[0m" : "");
    printExcerpt(d.loc);
    for (auto &n : d.notes) {
      fprintf(stderr, "%s:%d:%d: %snote:%s %s\n", filename_.c_str(), n.loc.line, n.loc.col,
              color ? "\x1b[1;36m" : "", color ? "\x1b[0m" : "", n.message.c_str());
      printExcerpt(n.loc);
    }
  }
  if (!diags_.empty())
    fprintf(stderr, "%zu error%s generated.\n", diags_.size(), diags_.size() == 1 ? "" : "s");
}

} // namespace co
