#pragma once
#include <string>
#include <vector>

namespace co {

struct SourceLoc {
  int line = 0;
  int col = 0;
};

struct Note {
  SourceLoc loc;
  std::string message;
};

struct Diagnostic {
  SourceLoc loc;
  std::string message;
  std::vector<Note> notes;
};

// Collects and prints compiler errors with source excerpts.
class Diagnostics {
public:
  Diagnostics(std::string filename, const std::string &source);

  void error(SourceLoc loc, std::string message, std::vector<Note> notes = {});
  bool hasErrors() const { return !diags_.empty(); }
  size_t count() const { return diags_.size(); }
  void print() const;

private:
  void printExcerpt(SourceLoc loc) const;

  std::string filename_;
  std::vector<std::string> lines_;
  std::vector<Diagnostic> diags_;
};

} // namespace co
