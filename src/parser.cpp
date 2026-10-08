#include "parser.h"

namespace co {
namespace {

struct ParseError {};

class Parser {
public:
  Parser(const std::vector<Token> &toks, Diagnostics &diag) : toks_(toks), diag_(diag) {}

  std::unique_ptr<Program> parseProgram() {
    auto prog = std::make_unique<Program>();
    while (!at(Tok::Eof)) {
      try {
        if (accept(Tok::Semi))
          continue;
        if (at(Tok::KwType))
          parseTypeDecl(*prog);
        else if (at(Tok::KwFunc))
          prog->funcs.push_back(parseFuncDecl());
        else
          fail("expected 'func' or 'type' declaration, found " + describe(cur()));
      } catch (ParseError &) {
        recoverTopLevel();
      }
    }
    return prog;
  }

private:
  const std::vector<Token> &toks_;
  Diagnostics &diag_;
  size_t pos_ = 0;
  bool noStructLit_ = false; // inside if/for headers, `x {` starts the block

  const Token &cur() const { return toks_[pos_]; }
  const Token &peekTok(size_t n = 1) const {
    return toks_[std::min(pos_ + n, toks_.size() - 1)];
  }
  bool at(Tok k) const { return cur().kind == k; }
  SourceLoc loc() const { return cur().loc; }
  const Token &next() {
    const Token &t = toks_[pos_];
    if (pos_ + 1 < toks_.size())
      pos_++;
    return t;
  }
  bool accept(Tok k) {
    if (at(k)) {
      next();
      return true;
    }
    return false;
  }
  static std::string describe(const Token &t) {
    if (t.kind == Tok::Ident)
      return "identifier '" + t.text + "'";
    if (t.kind == Tok::Semi && t.text == "\n")
      return "newline";
    return tokName(t.kind);
  }
  [[noreturn]] void fail(const std::string &msg) {
    diag_.error(loc(), msg);
    throw ParseError{};
  }
  const Token &expect(Tok k, const char *context = nullptr) {
    if (!at(k)) {
      std::string msg = std::string("expected ") + tokName(k);
      if (context)
        msg += std::string(" ") + context;
      msg += ", found " + describe(cur());
      fail(msg);
    }
    return next();
  }
  void skipSemis() {
    while (accept(Tok::Semi)) {
    }
  }
  void recoverTopLevel() {
    while (!at(Tok::Eof)) {
      if ((at(Tok::KwFunc) || at(Tok::KwType)) && pos_ > 0 && toks_[pos_ - 1].kind == Tok::Semi)
        return;
      next();
    }
  }

  // ----- types -----

  TypeExprPtr parseType() {
    auto t = std::make_unique<TypeExpr>();
    t->loc = loc();
    if (accept(Tok::AndAnd)) {
      // `&&T` is lexed as one token; it means a reference to a reference.
      t->kind = TypeExpr::Ref;
      auto inner = std::make_unique<TypeExpr>();
      inner->loc = t->loc;
      inner->kind = TypeExpr::Ref;
      inner->mut = accept(Tok::KwMut);
      inner->inner = parseType();
      t->inner = std::move(inner);
      return t;
    }
    if (accept(Tok::Amp)) {
      t->kind = TypeExpr::Ref;
      t->mut = accept(Tok::KwMut);
      t->inner = parseType();
      return t;
    }
    if (accept(Tok::Question)) {
      t->kind = TypeExpr::Optional;
      t->inner = parseType();
      return t;
    }
    if (accept(Tok::LBracket)) {
      expect(Tok::RBracket, "in slice type");
      t->kind = TypeExpr::Slice;
      t->inner = parseType();
      return t;
    }
    if (at(Tok::Ident)) {
      t->kind = TypeExpr::Name;
      t->name = next().text;
      return t;
    }
    fail("expected a type, found " + describe(cur()));
  }

  // ----- declarations -----

  void parseTypeDecl(Program &prog) {
    expect(Tok::KwType);
    SourceLoc l = loc();
    std::string name = expect(Tok::Ident, "after 'type'").text;
    if (at(Tok::KwEnum)) {
      prog.enums.push_back(parseEnumBody(l, name));
      return;
    }
    if (!at(Tok::KwStruct))
      fail("expected 'struct' or 'enum' after the type name");
    prog.structs.push_back(parseStructBody(l, name));
  }

  std::unique_ptr<EnumDecl> parseEnumBody(SourceLoc l, const std::string &name) {
    expect(Tok::KwEnum);
    auto ed = std::make_unique<EnumDecl>();
    ed->loc = l;
    ed->name = name;
    expect(Tok::LBrace);
    skipSemis();
    while (!at(Tok::RBrace)) {
      EnumVariantDecl v;
      v.loc = loc();
      v.name = expect(Tok::Ident, "for variant name").text;
      if (accept(Tok::LParen)) {
        v.fields = parseVariantFields();
        expect(Tok::RParen, "after variant fields");
      }
      ed->variants.push_back(std::move(v));
      if (!at(Tok::RBrace) && !accept(Tok::Comma))
        expect(Tok::Semi, "after enum variant");
      skipSemis();
    }
    expect(Tok::RBrace);
    return ed;
  }

  // Variant payloads may be written as types `(float, int)` or with names
  // for documentation `(w, h float)`; only the types matter.
  std::vector<TypeExprPtr> parseVariantFields() {
    std::vector<TypeExprPtr> out;
    std::vector<TypeExprPtr> pending; // bare identifiers: names or types
    while (!at(Tok::RParen)) {
      if (at(Tok::Ident) && peekTok(1).kind != Tok::Comma && peekTok(1).kind != Tok::RParen) {
        next(); // a field name
        auto ty = parseType();
        for (size_t i = 0; i < pending.size(); i++)
          out.push_back(cloneType(*ty));
        pending.clear();
        out.push_back(std::move(ty));
      } else if (at(Tok::Ident)) {
        pending.push_back(parseType());
      } else {
        for (auto &p : pending)
          out.push_back(std::move(p));
        pending.clear();
        out.push_back(parseType());
      }
      if (!accept(Tok::Comma))
        break;
    }
    for (auto &p : pending)
      out.push_back(std::move(p));
    return out;
  }

  std::unique_ptr<StructDecl> parseStructBody(SourceLoc l, const std::string &name) {
    auto sd = std::make_unique<StructDecl>();
    sd->loc = l;
    sd->name = name;
    expect(Tok::KwStruct);
    expect(Tok::LBrace);
    skipSemis();
    while (!at(Tok::RBrace)) {
      // Go allows `x, y int`
      std::vector<std::pair<std::string, SourceLoc>> names;
      do {
        SourceLoc l = loc();
        names.push_back({expect(Tok::Ident, "for field name").text, l});
      } while (accept(Tok::Comma));
      TypeExpr *firstType = nullptr;
      for (size_t i = 0; i < names.size(); i++) {
        StructFieldDecl f;
        f.name = names[i].first;
        f.loc = names[i].second;
        if (i == 0) {
          f.type = parseType();
          firstType = f.type.get();
        } else {
          f.type = cloneType(*firstType);
        }
        sd->fields.push_back(std::move(f));
      }
      if (!at(Tok::RBrace))
        expect(Tok::Semi, "after struct field");
      skipSemis();
    }
    expect(Tok::RBrace);
    return sd;
  }

  static TypeExprPtr cloneType(const TypeExpr &t) {
    auto c = std::make_unique<TypeExpr>();
    c->kind = t.kind;
    c->name = t.name;
    c->mut = t.mut;
    c->loc = t.loc;
    if (t.inner)
      c->inner = cloneType(*t.inner);
    return c;
  }

  std::unique_ptr<FuncDecl> parseFuncDecl() {
    expect(Tok::KwFunc);
    auto fd = std::make_unique<FuncDecl>();
    if (accept(Tok::LParen)) {
      Param recv;
      recv.loc = loc();
      recv.name = expect(Tok::Ident, "for receiver name").text;
      recv.type = parseType();
      expect(Tok::RParen, "after receiver");
      fd->receiver = std::move(recv);
    }
    fd->loc = loc();
    fd->name = expect(Tok::Ident, "for function name").text;
    expect(Tok::LParen);
    // Parameters, with Go-style grouping: `a, b int`
    std::vector<std::pair<std::string, SourceLoc>> pending;
    while (!at(Tok::RParen)) {
      SourceLoc l = loc();
      std::string name = expect(Tok::Ident, "for parameter name").text;
      pending.push_back({name, l});
      if (at(Tok::Comma) || at(Tok::RParen)) {
        if (at(Tok::RParen))
          fail("missing type for parameter '" + name + "'");
        next();
        continue;
      }
      auto ty = parseType();
      for (size_t i = 0; i < pending.size(); i++) {
        Param p;
        p.name = pending[i].first;
        p.loc = pending[i].second;
        p.type = i + 1 == pending.size() ? std::move(ty) : cloneType(*ty);
        fd->params.push_back(std::move(p));
      }
      pending.clear();
      if (!accept(Tok::Comma))
        break;
    }
    expect(Tok::RParen, "after parameters");
    if (!at(Tok::LBrace))
      fd->ret = parseType();
    fd->body = parseBlock();
    return fd;
  }

  // ----- statements -----

  std::unique_ptr<BlockStmt> parseBlock() {
    auto b = std::make_unique<BlockStmt>(loc());
    bool saved = noStructLit_;
    noStructLit_ = false;
    expect(Tok::LBrace);
    skipSemis();
    while (!at(Tok::RBrace) && !at(Tok::Eof)) {
      try {
        b->stmts.push_back(parseStmt());
        if (!at(Tok::RBrace))
          expect(Tok::Semi, "after statement");
      } catch (ParseError &) {
        // skip to the end of the statement
        int depth = 0;
        while (!at(Tok::Eof)) {
          if (at(Tok::LBrace))
            depth++;
          if (at(Tok::RBrace)) {
            if (depth == 0)
              break;
            depth--;
          }
          if (at(Tok::Semi) && depth == 0)
            break;
          next();
        }
      }
      skipSemis();
    }
    b->endLoc = loc();
    expect(Tok::RBrace);
    noStructLit_ = saved;
    return b;
  }

  StmtPtr parseStmt() {
    SourceLoc l = loc();
    switch (cur().kind) {
    case Tok::KwVar: {
      next();
      SourceLoc nl = loc();
      auto vd = std::make_unique<VarDeclStmt>(nl, expect(Tok::Ident, "after 'var'").text);
      if (!at(Tok::Assign))
        vd->typeExpr = parseType();
      if (accept(Tok::Assign))
        vd->init = parseExpr();
      return vd;
    }
    case Tok::KwReturn: {
      next();
      auto r = std::make_unique<ReturnStmt>(l);
      if (!at(Tok::Semi) && !at(Tok::RBrace))
        r->value = parseExpr();
      return r;
    }
    case Tok::KwBreak:
      next();
      return std::make_unique<BranchStmt>(StmtKind::Break, l);
    case Tok::KwContinue:
      next();
      return std::make_unique<BranchStmt>(StmtKind::Continue, l);
    case Tok::KwIf:
      return parseIf();
    case Tok::KwFor:
      return parseFor();
    case Tok::KwSwitch:
      return parseSwitch();
    case Tok::LBrace:
      return parseBlock();
    default:
      return parseSimpleStmt();
    }
  }

  StmtPtr parseSimpleStmt() {
    SourceLoc l = loc();
    auto e = parseExpr();
    if (at(Tok::Define)) {
      if (e->kind != ExprKind::Ident)
        fail("left side of ':=' must be a name");
      next();
      auto vd = std::make_unique<VarDeclStmt>(e->loc, static_cast<IdentExpr *>(e.get())->name);
      vd->init = parseExpr();
      return vd;
    }
    AssignOp op;
    switch (cur().kind) {
    case Tok::Assign: op = AssignOp::Set; break;
    case Tok::PlusAssign: op = AssignOp::Add; break;
    case Tok::MinusAssign: op = AssignOp::Sub; break;
    case Tok::StarAssign: op = AssignOp::Mul; break;
    case Tok::SlashAssign: op = AssignOp::Div; break;
    case Tok::PercentAssign: op = AssignOp::Rem; break;
    case Tok::PlusPlus:
      next();
      return std::make_unique<IncDecStmt>(l, std::move(e), true);
    case Tok::MinusMinus:
      next();
      return std::make_unique<IncDecStmt>(l, std::move(e), false);
    default:
      return std::make_unique<ExprStmt>(l, std::move(e));
    }
    SourceLoc opLoc = loc();
    next();
    auto rhs = parseExpr();
    return std::make_unique<AssignStmt>(opLoc, std::move(e), op, std::move(rhs));
  }

  StmtPtr parseIf() {
    auto s = std::make_unique<IfStmt>(loc());
    expect(Tok::KwIf);
    noStructLit_ = true;
    s->cond = parseExpr();
    noStructLit_ = false;
    s->then = parseBlock();
    if (accept(Tok::KwElse)) {
      if (at(Tok::KwIf))
        s->els = parseIf();
      else
        s->els = parseBlock();
    }
    return s;
  }

  StmtPtr parseSwitch() {
    auto sw = std::make_unique<SwitchStmt>(loc());
    expect(Tok::KwSwitch);
    if (!at(Tok::LBrace)) {
      noStructLit_ = true;
      sw->tag = parseExpr();
      noStructLit_ = false;
    }
    expect(Tok::LBrace, "to start switch body");
    skipSemis();
    while (!at(Tok::RBrace) && !at(Tok::Eof)) {
      SwitchCase c;
      c.loc = loc();
      if (accept(Tok::KwDefault)) {
        c.isDefault = true;
      } else {
        expect(Tok::KwCase, "in switch (each branch starts with 'case' or 'default')");
        do {
          c.values.push_back(parseExpr());
        } while (accept(Tok::Comma));
      }
      expect(Tok::Colon, "after case");
      skipSemis();
      while (!at(Tok::KwCase) && !at(Tok::KwDefault) && !at(Tok::RBrace) && !at(Tok::Eof)) {
        c.body.push_back(parseStmt());
        if (!at(Tok::KwCase) && !at(Tok::KwDefault) && !at(Tok::RBrace))
          expect(Tok::Semi, "after statement");
        skipSemis();
      }
      sw->cases.push_back(std::move(c));
    }
    expect(Tok::RBrace, "to end switch");
    return sw;
  }

  StmtPtr parseFor() {
    SourceLoc l = loc();
    expect(Tok::KwFor);
    if (at(Tok::LBrace)) {
      auto f = std::make_unique<ForStmt>(l);
      f->body = parseBlock();
      return f;
    }
    bool twoVars = at(Tok::Ident) && peekTok(1).kind == Tok::Comma && peekTok(2).kind == Tok::Ident &&
                   peekTok(3).kind == Tok::Define && peekTok(4).kind == Tok::KwRange;
    if (twoVars || (at(Tok::Ident) && peekTok(1).kind == Tok::Define && peekTok(2).kind == Tok::KwRange)) {
      auto f = std::make_unique<ForRangeStmt>(l);
      f->nameLoc = loc();
      f->name = next().text;
      if (twoVars) {
        next();
        f->valueLoc = loc();
        f->valueName = next().text;
      }
      next();
      next();
      noStructLit_ = true;
      f->range = parseExpr();
      noStructLit_ = false;
      f->body = parseBlock();
      return f;
    }
    auto f = std::make_unique<ForStmt>(l);
    noStructLit_ = true;
    StmtPtr first;
    if (!at(Tok::Semi))
      first = parseSimpleStmt();
    if (accept(Tok::Semi)) {
      f->init = std::move(first);
      if (!at(Tok::Semi))
        f->cond = parseExpr();
      expect(Tok::Semi, "in for loop header");
      if (!at(Tok::LBrace))
        f->post = parseSimpleStmt();
    } else {
      if (!first || first->kind != StmtKind::Expr)
        fail("expected a condition in for loop");
      f->cond = std::move(static_cast<ExprStmt *>(first.get())->expr);
    }
    noStructLit_ = false;
    f->body = parseBlock();
    return f;
  }

  // ----- expressions -----

  ExprPtr parseExpr() { return parseBinary(1); }

  static int precedence(Tok t) {
    switch (t) {
    case Tok::KwOr: return 1;
    case Tok::OrOr: return 2;
    case Tok::AndAnd: return 3;
    case Tok::Eq: case Tok::Ne: case Tok::Lt: case Tok::Le: case Tok::Gt: case Tok::Ge: return 4;
    case Tok::Plus: case Tok::Minus: return 5;
    case Tok::Star: case Tok::Slash: case Tok::Percent: return 6;
    default: return 0;
    }
  }
  static BinOp binOpFor(Tok t) {
    switch (t) {
    case Tok::KwOr: return BinOp::OrElse;
    case Tok::OrOr: return BinOp::Or;
    case Tok::AndAnd: return BinOp::And;
    case Tok::Eq: return BinOp::Eq;
    case Tok::Ne: return BinOp::Ne;
    case Tok::Lt: return BinOp::Lt;
    case Tok::Le: return BinOp::Le;
    case Tok::Gt: return BinOp::Gt;
    case Tok::Ge: return BinOp::Ge;
    case Tok::Plus: return BinOp::Add;
    case Tok::Minus: return BinOp::Sub;
    case Tok::Star: return BinOp::Mul;
    case Tok::Slash: return BinOp::Div;
    default: return BinOp::Rem;
    }
  }

  ExprPtr parseBinary(int minPrec) {
    auto lhs = parseUnary();
    for (;;) {
      int prec = precedence(cur().kind);
      if (prec < minPrec || prec == 0)
        return lhs;
      SourceLoc l = loc();
      BinOp op = binOpFor(next().kind);
      auto rhs = parseBinary(prec + 1);
      lhs = std::make_unique<BinaryExpr>(l, op, std::move(lhs), std::move(rhs));
    }
  }

  ExprPtr parseUnary() {
    SourceLoc l = loc();
    if (accept(Tok::Minus))
      return std::make_unique<UnaryExpr>(l, UnOp::Neg, parseUnary());
    if (accept(Tok::Not))
      return std::make_unique<UnaryExpr>(l, UnOp::Not, parseUnary());
    if (accept(Tok::Star))
      return std::make_unique<UnaryExpr>(l, UnOp::Deref, parseUnary());
    if (accept(Tok::Amp)) {
      bool mut = accept(Tok::KwMut);
      return std::make_unique<UnaryExpr>(l, mut ? UnOp::RefMut : UnOp::Ref, parseUnary());
    }
    if (accept(Tok::AndAnd)) {
      bool mut = accept(Tok::KwMut);
      auto inner = std::make_unique<UnaryExpr>(l, mut ? UnOp::RefMut : UnOp::Ref, parseUnary());
      return std::make_unique<UnaryExpr>(l, UnOp::Ref, std::move(inner));
    }
    return parsePostfix(parsePrimary());
  }

  ExprPtr parsePostfix(ExprPtr e) {
    for (;;) {
      SourceLoc l = loc();
      if (accept(Tok::Dot)) {
        SourceLoc nl = loc();
        std::string name = expect(Tok::Ident, "after '.'").text;
        e = std::make_unique<FieldExpr>(nl, std::move(e), name);
      } else if (accept(Tok::LParen)) {
        auto call = std::make_unique<CallExpr>(l, std::move(e));
        bool saved = noStructLit_;
        noStructLit_ = false;
        skipSemis();
        while (!at(Tok::RParen)) {
          call->args.push_back(parseExpr());
          skipSemis();
          if (!accept(Tok::Comma))
            break;
          skipSemis();
        }
        expect(Tok::RParen, "after call arguments");
        noStructLit_ = saved;
        e = std::move(call);
      } else if (accept(Tok::LBracket)) {
        bool saved = noStructLit_;
        noStructLit_ = false;
        auto idx = parseExpr();
        noStructLit_ = saved;
        expect(Tok::RBracket, "after index");
        e = std::make_unique<IndexExpr>(l, std::move(e), std::move(idx));
      } else {
        return e;
      }
    }
  }

  ExprPtr parsePrimary() {
    SourceLoc l = loc();
    const Token &t = cur();
    switch (t.kind) {
    case Tok::Int:
      next();
      return std::make_unique<IntLitExpr>(l, t.intVal);
    case Tok::Float:
      next();
      return std::make_unique<FloatLitExpr>(l, t.floatVal);
    case Tok::String:
      next();
      return std::make_unique<StrLitExpr>(l, t.text);
    case Tok::KwTrue:
      next();
      return std::make_unique<BoolLitExpr>(l, true);
    case Tok::KwFalse:
      next();
      return std::make_unique<BoolLitExpr>(l, false);
    case Tok::KwNone:
      next();
      return std::make_unique<NoneLitExpr>(l);
    case Tok::LParen: {
      next();
      bool saved = noStructLit_;
      noStructLit_ = false;
      auto e = parseExpr();
      noStructLit_ = saved;
      expect(Tok::RParen);
      return e;
    }
    case Tok::LBracket: {
      next();
      expect(Tok::RBracket, "in slice literal");
      auto sl = std::make_unique<SliceLitExpr>(l, parseType());
      expect(Tok::LBrace, "to start slice literal");
      skipSemis();
      while (!at(Tok::RBrace)) {
        sl->elems.push_back(parseExpr());
        skipSemis();
        if (!accept(Tok::Comma))
          break;
        skipSemis();
      }
      expect(Tok::RBrace, "to end slice literal");
      return sl;
    }
    case Tok::Ident: {
      next();
      if (at(Tok::LBrace) && !noStructLit_)
        return parseStructLit(l, t.text);
      return std::make_unique<IdentExpr>(l, t.text);
    }
    default:
      fail("expected an expression, found " + describe(t));
    }
  }

  ExprPtr parseStructLit(SourceLoc l, const std::string &name) {
    auto sl = std::make_unique<StructLitExpr>(l, name);
    expect(Tok::LBrace);
    skipSemis();
    while (!at(Tok::RBrace)) {
      FieldInit fi;
      fi.loc = loc();
      fi.name = expect(Tok::Ident, "for field name").text;
      expect(Tok::Colon, "after field name");
      fi.value = parseExpr();
      sl->fields.push_back(std::move(fi));
      skipSemis();
      if (!accept(Tok::Comma))
        break;
      skipSemis();
    }
    expect(Tok::RBrace, "to end struct literal");
    return sl;
  }
};

} // namespace

std::unique_ptr<Program> parse(const std::vector<Token> &toks, Diagnostics &diag) {
  Parser p(toks, diag);
  return p.parseProgram();
}

} // namespace co
