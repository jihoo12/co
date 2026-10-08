#include "mir_build.h"

#include <unordered_map>

namespace co {
using namespace mir;

namespace {

bool isPlaceExpr(const Expr *e) {
  switch (e->kind) {
  case ExprKind::Ident:
  case ExprKind::Field:
  case ExprKind::Index:
    return true;
  case ExprKind::Unary:
    return static_cast<const UnaryExpr *>(e)->op == UnOp::Deref;
  default:
    return false;
  }
}

class FnBuilder {
public:
  FnBuilder(FuncDecl &fd, TypeContext &tc, Diagnostics &diag) : fd_(fd), tc_(tc), diag_(diag) {}

  Function build() {
    FuncInfo *info = fd_.info;
    f_.info = info;
    f_.endLoc = fd_.body->endLoc;
    f_.locals.push_back({"", info->ret, fd_.loc});
    cur_ = newBlock();
    returnBB_ = newBlock();
    f_.blocks[returnBB_].term.kind = Terminator::Return;
    f_.blocks[returnBB_].term.loc = fd_.body->endLoc;
    f_.blocks[returnBB_].terminated = true;

    scopes_.emplace_back();
    std::vector<LocalVar *> params;
    if (fd_.receiver)
      params.push_back(fd_.receiver->var);
    for (auto &p : fd_.params)
      params.push_back(p.var);
    for (LocalVar *v : params) {
      int l = newLocal(v->name, v->type, v->loc);
      vars_[v] = l;
    }
    f_.numParams = (int)params.size();

    lowerBlock(*fd_.body);
    exitScopes(0, fd_.body->endLoc);
    gotoBlock(returnBB_, fd_.body->endLoc);
    return std::move(f_);
  }

private:
  FuncDecl &fd_;
  TypeContext &tc_;
  Diagnostics &diag_;
  Function f_;
  int cur_ = 0;
  int returnBB_ = 0;
  std::unordered_map<LocalVar *, int> vars_;
  std::vector<std::vector<int>> scopes_; // locals declared in each scope, in order
  struct Loop {
    int breakBB, continueBB;
    size_t scopeDepth;
  };
  std::vector<Loop> loops_;

  // ----- construction helpers -----

  int newBlock() {
    f_.blocks.emplace_back();
    return (int)f_.blocks.size() - 1;
  }

  int newLocal(const std::string &name, Type *ty, SourceLoc loc, int scopeIdx = -1) {
    f_.locals.push_back({name, ty, loc});
    int l = (int)f_.locals.size() - 1;
    if (scopeIdx < 0)
      scopeIdx = (int)scopes_.size() - 1;
    scopes_[scopeIdx].push_back(l);
    return l;
  }
  int newTemp(Type *ty, SourceLoc loc) { return newLocal("", ty, loc); }

  void emit(Statement s) { f_.blocks[cur_].stmts.push_back(std::move(s)); }

  void assign(Place dest, Rvalue rv, SourceLoc loc) {
    Statement s;
    s.kind = Statement::Assign;
    s.place = std::move(dest);
    s.rv = std::move(rv);
    s.loc = loc;
    emit(std::move(s));
  }

  void terminate(Terminator t) {
    BasicBlock &bb = f_.blocks[cur_];
    if (bb.terminated)
      return;
    bb.term = std::move(t);
    bb.terminated = true;
  }
  void gotoBlock(int target, SourceLoc loc) {
    Terminator t;
    t.kind = Terminator::Goto;
    t.target = target;
    t.loc = loc;
    terminate(std::move(t));
  }
  void branch(Operand cond, int thenBB, int elseBB, SourceLoc loc) {
    Terminator t;
    t.kind = Terminator::If;
    t.cond = std::move(cond);
    t.thenBB = thenBB;
    t.elseBB = elseBB;
    t.loc = loc;
    terminate(std::move(t));
  }

  void emitScopeExit(const std::vector<int> &locals, SourceLoc loc) {
    for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
      if (f_.locals[*it].type->needsDrop()) {
        Statement d;
        d.kind = Statement::Drop;
        d.place.local = *it;
        d.loc = loc;
        emit(std::move(d));
      }
      Statement sd;
      sd.kind = Statement::StorageDead;
      sd.local = *it;
      sd.loc = loc;
      emit(std::move(sd));
    }
  }
  // Emits drops for scopes [depth, top) without popping them (early exits).
  void exitScopes(size_t depth, SourceLoc loc) {
    for (size_t i = scopes_.size(); i-- > depth;)
      emitScopeExit(scopes_[i], loc);
  }
  void pushScope() { scopes_.emplace_back(); }
  void popScope(SourceLoc loc) {
    emitScopeExit(scopes_.back(), loc);
    scopes_.pop_back();
  }

  static Operand constInt(int64_t v, Type *ty) {
    Operand o;
    o.kind = Operand::Const;
    o.c.kind = Constant::Int;
    o.c.i = v;
    o.type = ty;
    return o;
  }
  static Rvalue use(Operand o) {
    Rvalue rv;
    rv.kind = Rvalue::Use;
    rv.type = o.type;
    rv.ops.push_back(std::move(o));
    return rv;
  }
  static Operand copyOf(Place p, Type *ty) {
    Operand o;
    o.kind = Operand::Copy;
    o.place = std::move(p);
    o.type = ty;
    return o;
  }

  // Reads a place as a value: copy for Copy types, otherwise a move.
  Operand useOf(Place p, Type *ty, SourceLoc loc) {
    Operand o;
    o.place = p;
    o.type = ty;
    if (ty->isCopy()) {
      o.kind = Operand::Copy;
      return o;
    }
    o.kind = Operand::Move;
    if (!p.proj.empty()) {
      bool behindRef = false;
      for (auto &pr : p.proj)
        behindRef |= pr.kind == Proj::Deref;
      std::string name = f_.placeName(p);
      std::string why;
      if (behindRef)
        why = "it is behind a reference";
      else if (p.proj.back().kind == Proj::Index)
        why = "it is an element of a slice";
      else
        why = "it is a field (moving it would leave the struct partially empty)";
      diag_.error(loc, "cannot move out of '" + name + "' because " + why + "; borrow it with '&' or use clone(...)");
      o.kind = Operand::Copy; // avoid cascading errors
    }
    return o;
  }

  // ----- expressions -----

  // Materializes `e` into a fresh temporary and returns an operand reading it.
  Operand toTemp(const Expr *e) {
    int t = newTemp(e->type, e->loc);
    lowerInto(Place{t, {}}, e);
    Operand o;
    o.kind = e->type->isCopy() ? Operand::Copy : Operand::Move;
    o.place.local = t;
    o.type = e->type;
    return o;
  }

  Operand lowerOperand(const Expr *e) {
    switch (e->kind) {
    case ExprKind::IntLit: {
      return constInt(static_cast<const IntLitExpr *>(e)->value, e->type);
    }
    case ExprKind::FloatLit: {
      Operand o;
      o.kind = Operand::Const;
      o.c.kind = Constant::Float;
      o.c.f = static_cast<const FloatLitExpr *>(e)->value;
      o.type = e->type;
      return o;
    }
    case ExprKind::BoolLit: {
      Operand o;
      o.kind = Operand::Const;
      o.c.kind = Constant::Bool;
      o.c.b = static_cast<const BoolLitExpr *>(e)->value;
      o.type = e->type;
      return o;
    }
    default:
      if (isPlaceExpr(e))
        return useOf(lowerPlace(e), e->type, e->loc);
      return toTemp(e);
    }
  }

  // Like lowerOperand, but reads of places happen *now* (into a temp), so
  // evaluation order of call arguments matches the source.
  Operand lowerArg(const Expr *e) {
    Operand o = lowerOperand(e);
    if (o.kind == Operand::Const || (o.place.isLocal() && f_.locals[o.place.local].name.empty()))
      return o;
    int t = newTemp(e->type, e->loc);
    assign(Place{t, {}}, use(o), e->loc);
    Operand r;
    r.kind = e->type->isCopy() ? Operand::Copy : Operand::Move;
    r.place.local = t;
    r.type = e->type;
    return r;
  }

  Place placeOrTemp(const Expr *e) {
    if (isPlaceExpr(e))
      return lowerPlace(e);
    int t = newTemp(e->type, e->loc);
    lowerInto(Place{t, {}}, e);
    return Place{t, {}};
  }

  Place lowerPlace(const Expr *e) {
    switch (e->kind) {
    case ExprKind::Ident:
      return Place{vars_.at(static_cast<const IdentExpr *>(e)->var), {}};
    case ExprKind::Field: {
      auto *fe = static_cast<const FieldExpr *>(e);
      Place p = placeOrTemp(fe->base.get());
      if (fe->autoDeref)
        p.proj.push_back({Proj::Deref});
      p.proj.push_back({Proj::Field, fe->index});
      return p;
    }
    case ExprKind::Index: {
      auto *ie = static_cast<const IndexExpr *>(e);
      Place p = placeOrTemp(ie->base.get());
      if (ie->autoDeref)
        p.proj.push_back({Proj::Deref});
      int idx = newTemp(tc_.intTy(), ie->index->loc);
      lowerInto(Place{idx, {}}, ie->index.get());
      p.proj.push_back({Proj::Index, -1, idx});
      return p;
    }
    case ExprKind::Unary: {
      auto *u = static_cast<const UnaryExpr *>(e);
      Place p = placeOrTemp(u->operand.get());
      p.proj.push_back({Proj::Deref});
      return p;
    }
    default:
      return placeOrTemp(e);
    }
  }

  void lowerInto(Place dest, const Expr *e) {
    SourceLoc loc = e->loc;
    switch (e->kind) {
    case ExprKind::IntLit:
    case ExprKind::FloatLit:
    case ExprKind::BoolLit:
      assign(dest, use(lowerOperand(e)), loc);
      return;
    case ExprKind::StrLit: {
      Rvalue rv;
      rv.kind = Rvalue::Builtin;
      rv.builtin = BuiltinOp::StrLit;
      rv.strLit = static_cast<const StrLitExpr *>(e)->value;
      rv.type = e->type;
      assign(dest, std::move(rv), loc);
      return;
    }
    case ExprKind::Ident:
    case ExprKind::Field:
    case ExprKind::Index:
      assign(dest, use(useOf(lowerPlace(e), e->type, loc)), loc);
      return;
    case ExprKind::Unary: {
      auto *u = static_cast<const UnaryExpr *>(e);
      Rvalue rv;
      rv.type = e->type;
      switch (u->op) {
      case UnOp::Deref:
        assign(dest, use(useOf(lowerPlace(e), e->type, loc)), loc);
        return;
      case UnOp::Neg:
      case UnOp::Not:
        rv.kind = Rvalue::UnaryOp;
        rv.uop = u->op;
        rv.ops.push_back(lowerOperand(u->operand.get()));
        break;
      case UnOp::Ref:
      case UnOp::RefMut:
        rv.kind = Rvalue::Ref;
        rv.mut = u->op == UnOp::RefMut;
        rv.place = placeOrTemp(u->operand.get());
        break;
      case UnOp::ReborrowShared:
      case UnOp::ReborrowMut:
        rv.kind = Rvalue::Ref;
        rv.mut = u->op == UnOp::ReborrowMut;
        rv.place = placeOrTemp(u->operand.get());
        rv.place.proj.push_back({Proj::Deref});
        break;
      }
      assign(dest, std::move(rv), loc);
      return;
    }
    case ExprKind::Binary: {
      auto *b = static_cast<const BinaryExpr *>(e);
      if (b->op == BinOp::And || b->op == BinOp::Or) {
        // Short-circuit: r = lhs; if (r == isAnd) r = rhs
        int r = newTemp(tc_.boolTy(), loc);
        lowerInto(Place{r, {}}, b->lhs.get());
        int rhsBB = newBlock(), join = newBlock();
        if (b->op == BinOp::And)
          branch(copyOf(Place{r, {}}, tc_.boolTy()), rhsBB, join, loc);
        else
          branch(copyOf(Place{r, {}}, tc_.boolTy()), join, rhsBB, loc);
        cur_ = rhsBB;
        lowerInto(Place{r, {}}, b->rhs.get());
        gotoBlock(join, loc);
        cur_ = join;
        assign(dest, use(copyOf(Place{r, {}}, tc_.boolTy())), loc);
        return;
      }
      Rvalue rv;
      rv.type = e->type;
      bool strOp = b->lhs->type->isRef() && b->lhs->type->inner->kind == TypeKind::String;
      Operand l = lowerOperand(b->lhs.get());
      Operand r = lowerOperand(b->rhs.get());
      rv.ops.push_back(std::move(l));
      rv.ops.push_back(std::move(r));
      if (strOp) {
        rv.kind = Rvalue::Builtin;
        if (b->op == BinOp::Add) {
          rv.builtin = BuiltinOp::StrConcat;
        } else {
          rv.builtin = BuiltinOp::StrCmp;
          rv.cmp = b->op;
        }
      } else {
        rv.kind = Rvalue::BinaryOp;
        rv.bop = b->op;
      }
      assign(dest, std::move(rv), loc);
      return;
    }
    case ExprKind::Call: {
      auto *c = static_cast<const CallExpr *>(e);
      assign(dest, lowerCall(c), loc);
      if (c->builtin == Builtin::Panic) {
        Terminator t;
        t.kind = Terminator::Unreachable;
        t.loc = loc;
        terminate(t);
        cur_ = newBlock();
      }
      return;
    }
    case ExprKind::StructLit: {
      auto *sl = static_cast<const StructLitExpr *>(e);
      Rvalue rv;
      rv.kind = Rvalue::Aggregate;
      rv.type = e->type;
      rv.ops.resize(sl->st->fields.size());
      std::vector<bool> given(sl->st->fields.size(), false);
      for (auto &fi : sl->fields) {
        rv.ops[fi.index] = lowerArg(fi.value.get());
        given[fi.index] = true;
      }
      for (size_t i = 0; i < given.size(); i++) {
        if (!given[i]) {
          rv.ops[i].kind = Operand::Const;
          rv.ops[i].c.kind = Constant::Zero;
          rv.ops[i].type = sl->st->fields[i].type;
        }
      }
      assign(dest, std::move(rv), loc);
      return;
    }
    case ExprKind::SliceLit: {
      auto *sl = static_cast<const SliceLitExpr *>(e);
      Rvalue rv;
      rv.kind = Rvalue::SliceLit;
      rv.type = e->type;
      for (auto &el : sl->elems)
        rv.ops.push_back(lowerArg(el.get()));
      assign(dest, std::move(rv), loc);
      return;
    }
    }
  }

  Rvalue lowerCall(const CallExpr *c) {
    Rvalue rv;
    rv.type = c->type;
    if (c->builtin != Builtin::None) {
      rv.kind = Rvalue::Builtin;
      switch (c->builtin) {
      case Builtin::Print: rv.builtin = BuiltinOp::Print; break;
      case Builtin::Println: rv.builtin = BuiltinOp::Println; break;
      case Builtin::Len: rv.builtin = BuiltinOp::Len; break;
      case Builtin::Append: rv.builtin = BuiltinOp::Append; break;
      case Builtin::Clone: rv.builtin = BuiltinOp::Clone; break;
      case Builtin::ToInt: rv.builtin = BuiltinOp::ToInt; break;
      case Builtin::ToFloat: rv.builtin = BuiltinOp::ToFloat; break;
      case Builtin::ToStr: rv.builtin = BuiltinOp::ToStr; break;
      case Builtin::Panic: rv.builtin = BuiltinOp::Panic; break;
      case Builtin::None: break;
      }
      for (auto &a : c->args)
        rv.ops.push_back(lowerArg(a.get()));
      return rv;
    }
    rv.kind = Rvalue::Call;
    rv.func = c->func;
    rv.ops.resize(c->args.size());
    size_t first = c->receiverLast ? 1 : 0;
    for (size_t i = first; i < c->args.size(); i++)
      rv.ops[i] = lowerArg(c->args[i].get());
    if (c->receiverLast)
      rv.ops[0] = lowerArg(c->args[0].get());
    return rv;
  }

  // Evaluates a condition into a bool local; temporaries are dropped before branching.
  Operand lowerCond(const Expr *e) {
    int c = newTemp(tc_.boolTy(), e->loc);
    pushScope();
    lowerInto(Place{c, {}}, e);
    popScope(e->loc);
    return copyOf(Place{c, {}}, tc_.boolTy());
  }

  // ----- statements -----

  void lowerBlock(const BlockStmt &b) {
    pushScope();
    for (auto &s : b.stmts)
      lowerStmt(*s);
    popScope(b.endLoc);
  }

  void lowerStmt(const Stmt &s) {
    switch (s.kind) {
    case StmtKind::Block:
      lowerBlock(static_cast<const BlockStmt &>(s));
      return;
    case StmtKind::If:
      lowerIf(static_cast<const IfStmt &>(s));
      return;
    case StmtKind::For:
      lowerFor(static_cast<const ForStmt &>(s));
      return;
    case StmtKind::ForRange:
      lowerForRange(static_cast<const ForRangeStmt &>(s));
      return;
    default:
      break;
    }
    // Simple statements get their own scope for temporaries.
    pushScope();
    lowerSimple(s);
    popScope(s.loc);
  }

  void lowerSimple(const Stmt &s) {
    switch (s.kind) {
    case StmtKind::VarDecl: {
      auto &vd = static_cast<const VarDeclStmt &>(s);
      int blockScope = (int)scopes_.size() - 2;
      int l = newLocal(vd.name, vd.var->type, vd.loc, blockScope);
      vars_[vd.var] = l;
      if (!vd.init) {
        Operand z;
        z.kind = Operand::Const;
        z.c.kind = Constant::Zero;
        z.type = vd.var->type;
        assign(Place{l, {}}, use(z), vd.loc);
        return;
      }
      const Expr *init = vd.init.get();
      if (init->kind == ExprKind::Unary) {
        auto *u = static_cast<const UnaryExpr *>(init);
        if ((u->op == UnOp::Ref || u->op == UnOp::RefMut) && !isPlaceExpr(u->operand.get())) {
          // `x := &value()` keeps the temporary alive as long as the block.
          int t = newLocal("", u->operand->type, u->operand->loc, blockScope);
          lowerInto(Place{t, {}}, u->operand.get());
          Rvalue rv;
          rv.kind = Rvalue::Ref;
          rv.mut = u->op == UnOp::RefMut;
          rv.place = Place{t, {}};
          rv.type = init->type;
          assign(Place{l, {}}, std::move(rv), init->loc);
          return;
        }
      }
      lowerInto(Place{l, {}}, init);
      return;
    }
    case StmtKind::Expr: {
      auto &es = static_cast<const ExprStmt &>(s);
      int t = newTemp(es.expr->type, es.loc);
      lowerInto(Place{t, {}}, es.expr.get());
      return;
    }
    case StmtKind::Assign:
      lowerAssign(static_cast<const AssignStmt &>(s));
      return;
    case StmtKind::IncDec: {
      auto &st = static_cast<const IncDecStmt &>(s);
      Place p = lowerPlace(st.target.get());
      Rvalue rv;
      rv.kind = Rvalue::BinaryOp;
      rv.bop = st.inc ? BinOp::Add : BinOp::Sub;
      rv.type = tc_.intTy();
      rv.ops.push_back(copyOf(p, tc_.intTy()));
      rv.ops.push_back(constInt(1, tc_.intTy()));
      assign(p, std::move(rv), st.loc);
      return;
    }
    case StmtKind::Return: {
      auto &rs = static_cast<const ReturnStmt &>(s);
      if (rs.value)
        lowerInto(Place{0, {}}, rs.value.get());
      exitScopes(0, rs.loc);
      gotoBlock(returnBB_, rs.loc);
      cur_ = newBlock();
      return;
    }
    case StmtKind::Break:
    case StmtKind::Continue: {
      const Loop &lp = loops_.back();
      exitScopes(lp.scopeDepth, s.loc);
      gotoBlock(s.kind == StmtKind::Break ? lp.breakBB : lp.continueBB, s.loc);
      cur_ = newBlock();
      return;
    }
    default:
      return;
    }
  }

  void lowerAssign(const AssignStmt &as) {
    if (as.appendInPlace) {
      // x = append(x, v)  ==>  push(&mut x, v)
      auto *call = static_cast<const CallExpr *>(as.rhs.get());
      Operand elem = lowerArg(call->args[1].get());
      Place p = lowerPlace(as.lhs.get());
      int r = newTemp(tc_.ref(as.lhs->type, true), as.loc);
      Rvalue ref;
      ref.kind = Rvalue::Ref;
      ref.mut = true;
      ref.place = p;
      ref.type = f_.locals[r].type;
      assign(Place{r, {}}, std::move(ref), as.loc);
      Rvalue push;
      push.kind = Rvalue::Builtin;
      push.builtin = BuiltinOp::Push;
      push.type = tc_.voidTy();
      Operand rop;
      rop.kind = Operand::Move;
      rop.place.local = r;
      rop.type = f_.locals[r].type;
      push.ops.push_back(rop);
      push.ops.push_back(elem);
      int v = newTemp(tc_.voidTy(), as.loc);
      assign(Place{v, {}}, std::move(push), as.loc);
      return;
    }
    if (as.op == AssignOp::Set) {
      Operand val = lowerOperand(as.rhs.get());
      Place p = lowerPlace(as.lhs.get());
      assign(p, use(val), as.loc);
      return;
    }
    Type *lt = as.lhs->type;
    if (lt->kind == TypeKind::String) {
      Operand rhs = lowerArg(as.rhs.get());
      Place p = lowerPlace(as.lhs.get());
      int r = newTemp(tc_.ref(lt, false), as.loc);
      Rvalue ref;
      ref.kind = Rvalue::Ref;
      ref.place = p;
      ref.type = f_.locals[r].type;
      assign(Place{r, {}}, std::move(ref), as.loc);
      Rvalue cat;
      cat.kind = Rvalue::Builtin;
      cat.builtin = BuiltinOp::StrConcat;
      cat.type = lt;
      cat.ops.push_back(copyOf(Place{r, {}}, f_.locals[r].type));
      cat.ops.push_back(rhs);
      int t = newTemp(lt, as.loc);
      assign(Place{t, {}}, std::move(cat), as.loc);
      Operand mv;
      mv.kind = Operand::Move;
      mv.place.local = t;
      mv.type = lt;
      assign(p, use(mv), as.loc);
      return;
    }
    Operand rhs = lowerArg(as.rhs.get());
    Place p = lowerPlace(as.lhs.get());
    Rvalue rv;
    rv.kind = Rvalue::BinaryOp;
    rv.type = lt;
    switch (as.op) {
    case AssignOp::Add: rv.bop = BinOp::Add; break;
    case AssignOp::Sub: rv.bop = BinOp::Sub; break;
    case AssignOp::Mul: rv.bop = BinOp::Mul; break;
    case AssignOp::Div: rv.bop = BinOp::Div; break;
    default: rv.bop = BinOp::Rem; break;
    }
    rv.ops.push_back(copyOf(p, lt));
    rv.ops.push_back(rhs);
    assign(p, std::move(rv), as.loc);
  }

  void lowerIf(const IfStmt &is) {
    Operand c = lowerCond(is.cond.get());
    int thenBB = newBlock(), elseBB = newBlock(), join = newBlock();
    branch(c, thenBB, elseBB, is.loc);
    cur_ = thenBB;
    lowerBlock(*is.then);
    gotoBlock(join, is.then->endLoc);
    cur_ = elseBB;
    if (is.els)
      lowerStmt(*is.els);
    gotoBlock(join, is.loc);
    cur_ = join;
  }

  void lowerFor(const ForStmt &fs) {
    pushScope();
    if (fs.init)
      lowerStmt(*fs.init);
    int header = newBlock(), body = newBlock(), cont = newBlock(), exit = newBlock();
    gotoBlock(header, fs.loc);
    cur_ = header;
    if (fs.cond)
      branch(lowerCond(fs.cond.get()), body, exit, fs.loc);
    else
      gotoBlock(body, fs.loc);
    cur_ = body;
    loops_.push_back({exit, cont, scopes_.size()});
    lowerBlock(*fs.body);
    loops_.pop_back();
    gotoBlock(cont, fs.body->endLoc);
    cur_ = cont;
    if (fs.post)
      lowerStmt(*fs.post);
    gotoBlock(header, fs.loc);
    cur_ = exit;
    popScope(fs.body->endLoc);
  }

  void lowerForRange(const ForRangeStmt &fr) {
    pushScope();
    Type *intTy = tc_.intTy();
    int n = newTemp(intTy, fr.range->loc);
    lowerInto(Place{n, {}}, fr.range.get());
    int i = newTemp(intTy, fr.loc);
    assign(Place{i, {}}, use(constInt(0, intTy)), fr.loc);
    int header = newBlock(), body = newBlock(), cont = newBlock(), exit = newBlock();
    gotoBlock(header, fr.loc);
    cur_ = header;
    int c = newTemp(tc_.boolTy(), fr.loc);
    Rvalue lt;
    lt.kind = Rvalue::BinaryOp;
    lt.bop = BinOp::Lt;
    lt.type = tc_.boolTy();
    lt.ops.push_back(copyOf(Place{i, {}}, intTy));
    lt.ops.push_back(copyOf(Place{n, {}}, intTy));
    assign(Place{c, {}}, std::move(lt), fr.loc);
    branch(copyOf(Place{c, {}}, tc_.boolTy()), body, exit, fr.loc);

    cur_ = body;
    loops_.push_back({exit, cont, scopes_.size()});
    pushScope();
    int v = newLocal(fr.name, intTy, fr.nameLoc);
    vars_[fr.var] = v;
    assign(Place{v, {}}, use(copyOf(Place{i, {}}, intTy)), fr.nameLoc);
    lowerBlock(*fr.body);
    popScope(fr.body->endLoc);
    loops_.pop_back();
    gotoBlock(cont, fr.body->endLoc);

    cur_ = cont;
    Rvalue inc;
    inc.kind = Rvalue::BinaryOp;
    inc.bop = BinOp::Add;
    inc.type = intTy;
    inc.ops.push_back(copyOf(Place{i, {}}, intTy));
    inc.ops.push_back(constInt(1, intTy));
    assign(Place{i, {}}, std::move(inc), fr.loc);
    gotoBlock(header, fr.loc);

    cur_ = exit;
    popScope(fr.body->endLoc);
  }
};

} // namespace

mir::Module buildMir(Program &prog, TypeContext &tc, Diagnostics &diag) {
  mir::Module m;
  for (auto &fd : prog.funcs) {
    if (!fd->info)
      continue;
    FnBuilder b(*fd, tc, diag);
    m.funcs.push_back(b.build());
  }
  return m;
}

} // namespace co
