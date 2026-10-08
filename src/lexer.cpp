#include "lexer.h"

#include <cctype>
#include <unordered_map>

namespace co {

const char *tokName(Tok t) {
  switch (t) {
  case Tok::Eof: return "end of file";
  case Tok::Ident: return "identifier";
  case Tok::Int: return "integer literal";
  case Tok::Float: return "float literal";
  case Tok::String: return "string literal";
  case Tok::KwFunc: return "'func'";
  case Tok::KwType: return "'type'";
  case Tok::KwStruct: return "'struct'";
  case Tok::KwVar: return "'var'";
  case Tok::KwReturn: return "'return'";
  case Tok::KwIf: return "'if'";
  case Tok::KwElse: return "'else'";
  case Tok::KwFor: return "'for'";
  case Tok::KwRange: return "'range'";
  case Tok::KwBreak: return "'break'";
  case Tok::KwContinue: return "'continue'";
  case Tok::KwTrue: return "'true'";
  case Tok::KwFalse: return "'false'";
  case Tok::KwMut: return "'mut'";
  case Tok::KwEnum: return "'enum'";
  case Tok::KwSwitch: return "'switch'";
  case Tok::KwCase: return "'case'";
  case Tok::KwDefault: return "'default'";
  case Tok::KwNone: return "'none'";
  case Tok::KwOr: return "'or'";
  case Tok::KwTry: return "'try'";
  case Tok::KwMap: return "'map'";
  case Tok::KwImport: return "'import'";
  case Tok::KwExtern: return "'extern'";
  case Tok::KwNil: return "'nil'";
  case Tok::Question: return "'?'";
  case Tok::LParen: return "'('";
  case Tok::RParen: return "')'";
  case Tok::LBrace: return "'{'";
  case Tok::RBrace: return "'}'";
  case Tok::LBracket: return "'['";
  case Tok::RBracket: return "']'";
  case Tok::Comma: return "','";
  case Tok::Dot: return "'.'";
  case Tok::Ellipsis: return "'...'";
  case Tok::Colon: return "':'";
  case Tok::Semi: return "';' or newline";
  case Tok::Plus: return "'+'";
  case Tok::Minus: return "'-'";
  case Tok::Star: return "'*'";
  case Tok::Slash: return "'/'";
  case Tok::Percent: return "'%'";
  case Tok::Amp: return "'&'";
  case Tok::AndAnd: return "'&&'";
  case Tok::OrOr: return "'||'";
  case Tok::Not: return "'!'";
  case Tok::Eq: return "'=='";
  case Tok::Ne: return "'!='";
  case Tok::Lt: return "'<'";
  case Tok::Le: return "'<='";
  case Tok::Gt: return "'>'";
  case Tok::Ge: return "'>='";
  case Tok::Assign: return "'='";
  case Tok::Define: return "':='";
  case Tok::PlusAssign: return "'+='";
  case Tok::MinusAssign: return "'-='";
  case Tok::StarAssign: return "'*='";
  case Tok::SlashAssign: return "'/='";
  case Tok::PercentAssign: return "'%='";
  case Tok::PlusPlus: return "'++'";
  case Tok::MinusMinus: return "'--'";
  }
  return "?";
}

static bool endsStatement(Tok t) {
  switch (t) {
  case Tok::Ident:
  case Tok::Int:
  case Tok::Float:
  case Tok::String:
  case Tok::KwReturn:
  case Tok::KwBreak:
  case Tok::KwContinue:
  case Tok::KwTrue:
  case Tok::KwFalse:
  case Tok::KwNone:
  case Tok::KwNil:
  case Tok::RParen:
  case Tok::RBracket:
  case Tok::RBrace:
  case Tok::PlusPlus:
  case Tok::MinusMinus:
    return true;
  default:
    return false;
  }
}

std::vector<Token> lex(const std::string &src, int file, Diagnostics &diag) {
  static const std::unordered_map<std::string, Tok> keywords = {
      {"func", Tok::KwFunc},     {"type", Tok::KwType},   {"struct", Tok::KwStruct},
      {"var", Tok::KwVar},       {"return", Tok::KwReturn}, {"if", Tok::KwIf},
      {"else", Tok::KwElse},     {"for", Tok::KwFor},     {"range", Tok::KwRange},
      {"break", Tok::KwBreak},   {"continue", Tok::KwContinue}, {"true", Tok::KwTrue},
      {"false", Tok::KwFalse},   {"mut", Tok::KwMut},
      {"enum", Tok::KwEnum},     {"switch", Tok::KwSwitch}, {"case", Tok::KwCase},
      {"default", Tok::KwDefault}, {"none", Tok::KwNone},  {"or", Tok::KwOr},
      {"try", Tok::KwTry},       {"map", Tok::KwMap},     {"import", Tok::KwImport},
      {"extern", Tok::KwExtern}, {"nil", Tok::KwNil},
  };

  std::vector<Token> toks;
  size_t i = 0;
  int line = 1, col = 1;

  auto peek = [&](size_t off = 0) -> char { return i + off < src.size() ? src[i + off] : '\0'; };
  auto advance = [&]() {
    if (src[i] == '\n') {
      line++;
      col = 1;
    } else {
      col++;
    }
    i++;
  };
  auto newline = [&]() {
    if (!toks.empty() && endsStatement(toks.back().kind))
      toks.push_back({Tok::Semi, "\n", 0, 0, {line, col, file}});
  };

  while (i < src.size()) {
    char c = peek();
    if (c == '\n') {
      newline();
      advance();
      continue;
    }
    if (isspace((unsigned char)c)) {
      advance();
      continue;
    }
    if (c == '/' && peek(1) == '/') {
      while (i < src.size() && peek() != '\n')
        advance();
      continue;
    }
    if (c == '/' && peek(1) == '*') {
      SourceLoc start{line, col, file};
      advance();
      advance();
      bool sawNewline = false;
      while (i < src.size() && !(peek() == '*' && peek(1) == '/')) {
        if (peek() == '\n')
          sawNewline = true;
        advance();
      }
      if (i >= src.size()) {
        diag.error(start, "unterminated block comment");
        break;
      }
      advance();
      advance();
      if (sawNewline)
        newline();
      continue;
    }

    SourceLoc loc{line, col, file};
    Token tok{Tok::Eof, "", 0, 0, loc};

    if (isalpha((unsigned char)c) || c == '_') {
      std::string id;
      while (isalnum((unsigned char)peek()) || peek() == '_') {
        id += peek();
        advance();
      }
      auto it = keywords.find(id);
      tok.kind = it != keywords.end() ? it->second : Tok::Ident;
      tok.text = id;
      toks.push_back(tok);
      continue;
    }

    if (isdigit((unsigned char)c)) {
      std::string num;
      bool isFloat = false;
      while (isdigit((unsigned char)peek()) || peek() == '_') {
        if (peek() != '_')
          num += peek();
        advance();
      }
      if (peek() == '.' && isdigit((unsigned char)peek(1))) {
        isFloat = true;
        num += '.';
        advance();
        while (isdigit((unsigned char)peek())) {
          num += peek();
          advance();
        }
      }
      if (peek() == 'e' || peek() == 'E') {
        isFloat = true;
        num += 'e';
        advance();
        if (peek() == '+' || peek() == '-') {
          num += peek();
          advance();
        }
        while (isdigit((unsigned char)peek())) {
          num += peek();
          advance();
        }
      }
      if (isFloat) {
        tok.kind = Tok::Float;
        tok.floatVal = std::stod(num);
      } else {
        tok.kind = Tok::Int;
        try {
          tok.intVal = (int64_t)std::stoull(num);
        } catch (...) {
          diag.error(loc, "integer literal is too large");
        }
      }
      tok.text = num;
      toks.push_back(tok);
      continue;
    }

    if (c == '"') {
      advance();
      std::string s;
      bool closed = false;
      while (i < src.size() && peek() != '\n') {
        char ch = peek();
        if (ch == '"') {
          advance();
          closed = true;
          break;
        }
        if (ch == '\\') {
          advance();
          char e = peek();
          switch (e) {
          case 'n': s += '\n'; break;
          case 't': s += '\t'; break;
          case 'r': s += '\r'; break;
          case '0': s += '\0'; break;
          case '\\': s += '\\'; break;
          case '"': s += '"'; break;
          default:
            diag.error({line, col, file}, std::string("unknown escape sequence '\\") + e + "'");
          }
          advance();
          continue;
        }
        s += ch;
        advance();
      }
      if (!closed)
        diag.error(loc, "unterminated string literal");
      tok.kind = Tok::String;
      tok.text = s;
      toks.push_back(tok);
      continue;
    }

    auto two = [&](char second, Tok yes, Tok no) {
      advance();
      if (peek() == second) {
        advance();
        return yes;
      }
      return no;
    };

    switch (c) {
    case '(': advance(); tok.kind = Tok::LParen; break;
    case ')': advance(); tok.kind = Tok::RParen; break;
    case '{': advance(); tok.kind = Tok::LBrace; break;
    case '}': advance(); tok.kind = Tok::RBrace; break;
    case '[': advance(); tok.kind = Tok::LBracket; break;
    case ']': advance(); tok.kind = Tok::RBracket; break;
    case ',': advance(); tok.kind = Tok::Comma; break;
    case '.':
      advance();
      tok.kind = Tok::Dot;
      if (peek() == '.' && peek(1) == '.') {
        advance();
        advance();
        tok.kind = Tok::Ellipsis;
      }
      break;
    case ';': advance(); tok.kind = Tok::Semi; break;
    case '?': advance(); tok.kind = Tok::Question; break;
    case ':': tok.kind = two('=', Tok::Define, Tok::Colon); break;
    case '=': tok.kind = two('=', Tok::Eq, Tok::Assign); break;
    case '!': tok.kind = two('=', Tok::Ne, Tok::Not); break;
    case '<': tok.kind = two('=', Tok::Le, Tok::Lt); break;
    case '>': tok.kind = two('=', Tok::Ge, Tok::Gt); break;
    case '*': tok.kind = two('=', Tok::StarAssign, Tok::Star); break;
    case '/': tok.kind = two('=', Tok::SlashAssign, Tok::Slash); break;
    case '%': tok.kind = two('=', Tok::PercentAssign, Tok::Percent); break;
    case '&': tok.kind = two('&', Tok::AndAnd, Tok::Amp); break;
    case '|':
      advance();
      if (peek() == '|') {
        advance();
        tok.kind = Tok::OrOr;
      } else {
        diag.error(loc, "unexpected character '|'");
        continue;
      }
      break;
    case '+':
      advance();
      if (peek() == '+') { advance(); tok.kind = Tok::PlusPlus; }
      else if (peek() == '=') { advance(); tok.kind = Tok::PlusAssign; }
      else tok.kind = Tok::Plus;
      break;
    case '-':
      advance();
      if (peek() == '-') { advance(); tok.kind = Tok::MinusMinus; }
      else if (peek() == '=') { advance(); tok.kind = Tok::MinusAssign; }
      else tok.kind = Tok::Minus;
      break;
    default:
      diag.error(loc, std::string("unexpected character '") + c + "'");
      advance();
      continue;
    }
    toks.push_back(tok);
  }
  newline();
  toks.push_back({Tok::Eof, "", 0, 0, {line, col, file}});
  return toks;
}

} // namespace co
