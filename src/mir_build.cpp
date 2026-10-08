#include "mir_build.h"

#include <unordered_map>

namespace co {
using namespace mir;

namespace {

bool isPlaceExpr(const Expr *e) {
  switch (e->kind) {
  case ExprKind::Ident:
  case ExprKind::Field:
    return true;
  case ExprKind::Index:
    return static_cast<const IndexExpr *>(e)->mode != IndexExpr::MapRead;
  case ExprKind::Unary:
    return static_cast<const UnaryExpr *>(e)->op == UnOp::Deref;
  default:
    return false;
  }
}

class FnBuilder {
public:
  FnBuilder(FuncDecl &fd, TypeContext &tc) : fd_(fd), tc_(tc) {}

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
    if (returnsOkVoid())
      assignOkVoid(fd_.body->endLoc); // falling off the end of a `!` function means success
    exitScopes(0, fd_.body->endLoc);
    gotoBlock(returnBB_, fd_.body->endLoc);
    insertCopies();
    return std::move(f_);
  }

private:
  FuncDecl &fd_;
  TypeContext &tc_;
  Function f_;
  int cur_ = 0;
  int returnBB_ = 0;
  std::unordered_map<LocalVar *, int> vars_;
  std::vector<std::vector<int>> scopes_; // locals declared in each scope, in order
  struct Loop {
    int breakBB, continueBB;
    size_t scopeDepth;
    bool isSwitch = false; // `break` leaves a switch; `continue` skips it
  };
  std::vector<Loop> loops_;
  int extendScope_ = -1; // while lowering `x := ...`, `&temporary` lives as long as x's block

  // ----- moves and copies -----
  //
  // Assigning or passing a value moves it when the variable it came from is
  // not used again, and copies it otherwise, so a variable can always be used
  // after its value was handed on. Liveness decides: a move out of a local
  // that may still be read afterwards (itself, or through a reference taken
  // from it, like a switch binding) becomes a move out of a fresh copy.

  struct Uses {
    std::vector<int> uses, defs;
  };

  static void placeUses(const Place &p, std::vector<int> &uses) {
    uses.push_back(p.local);
    for (auto &pr : p.proj)
      if (pr.kind == Proj::Index)
        uses.push_back(pr.indexLocal);
  }

  static Uses usesOf(const Statement &s) {
    Uses u;
    if (s.kind == Statement::StorageDead) {
      u.defs.push_back(s.local);
    } else if (s.kind == Statement::Assign) {
      for (auto &op : s.rv.ops)
        if (op.kind != Operand::Const)
          placeUses(op.place, u.uses);
      if (s.rv.kind == Rvalue::Ref || s.rv.kind == Rvalue::Discriminant)
        placeUses(s.rv.place, u.uses);
      if (s.place.isLocal())
        u.defs.push_back(s.place.local);
      else
        placeUses(s.place, u.uses);
    }
    // Drops don't count: a moved-out variable is simply not dropped.
    return u;
  }

  Uses usesOf(const Terminator &t) const {
    Uses u;
    if (t.kind == Terminator::If && t.cond.kind != Operand::Const)
      placeUses(t.cond.place, u.uses);
    if (t.kind == Terminator::Return && f_.info->ret->kind != TypeKind::Void)
      u.uses.push_back(0);
    return u;
  }

  static std::vector<int> successors(const BasicBlock &bb) {
    switch (bb.term.kind) {
    case Terminator::Goto: return {bb.term.target};
    case Terminator::If: return {bb.term.thenBB, bb.term.elseBB};
    default: return {};
    }
  }

  void insertCopies() {
    size_t nb = f_.blocks.size(), nl = f_.locals.size();
    using Live = std::vector<bool>;
    auto step = [](Live &live, const Uses &u) {
      for (int d : u.defs)
        live[d] = false;
      for (int x : u.uses)
        live[x] = true;
    };
    std::vector<Live> liveOut(nb, Live(nl, false));
    auto liveIn = [&](size_t b) {
      Live live = liveOut[b];
      step(live, usesOf(f_.blocks[b].term));
      for (size_t i = f_.blocks[b].stmts.size(); i-- > 0;)
        step(live, usesOf(f_.blocks[b].stmts[i]));
      return live;
    };
    std::vector<std::vector<int>> preds(nb);
    for (size_t b = 0; b < nb; b++)
      for (int s : successors(f_.blocks[b]))
        preds[s].push_back((int)b);
    for (bool changed = true; changed;) {
      changed = false;
      for (size_t b = nb; b-- > 0;) {
        Live in = liveIn(b);
        for (int p : preds[b])
          for (size_t l = 0; l < nl; l++)
            if (in[l] && !liveOut[p][l]) {
              liveOut[p][l] = true;
              changed = true;
            }
      }
    }

    // The locals each reference-holding local may point into (transitively).
    // Only locals holding references get a row.
    std::vector<std::vector<bool>> from(nl);
    for (size_t l = 0; l < nl; l++)
      if (f_.locals[l].type->containsRef())
        from[l].assign(nl, false);
    for (bool changed = true; changed;) {
      changed = false;
      auto add = [&](int dst, int src, bool direct) {
        if (direct && !from[dst][src])
          from[dst][src] = changed = true;
        if (from[src].empty())
          return;
        for (size_t l = 0; l < nl; l++)
          if (from[src][l] && !from[dst][l])
            from[dst][l] = changed = true;
      };
      for (auto &bb : f_.blocks)
        for (auto &st : bb.stmts) {
          if (st.kind != Statement::Assign || !st.place.isLocal() || !f_.locals[st.place.local].type->containsRef())
            continue;
          int d = st.place.local;
          if (st.rv.kind == Rvalue::Ref)
            add(d, st.rv.place.local, true);
          for (auto &op : st.rv.ops)
            if (op.kind != Operand::Const && op.type->containsRef())
              add(d, op.place.local, false);
        }
    }
    auto stillNeeded = [&](const Live &after, int l) {
      if (after[l])
        return true;
      for (size_t r = 0; r < nl; r++)
        if (after[r] && !from[r].empty() && from[r][l])
          return true;
      return false;
    };

    for (size_t b = 0; b < nb; b++) {
      BasicBlock &bb = f_.blocks[b];
      // Which statements move a variable that is used again?
      Live live = liveOut[b];
      step(live, usesOf(bb.term));
      std::vector<std::vector<size_t>> copyOps(bb.stmts.size());
      for (size_t i = bb.stmts.size(); i-- > 0;) {
        Statement &s = bb.stmts[i];
        Uses u = usesOf(s);
        if (s.kind == Statement::Assign) {
          Live after = live;
          for (int d : u.defs)
            after[d] = false; // the statement's own result is a new value
          for (size_t oi = 0; oi < s.rv.ops.size(); oi++) {
            const Operand &op = s.rv.ops[oi];
            if (op.kind == Operand::Move && op.place.isLocal() && op.place.local != 0 &&
                stillNeeded(after, op.place.local) && !op.type->containsRef())
              copyOps[i].push_back(oi);
          }
        }
        step(live, u);
      }
      std::vector<Statement> out;
      for (size_t i = 0; i < bb.stmts.size(); i++) {
        for (size_t oi : copyOps[i]) {
          Operand &op = bb.stmts[i].rv.ops[oi];
          SourceLoc loc = bb.stmts[i].loc;
          int r = (int)f_.locals.size();
          f_.locals.push_back({"", tc_.ref(op.type, false), loc});
          int t = (int)f_.locals.size();
          f_.locals.push_back({"", op.type, loc});
          Statement ref;
          ref.kind = Statement::Assign;
          ref.place = Place{r, {}};
          ref.rv.kind = Rvalue::Ref;
          ref.rv.place = op.place;
          ref.rv.type = f_.locals[r].type;
          ref.loc = loc;
          out.push_back(std::move(ref));
          Statement cl;
          cl.kind = Statement::Assign;
          cl.place = Place{t, {}};
          cl.rv = builtinCall(BuiltinOp::Clone, op.type, {copyOf(Place{r, {}}, f_.locals[r].type)});
          cl.loc = loc;
          out.push_back(std::move(cl));
          op.place = Place{t, {}};
        }
        out.push_back(std::move(bb.stmts[i]));
      }
      bb.stmts = std::move(out);
    }
  }

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

  // Reads a place as a value: copy for Copy types, otherwise a move. Values
  // inside others (fields, elements, through references) are copied, since
  // taking them would leave a hole; whole variables are moved, and
  // insertCopies later turns moves of variables still needed into copies.
  Operand useOf(Place p, Type *ty, SourceLoc loc) {
    Operand o;
    o.place = p;
    o.type = ty;
    if (ty->isCopy()) {
      o.kind = Operand::Copy;
      return o;
    }
    if (!p.proj.empty())
      return cloneOf(std::move(p), ty, loc);
    o.kind = Operand::Move;
    return o;
  }

  // A fresh copy of the value at `p`.
  Operand cloneOf(Place p, Type *ty, SourceLoc loc) {
    int t = newTemp(ty, loc);
    assign(Place{t, {}}, builtinCall(BuiltinOp::Clone, ty, {borrow(std::move(p), ty, false, loc)}), loc);
    Operand o;
    o.kind = Operand::Move;
    o.place.local = t;
    o.type = ty;
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
    case ExprKind::NilLit: {
      Operand o;
      o.kind = Operand::Const;
      o.c.kind = Constant::Zero; // a null pointer
      o.type = e->type;
      return o;
    }
    case ExprKind::FuncRef: {
      Operand o;
      o.kind = Operand::Const;
      o.c.kind = Constant::Func;
      o.c.fn = static_cast<const FuncRefExpr *>(e)->func;
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
      if (ie->mode == IndexExpr::MapWrite) {
        // The entry itself: *map_slot(&mut m, &k)
        Operand key = lowerArg(ie->index.get());
        Type *mt = ie->base->type->derefAll();
        int r = newTemp(tc_.ref(mt->inner, true), e->loc);
        assign(Place{r, {}}, builtinCall(BuiltinOp::MapSlot, f_.locals[r].type, {borrow(p, mt, true, e->loc), key}),
               e->loc);
        return Place{r, {}}.withProj({Proj::Deref});
      }
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

  Rvalue builtinCall(BuiltinOp op, Type *ty, std::vector<Operand> ops) {
    Rvalue rv;
    rv.kind = Rvalue::Builtin;
    rv.builtin = op;
    rv.type = ty;
    rv.ops = std::move(ops);
    return rv;
  }

  // A temporary reference to place `p` (of type `ty`).
  Operand borrow(Place p, Type *ty, bool mut, SourceLoc loc) {
    Type *rt = tc_.ref(ty, mut);
    int r = newTemp(rt, loc);
    Rvalue ref;
    ref.kind = Rvalue::Ref;
    ref.mut = mut;
    ref.place = std::move(p);
    ref.type = rt;
    assign(Place{r, {}}, std::move(ref), loc);
    Operand o;
    o.kind = mut ? Operand::Move : Operand::Copy;
    o.place.local = r;
    o.type = rt;
    return o;
  }

  // The enum place an expression refers to, looking through one reference.
  Place enumPlace(const Expr *e) {
    Place p = placeOrTemp(e);
    if (e->type->isRef())
      p.proj.push_back({Proj::Deref});
    return p;
  }

  Operand discriminant(Place p, SourceLoc loc) {
    int t = newTemp(tc_.intTy(), loc);
    Rvalue rv;
    rv.kind = Rvalue::Discriminant;
    rv.place = std::move(p);
    rv.type = tc_.intTy();
    assign(Place{t, {}}, std::move(rv), loc);
    return copyOf(Place{t, {}}, tc_.intTy());
  }

  Operand isVariant(Operand disc, int variant, SourceLoc loc) {
    int c = newTemp(tc_.boolTy(), loc);
    Rvalue eq;
    eq.kind = Rvalue::BinaryOp;
    eq.bop = BinOp::Eq;
    eq.type = tc_.boolTy();
    eq.ops.push_back(std::move(disc));
    eq.ops.push_back(constInt(variant, tc_.intTy()));
    assign(Place{c, {}}, std::move(eq), loc);
    return copyOf(Place{c, {}}, tc_.boolTy());
  }

  // Assigns `ok` (no value) to the return slot of a function returning `!`.
  void assignOkVoid(SourceLoc loc) {
    Rvalue ok;
    ok.kind = Rvalue::Aggregate;
    ok.variant = 0;
    ok.type = f_.locals[0].type;
    assign(Place{0, {}}, std::move(ok), loc);
  }
  bool returnsOkVoid() const {
    Type *r = f_.locals[0].type;
    return r->isResult() && r->en->resultOf->kind == TypeKind::Void;
  }

  // `try x`: unwrap the value, or return the error (or none) to our caller.
  void lowerTry(Place dest, const UnaryExpr *u) {
    Type *ty = u->operand->type;
    EnumInfo *en = ty->en;
    int t = newTemp(ty, u->loc);
    lowerInto(Place{t, {}}, u->operand.get());
    Operand failed = isVariant(discriminant(Place{t, {}}, u->loc), en->failVariant(), u->loc);
    int failBB = newBlock(), okBB = newBlock();
    branch(failed, failBB, okBB, u->loc);

    cur_ = failBB;
    Type *retTy = f_.locals[0].type;
    Rvalue out;
    out.kind = Rvalue::Aggregate;
    out.type = retTy;
    out.variant = retTy->en->failVariant();
    if (en->resultOf) {
      Operand e;
      e.kind = Operand::Move; // taking the error consumes the temporary
      e.place = Place{t, {}}.withProj({Proj::VariantField, 0, -1, 1});
      e.type = tc_.errorTy();
      out.ops.push_back(e);
    }
    assign(Place{0, {}}, std::move(out), u->loc);
    exitScopes(0, u->loc);
    gotoBlock(returnBB_, u->loc);

    cur_ = okBB;
    Type *vt = en->valueType();
    if (vt->kind != TypeKind::Void) {
      Operand v;
      v.kind = vt->isCopy() ? Operand::Copy : Operand::Move;
      v.place = Place{t, {}}.withProj({Proj::VariantField, 0, -1, en->valueVariant()});
      v.type = vt;
      assign(dest, use(v), u->loc);
    }
  }

  // `opt or fallback`
  void lowerOrElse(Place dest, const BinaryExpr *b) {
    Type *optTy = b->lhs->type->derefAll();
    Type *inner = optTy->en->valueType();
    int valueV = optTy->en->valueVariant();
    if (b->orBorrows || b->lhs->type->isRef()) {
      // Look into the optional in place.
      Place p = enumPlace(b->lhs.get());
      Operand c = isVariant(discriminant(p, b->loc), valueV, b->loc);
      int someBB = newBlock(), noneBB = newBlock(), join = newBlock();
      branch(c, someBB, noneBB, b->loc);
      cur_ = someBB;
      Place payload = p.withProj({Proj::VariantField, 0, -1, valueV});
      if (b->orBorrows && !inner->isRef()) {
        Rvalue ref;
        ref.kind = Rvalue::Ref;
        ref.place = payload;
        ref.type = b->type;
        assign(dest, std::move(ref), b->loc);
      } else {
        assign(dest, use(useOf(payload, inner, b->loc)), b->loc);
      }
      gotoBlock(join, b->loc);
      cur_ = noneBB;
      lowerInto(dest, b->rhs.get());
      gotoBlock(join, b->loc);
      cur_ = join;
      return;
    }
    int t = newTemp(optTy, b->loc);
    lowerInto(Place{t, {}}, b->lhs.get());
    Operand c = isVariant(discriminant(Place{t, {}}, b->loc), valueV, b->loc);
    int someBB = newBlock(), noneBB = newBlock(), join = newBlock();
    branch(c, someBB, noneBB, b->loc);
    cur_ = someBB;
    Operand val;
    if (b->type == optTy) {
      val.kind = optTy->isCopy() ? Operand::Copy : Operand::Move;
      val.place = Place{t, {}};
      val.type = optTy;
    } else {
      // Taking the payload consumes the whole temporary.
      val.kind = inner->isCopy() ? Operand::Copy : Operand::Move;
      val.place = Place{t, {}}.withProj({Proj::VariantField, 0, -1, valueV});
      val.type = inner;
    }
    assign(dest, use(val), b->loc);
    gotoBlock(join, b->loc);
    cur_ = noneBB;
    lowerInto(dest, b->rhs.get());
    gotoBlock(join, b->loc);
    cur_ = join;
  }

  void lowerInto(Place dest, const Expr *e) {
    SourceLoc loc = e->loc;
    switch (e->kind) {
    case ExprKind::NoneLit: {
      Operand z;
      z.kind = Operand::Const;
      z.c.kind = Constant::Zero; // variant 0 (none) with no payload
      z.type = e->type;
      assign(dest, use(z), loc);
      return;
    }
    case ExprKind::EnumLit: {
      auto *el = static_cast<const EnumLitExpr *>(e);
      Rvalue rv;
      rv.kind = Rvalue::Aggregate;
      rv.variant = el->variant;
      rv.type = e->type;
      for (auto &a : el->args)
        rv.ops.push_back(lowerArg(a.get()));
      assign(dest, std::move(rv), loc);
      return;
    }
    case ExprKind::IntLit:
    case ExprKind::FloatLit:
    case ExprKind::BoolLit:
    case ExprKind::NilLit:
    case ExprKind::FuncRef:
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
    case ExprKind::Index:
      if (static_cast<const IndexExpr *>(e)->mode == IndexExpr::MapRead) {
        auto *ie = static_cast<const IndexExpr *>(e);
        Place p = placeOrTemp(ie->base.get());
        if (ie->autoDeref)
          p.proj.push_back({Proj::Deref});
        Operand m = borrow(p, ie->base->type->derefAll(), false, loc);
        Operand key = lowerArg(ie->index.get());
        if (!ie->ownedRead) {
          assign(dest, builtinCall(BuiltinOp::MapGet, e->type, {m, key}), loc);
          return;
        }
        // `?V` from the `?&V` lookup: some(copy of the value), or none.
        Type *vt = e->type->en->optionalOf;
        Type *found = tc_.optional(tc_.ref(vt, false));
        int r = newTemp(found, loc);
        assign(Place{r, {}}, builtinCall(BuiltinOp::MapGet, found, {m, key}), loc);
        Operand has = isVariant(discriminant(Place{r, {}}, loc), 1, loc);
        int someBB = newBlock(), noneBB = newBlock(), join = newBlock();
        branch(has, someBB, noneBB, loc);
        cur_ = someBB;
        Place val = Place{r, {}}.withProj({Proj::VariantField, 0, -1, 1}).withProj({Proj::Deref});
        Rvalue some;
        some.kind = Rvalue::Aggregate;
        some.variant = 1;
        some.type = e->type;
        some.ops.push_back(useOf(val, vt, loc));
        assign(dest, std::move(some), loc);
        gotoBlock(join, loc);
        cur_ = noneBB;
        Operand z;
        z.kind = Operand::Const;
        z.c.kind = Constant::Zero;
        z.type = e->type;
        assign(dest, use(z), loc);
        gotoBlock(join, loc);
        cur_ = join;
        return;
      }
      [[fallthrough]];
    case ExprKind::Ident:
    case ExprKind::Field:
      assign(dest, use(useOf(lowerPlace(e), e->type, loc)), loc);
      return;
    case ExprKind::MapLit: {
      auto *ml = static_cast<const MapLitExpr *>(e);
      Operand z;
      z.kind = Operand::Const;
      z.c.kind = Constant::Zero; // an empty map
      z.type = e->type;
      assign(dest, use(z), loc);
      for (auto &[k, v] : ml->entries) {
        Operand key = lowerArg(k.get());
        Operand val = lowerArg(v.get());
        int r = newTemp(tc_.ref(e->type->inner, true), loc);
        assign(Place{r, {}},
               builtinCall(BuiltinOp::MapSlot, f_.locals[r].type, {borrow(dest, e->type, true, loc), key}), loc);
        assign(Place{r, {}}.withProj({Proj::Deref}), use(val), loc);
      }
      return;
    }
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
      case UnOp::BitNot:
        rv.kind = Rvalue::UnaryOp;
        rv.uop = u->op;
        rv.ops.push_back(lowerOperand(u->operand.get()));
        break;
      case UnOp::Ref:
      case UnOp::RefMut:
        rv.kind = Rvalue::Ref;
        rv.mut = u->op == UnOp::RefMut;
        if (extendScope_ >= 0 && !isPlaceExpr(u->operand.get())) {
          int t = newLocal("", u->operand->type, u->operand->loc, extendScope_);
          lowerInto(Place{t, {}}, u->operand.get());
          rv.place = Place{t, {}};
        } else {
          rv.place = placeOrTemp(u->operand.get());
        }
        break;
      case UnOp::ReborrowShared:
      case UnOp::ReborrowMut:
        rv.kind = Rvalue::Ref;
        rv.mut = u->op == UnOp::ReborrowMut;
        rv.place = placeOrTemp(u->operand.get());
        rv.place.proj.push_back({Proj::Deref});
        break;
      case UnOp::Try:
        lowerTry(dest, u);
        return;
      case UnOp::Copy:
        assign(dest, use(cloneOf(lowerPlace(u->operand.get()), e->type, loc)), loc);
        return;
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
      if (b->op == BinOp::OrElse) {
        lowerOrElse(dest, b);
        return;
      }
      Rvalue rv;
      rv.type = e->type;
      if (b->noneCheck) {
        // Compares which variant is active (`opt == none`, `dir == North`).
        auto disc = [&](const Expr *x) {
          if (x->kind == ExprKind::NoneLit)
            return constInt(0, tc_.intTy());
          return discriminant(enumPlace(x), x->loc);
        };
        rv.kind = Rvalue::BinaryOp;
        rv.bop = b->op;
        rv.ops.push_back(disc(b->lhs.get()));
        rv.ops.push_back(disc(b->rhs.get()));
        assign(dest, std::move(rv), loc);
        return;
      }
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
      if (sl->arrayLen >= 0) { // [N]T{...}: the given elements, then zeros
        Rvalue rv;
        rv.kind = Rvalue::Aggregate;
        rv.type = e->type;
        for (auto &el : sl->elems)
          rv.ops.push_back(lowerArg(el.get()));
        while ((int64_t)rv.ops.size() < sl->arrayLen) {
          Operand z;
          z.kind = Operand::Const;
          z.c.kind = Constant::Zero;
          z.type = e->type->inner;
          rv.ops.push_back(z);
        }
        assign(dest, std::move(rv), loc);
        return;
      }
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
      case Builtin::Convert: rv.builtin = BuiltinOp::Convert; break;
      case Builtin::CStr: rv.builtin = BuiltinOp::CStr; break;
      case Builtin::ToStr: rv.builtin = BuiltinOp::ToStr; break;
      case Builtin::Panic: rv.builtin = BuiltinOp::Panic; break;
      case Builtin::MakeError: rv.builtin = BuiltinOp::MakeError; break;
      case Builtin::Delete: rv.builtin = BuiltinOp::MapDelete; break;
      case Builtin::SizeOf: // a constant by now
      case Builtin::None: break;
      }
      for (auto &a : c->args)
        rv.ops.push_back(lowerArg(a.get()));
      return rv;
    }
    rv.kind = Rvalue::Call;
    if (c->indirect) {
      rv.ops.push_back(lowerArg(c->callee.get()));
      for (auto &a : c->args)
        rv.ops.push_back(lowerArg(a.get()));
      return rv;
    }
    rv.func = c->func;
    rv.ops.resize(c->args.size());
    // Arguments a function may change are lent last, so the others can still
    // read them first: `push(v, len(v))`, `v.add(v[0])`.
    size_t first = c->receiverLast ? 1 : 0;
    auto changes = [&](size_t i) { return i < c->func->params.size() && c->func->params[i]->isMutRef(); };
    for (size_t i = first; i < c->args.size(); i++)
      if (!changes(i))
        rv.ops[i] = lowerArg(c->args[i].get());
    if (c->receiverLast)
      rv.ops[0] = lowerArg(c->args[0].get());
    for (size_t i = first; i < c->args.size(); i++)
      if (changes(i))
        rv.ops[i] = lowerArg(c->args[i].get());
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
    case StmtKind::Switch:
      lowerSwitch(static_cast<const SwitchStmt &>(s));
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
      // `x := &value()` or `x := opt or "default"` keep the temporary alive
      // as long as the block.
      if (vd.init->type->containsRef())
        extendScope_ = blockScope;
      lowerInto(Place{l, {}}, vd.init.get());
      extendScope_ = -1;
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
      Type *ty = st.target->type; // int or a sized integer
      rv.type = ty;
      rv.ops.push_back(copyOf(p, ty));
      rv.ops.push_back(constInt(1, ty));
      assign(p, std::move(rv), st.loc);
      return;
    }
    case StmtKind::Return: {
      auto &rs = static_cast<const ReturnStmt &>(s);
      if (rs.value)
        lowerInto(Place{0, {}}, rs.value.get());
      else if (returnsOkVoid())
        assignOkVoid(rs.loc);
      exitScopes(0, rs.loc);
      gotoBlock(returnBB_, rs.loc);
      cur_ = newBlock();
      return;
    }
    case StmtKind::Break:
    case StmtKind::Continue: {
      const Loop *lp = &loops_.back();
      if (s.kind == StmtKind::Continue)
        for (auto it = loops_.rbegin(); it != loops_.rend(); ++it)
          if (!it->isSwitch) {
            lp = &*it;
            break;
          }
      exitScopes(lp->scopeDepth, s.loc);
      gotoBlock(s.kind == StmtKind::Break ? lp->breakBB : lp->continueBB, s.loc);
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
    case AssignOp::BitAnd: rv.bop = BinOp::BitAnd; break;
    case AssignOp::BitOr: rv.bop = BinOp::BitOr; break;
    case AssignOp::BitXor: rv.bop = BinOp::BitXor; break;
    case AssignOp::AndNot: rv.bop = BinOp::AndNot; break;
    case AssignOp::Shl: rv.bop = BinOp::Shl; break;
    case AssignOp::Shr: rv.bop = BinOp::Shr; break;
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

  void lowerSwitch(const SwitchStmt &sw) {
    pushScope();
    int join = newBlock();
    std::vector<int> bodies;
    int defaultBody = -1;
    for (auto &c : sw.cases) {
      bodies.push_back(newBlock());
      if (c.isDefault)
        defaultBody = bodies.back();
    }
    Operand tag;
    Place enumP;
    bool strTag = false;
    if (sw.mode == SwitchStmt::Enum) {
      enumP = enumPlace(sw.tag.get());
      tag = discriminant(enumP, sw.tag->loc);
    } else if (sw.mode == SwitchStmt::Values) {
      strTag = sw.tag->type->isRef();
      tag = lowerArg(sw.tag.get());
    }
    // Tests, in source order.
    for (size_t i = 0; i < sw.cases.size(); i++) {
      const SwitchCase &c = sw.cases[i];
      auto test = [&](Operand cond, SourceLoc loc) {
        int next = newBlock();
        branch(std::move(cond), bodies[i], next, loc);
        cur_ = next;
      };
      if (sw.mode == SwitchStmt::Enum) {
        for (int v : c.variants)
          test(isVariant(tag, v, c.loc), c.loc);
        continue;
      }
      for (auto &v : c.values) {
        if (sw.mode == SwitchStmt::Conditions) {
          test(lowerCond(v.get()), v->loc);
          continue;
        }
        int cb = newTemp(tc_.boolTy(), v->loc);
        Rvalue eq;
        eq.type = tc_.boolTy();
        eq.ops.push_back(tag);
        eq.ops.push_back(lowerOperand(v.get()));
        if (strTag) {
          eq.kind = Rvalue::Builtin;
          eq.builtin = BuiltinOp::StrCmp;
          eq.cmp = BinOp::Eq;
        } else {
          eq.kind = Rvalue::BinaryOp;
          eq.bop = BinOp::Eq;
        }
        assign(Place{cb, {}}, std::move(eq), v->loc);
        test(copyOf(Place{cb, {}}, tc_.boolTy()), v->loc);
      }
    }
    if (defaultBody >= 0) {
      gotoBlock(defaultBody, sw.loc);
    } else if (sw.exhaustive) {
      Terminator t;
      t.kind = Terminator::Unreachable;
      t.loc = sw.loc;
      terminate(t);
    } else {
      gotoBlock(join, sw.loc);
    }

    loops_.push_back({join, -1, scopes_.size(), true});
    for (size_t i = 0; i < sw.cases.size(); i++) {
      const SwitchCase &c = sw.cases[i];
      cur_ = bodies[i];
      pushScope();
      if (sw.mode == SwitchStmt::Enum && c.variants.size() == 1) {
        int vi = c.variants[0];
        const Variant &var = sw.tag->type->derefAll()->en->variants[vi];
        for (size_t j = 0; j < c.bindings.size(); j++) {
          LocalVar *lv = c.bindings[j];
          if (!lv)
            continue;
          int l = newLocal(lv->name, lv->type, lv->loc);
          vars_[lv] = l;
          Place field = enumP.withProj({Proj::VariantField, (int)j, -1, vi});
          if (c.bindByRef[j]) {
            Rvalue ref;
            ref.kind = Rvalue::Ref;
            ref.place = field;
            ref.type = lv->type;
            assign(Place{l, {}}, std::move(ref), lv->loc);
          } else {
            assign(Place{l, {}}, use(copyOf(field, var.fields[j])), lv->loc);
          }
        }
      }
      for (auto &st : c.body)
        lowerStmt(*st);
      SourceLoc end = i + 1 < sw.cases.size() ? sw.cases[i + 1].loc : c.loc;
      popScope(end);
      gotoBlock(join, end);
    }
    loops_.pop_back();
    cur_ = join;
    popScope(sw.loc);
  }

  void lowerForRange(const ForRangeStmt &fr) {
    pushScope();
    Type *intTy = tc_.intTy();
    int n = newTemp(intTy, fr.range->loc);
    int slice = -1;
    if (fr.overMap) {
      slice = newTemp(fr.range->type, fr.range->loc);
      lowerInto(Place{slice, {}}, fr.range.get());
      assign(Place{n, {}}, builtinCall(BuiltinOp::MapUsed, intTy, {copyOf(Place{slice, {}}, fr.range->type)}),
             fr.range->loc);
    } else if (fr.overSlice) {
      // The slice stays borrowed for the whole loop when elements are used.
      slice = newTemp(fr.range->type, fr.range->loc);
      lowerInto(Place{slice, {}}, fr.range.get());
      Rvalue len;
      len.kind = Rvalue::Builtin;
      len.builtin = BuiltinOp::Len;
      len.type = intTy;
      len.ops.push_back(copyOf(Place{slice, {}}, fr.range->type));
      assign(Place{n, {}}, std::move(len), fr.range->loc);
    } else {
      lowerInto(Place{n, {}}, fr.range.get());
    }
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
    if (fr.overMap) {
      // Skip deleted entries; bind the key and (optionally) the value.
      Operand rop = copyOf(Place{slice, {}}, fr.range->type);
      int alive = newTemp(tc_.boolTy(), fr.loc);
      assign(Place{alive, {}}, builtinCall(BuiltinOp::MapAlive, tc_.boolTy(), {rop, copyOf(Place{i, {}}, intTy)}),
             fr.loc);
      int liveBB = newBlock();
      branch(copyOf(Place{alive, {}}, tc_.boolTy()), liveBB, cont, fr.loc);
      cur_ = liveBB;
      int k = newLocal(fr.name, fr.var->type, fr.nameLoc);
      vars_[fr.var] = k;
      assign(Place{k, {}}, builtinCall(BuiltinOp::MapKeyAt, fr.var->type, {rop, copyOf(Place{i, {}}, intTy)}),
             fr.nameLoc);
      if (fr.valueVar) {
        int x = newLocal(fr.valueName, fr.valueVar->type, fr.valueLoc);
        vars_[fr.valueVar] = x;
        assign(Place{x, {}},
               builtinCall(BuiltinOp::MapValAt, fr.valueVar->type, {rop, copyOf(Place{i, {}}, intTy)}),
               fr.valueLoc);
      }
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
      return;
    }
    int v = newLocal(fr.name, intTy, fr.nameLoc);
    vars_[fr.var] = v;
    assign(Place{v, {}}, use(copyOf(Place{i, {}}, intTy)), fr.nameLoc);
    if (fr.valueVar) {
      Type *vt = fr.valueVar->type;
      int x = newLocal(fr.valueName, vt, fr.valueLoc);
      vars_[fr.valueVar] = x;
      Place elem = Place{slice, {}}.withProj({Proj::Deref}).withProj({Proj::Index, -1, i});
      if (fr.valueByRef) {
        Rvalue ref;
        ref.kind = Rvalue::Ref;
        ref.place = elem;
        ref.type = vt;
        assign(Place{x, {}}, std::move(ref), fr.valueLoc);
      } else {
        assign(Place{x, {}}, use(copyOf(elem, vt)), fr.valueLoc);
      }
    }
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

mir::Module buildMir(Program &prog, TypeContext &tc) {
  mir::Module m;
  for (auto &fd : prog.funcs) {
    if (!fd->info || fd->isExtern)
      continue;
    FnBuilder b(*fd, tc);
    m.funcs.push_back(b.build());
  }
  return m;
}

} // namespace co
