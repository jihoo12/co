#pragma once
#include "diag.h"

#include <cstdint>
#include <string>
#include <vector>

namespace co {

enum class Tok {
  Eof,
  Ident,
  Int,
  Float,
  String,
  // keywords
  KwFunc,
  KwType,
  KwStruct,
  KwVar,
  KwReturn,
  KwIf,
  KwElse,
  KwFor,
  KwRange,
  KwBreak,
  KwContinue,
  KwTrue,
  KwFalse,
  KwMut,
  KwEnum,
  KwSwitch,
  KwCase,
  KwDefault,
  KwNone,
  KwOr,
  KwTry,
  KwMap,
  KwImport,
  KwExtern,
  KwNil,
  // punctuation
  LParen,
  RParen,
  LBrace,
  RBrace,
  LBracket,
  RBracket,
  Comma,
  Dot,
  Ellipsis,
  Colon,
  Semi,
  Question,
  // operators
  Plus,
  Minus,
  Star,
  Slash,
  Percent,
  Amp,
  AndAnd,
  OrOr,
  Not,
  Eq,
  Ne,
  Lt,
  Le,
  Gt,
  Ge,
  Assign,
  Define, // :=
  PlusAssign,
  MinusAssign,
  StarAssign,
  SlashAssign,
  PercentAssign,
  PlusPlus,
  MinusMinus,
};

const char *tokName(Tok t);

struct Token {
  Tok kind;
  std::string text; // identifier name or decoded string literal
  int64_t intVal = 0;
  double floatVal = 0;
  SourceLoc loc;
};

// Tokenizes source code. Like Go, a semicolon is inserted automatically at a
// newline when the line ends with a token that can end a statement.
// `file` is the source file index stamped into every token location.
std::vector<Token> lex(const std::string &src, int file, Diagnostics &diag);

} // namespace co
