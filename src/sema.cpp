#include "sema.h"

#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace co {
namespace {

const std::unordered_map<std::string, Builtin> kBuiltins = {
    {"print", Builtin::Print},   {"println", Builtin::Println}, {"len", Builtin::Len},
    {"append", Builtin::Append}, {"clone", Builtin::Clone},     {"int", Builtin::ToInt},
    {"float", Builtin::ToFloat}, {"str", Builtin::ToStr},       {"panic", Builtin::Panic},
};

bool isPlace(const Expr *e) {
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

std::string exprStr(const Expr *e) {
  switch (e->kind) {
  case ExprKind::Ident:
    return static_cast<const IdentExpr *>(e)->name;
  case ExprKind::Field: {
    auto *f = static_cast<const FieldExpr *>(e);
    return exprStr(f->base.get()) + "." + f->name;
  }
  case ExprKind::Index:
    return exprStr(static_cast<const IndexExpr *>(e)->base.get()) + "[..]";
  case ExprKind::Unary: {
    auto *u = static_cast<const UnaryExpr *>(e);
    if (u->op == UnOp::Deref)
      return "*" + exprStr(u->operand.get());
    return "expression";
  }
  default:
    return "expression";
  }
}

bool samePlace(const Expr *a, const Expr *b) {
  if (a->kind != b->kind)
    return false;
  switch (a->kind) {
  case ExprKind::Ident:
    return static_cast<const IdentExpr *>(a)->var == static_cast<const IdentExpr *>(b)->var;
  case ExprKind::Field: {
    auto *fa = static_cast<const FieldExpr *>(a);
    auto *fb = static_cast<const FieldExpr *>(b);
    return fa->name == fb->name && samePlace(fa->base.get(), fb->base.get());
  }
  case ExprKind::Unary: {
    auto *ua = static_cast<const UnaryExpr *>(a);
    auto *ub = static_cast<const UnaryExpr *>(b);
    return ua->op == UnOp::Deref && ub->op == UnOp::Deref &&
           samePlace(ua->operand.get(), ub->operand.get());
  }
  default:
    return false;
  }
}

class Sema {
public:
  Sema(Program &p, TypeContext &tc, Diagnostics &d) : prog_(p), tc_(tc), diag_(d) {}

  void run() {
    declareStructs();
    declareFuncs();
    for (auto &fd : prog_.funcs)
      if (fd->info)
        checkFunc(*fd);
  }

private:
  Program &prog_;
  TypeContext &tc_;
  Diagnostics &diag_;
  std::unordered_map<std::string, StructInfo *> structs_;
  std::unordered_map<std::string, FuncInfo *> funcs_;

  FuncDecl *fn_ = nullptr;
  std::vector<std::unordered_map<std::string, LocalVar *>> scopes_;
  int loopDepth_ = 0;

  void error(SourceLoc l, std::string msg) { diag_.error(l, std::move(msg)); }

  // ----- types -----

  Type *resolveType(const TypeExpr &t) {
    switch (t.kind) {
    case TypeExpr::Name: {
      if (t.name == "int") return tc_.intTy();
      if (t.name == "float") return tc_.floatTy();
      if (t.name == "bool") return tc_.boolTy();
      if (t.name == "string") return tc_.stringTy();
      auto it = structs_.find(t.name);
      if (it == structs_.end()) {
        error(t.loc, "unknown type '" + t.name + "'");
        return nullptr;
      }
      return tc_.structTy(it->second);
    }
    case TypeExpr::Ref: {
      Type *inner = resolveType(*t.inner);
      if (!inner)
        return nullptr;
      if (inner->isRef()) {
        error(t.loc, "references to references are not supported");
        return nullptr;
      }
      return tc_.ref(inner, t.mut);
    }
    case TypeExpr::Slice: {
      Type *inner = resolveType(*t.inner);
      if (!inner)
        return nullptr;
      if (inner->isRef()) {
        error(t.loc, "slices cannot hold references (borrowed data can't be stored in containers)");
        return nullptr;
      }
      return tc_.slice(inner);
    }
    }
    return nullptr;
  }

  void declareStructs() {
    for (auto &sd : prog_.structs) {
      if (structs_.count(sd->name) || sd->name == "int" || sd->name == "float" ||
          sd->name == "bool" || sd->name == "string") {
        error(sd->loc, "type '" + sd->name + "' is already defined");
        continue;
      }
      auto info = std::make_unique<StructInfo>();
      info->name = sd->name;
      info->loc = sd->loc;
      sd->info = info.get();
      structs_[sd->name] = info.get();
      prog_.structInfos.push_back(std::move(info));
    }
    for (auto &sd : prog_.structs) {
      if (!sd->info)
        continue;
      for (auto &f : sd->fields) {
        Type *ty = resolveType(*f.type);
        if (!ty)
          continue;
        if (ty->isRef()) {
          error(f.loc, "struct fields cannot be references (borrowed data can't be stored in structs); "
                       "store an owned value instead");
          continue;
        }
        if (sd->info->fieldIndex.count(f.name)) {
          error(f.loc, "duplicate field '" + f.name + "'");
          continue;
        }
        sd->info->fieldIndex[f.name] = (int)sd->info->fields.size();
        sd->info->fields.push_back({f.name, ty, f.loc});
      }
    }
    // Reject infinitely sized structs and compute which ones need drop glue.
    std::unordered_map<StructInfo *, int> state; // 0 = new, 1 = visiting, 2 = done
    std::function<bool(StructInfo *)> visit = [&](StructInfo *st) -> bool {
      if (state[st] == 2)
        return true;
      if (state[st] == 1) {
        error(st->loc, "recursive struct '" + st->name +
                           "' has infinite size; use a slice ([]" + st->name + ") for indirection");
        return false;
      }
      state[st] = 1;
      bool ok = true;
      st->needsDrop = false;
      for (auto &f : st->fields) {
        if (f.type->kind == TypeKind::Struct) {
          ok = visit(f.type->st) && ok;
          if (f.type->st->needsDrop)
            st->needsDrop = true;
        } else if (f.type->needsDrop()) {
          st->needsDrop = true;
        }
      }
      state[st] = 2;
      return ok;
    };
    for (auto &info : prog_.structInfos)
      if (!visit(info.get())) {
        info->fields.clear(); // prevent further cascades
        info->fieldIndex.clear();
      }
  }

  void declareFuncs() {
    for (auto &fd : prog_.funcs) {
      auto info = std::make_unique<FuncInfo>();
      info->name = fd->name;
      info->loc = fd->loc;
      info->decl = fd.get();
      bool ok = true;
      int refParams = 0;
      if (fd->receiver) {
        Type *rt = resolveType(*fd->receiver->type);
        if (rt && rt->derefAll()->kind != TypeKind::Struct) {
          error(fd->receiver->loc, "method receiver must be a struct type T, &T or &mut T");
          rt = nullptr;
        }
        if (!rt) {
          ok = false;
        } else {
          info->recvStruct = rt->derefAll()->st;
          info->params.push_back(rt);
          info->paramNames.push_back(fd->receiver->name);
          refParams += rt->isRef();
        }
      }
      for (auto &p : fd->params) {
        Type *pt = resolveType(*p.type);
        if (!pt)
          ok = false;
        else
          refParams += pt->isRef();
        info->params.push_back(pt);
        info->paramNames.push_back(p.name);
      }
      info->ret = tc_.voidTy();
      if (fd->ret) {
        info->ret = resolveType(*fd->ret);
        if (!info->ret) {
          ok = false;
        } else if (info->ret->isRef() && refParams == 0) {
          error(fd->ret->loc, "function returns a reference but has no reference parameters to borrow "
                              "from; return an owned value instead");
          ok = false;
        }
      }
      if (!ok)
        continue;

      if (info->recvStruct) {
        StructInfo *st = info->recvStruct;
        if (st->methods.count(fd->name)) {
          error(fd->loc, "method '" + fd->name + "' is already defined for '" + st->name + "'");
          continue;
        }
        if (st->fieldIndex.count(fd->name)) {
          error(fd->loc, "'" + st->name + "' has both a field and a method named '" + fd->name + "'");
          continue;
        }
        info->symbol = "co." + st->name + "." + fd->name;
        st->methods[fd->name] = info.get();
      } else {
        if (kBuiltins.count(fd->name)) {
          error(fd->loc, "cannot redefine builtin function '" + fd->name + "'");
          continue;
        }
        if (funcs_.count(fd->name)) {
          error(fd->loc, "function '" + fd->name + "' is already defined");
          continue;
        }
        if (fd->name == "main") {
          info->symbol = "co_main";
          if (!info->params.empty() || info->ret->kind != TypeKind::Void)
            error(fd->loc, "func main must take no parameters and return nothing");
        } else {
          info->symbol = "co." + fd->name;
        }
        funcs_[fd->name] = info.get();
      }
      fd->info = info.get();
      prog_.funcInfos.push_back(std::move(info));
    }
    if (!funcs_.count("main"))
      error({1, 1}, "program has no 'func main()'");
  }

  // ----- scopes -----

  LocalVar *newVar(const std::string &name, Type *ty, SourceLoc loc) {
    auto v = std::make_unique<LocalVar>();
    v->name = name;
    v->type = ty;
    v->loc = loc;
    LocalVar *raw = v.get();
    fn_->locals.push_back(std::move(v));
    return raw;
  }

  void declare(LocalVar *v) {
    auto &scope = scopes_.back();
    if (scope.count(v->name))
      error(v->loc, "'" + v->name + "' is already declared in this scope");
    scope[v->name] = v;
  }

  LocalVar *lookup(const std::string &name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto f = it->find(name);
      if (f != it->end())
        return f->second;
    }
    return nullptr;
  }

  // ----- functions & statements -----

  void checkFunc(FuncDecl &fd) {
    fn_ = &fd;
    scopes_.clear();
    scopes_.emplace_back();
    FuncInfo *info = fd.info;
    size_t pi = 0;
    if (fd.receiver) {
      fd.receiver->var = newVar(fd.receiver->name, info->params[pi++], fd.receiver->loc);
      declare(fd.receiver->var);
    }
    for (auto &p : fd.params) {
      p.var = newVar(p.name, info->params[pi++], p.loc);
      declare(p.var);
    }
    checkBlock(*fd.body);
    scopes_.clear();
    fn_ = nullptr;
  }

  void checkBlock(BlockStmt &b) {
    scopes_.emplace_back();
    for (auto &s : b.stmts)
      checkStmt(*s);
    scopes_.pop_back();
  }

  void checkStmt(Stmt &s) {
    switch (s.kind) {
    case StmtKind::Block:
      checkBlock(static_cast<BlockStmt &>(s));
      break;
    case StmtKind::VarDecl: {
      auto &vd = static_cast<VarDeclStmt &>(s);
      Type *ty = nullptr;
      if (vd.typeExpr)
        ty = resolveType(*vd.typeExpr);
      if (vd.init) {
        Type *it = check(vd.init);
        if (ty && it)
          coerce(vd.init, ty);
        else if (!vd.typeExpr)
          ty = it;
        if (ty && ty->kind == TypeKind::Void) {
          error(vd.init->loc, "expression produces no value");
          ty = nullptr;
        }
      } else if (ty && ty->isRef()) {
        error(vd.loc, "reference variable '" + vd.name + "' must be initialized");
      }
      vd.var = newVar(vd.name, ty, vd.loc);
      declare(vd.var);
      break;
    }
    case StmtKind::Expr: {
      auto &es = static_cast<ExprStmt &>(s);
      if (es.expr->kind != ExprKind::Call) {
        check(es.expr);
        error(es.expr->loc, "expression is not used (only calls can be statements)");
        break;
      }
      check(es.expr);
      auto *call = static_cast<CallExpr *>(es.expr.get());
      if (call->builtin == Builtin::Append)
        error(call->loc, "result of append must be assigned: write 'v = append(v, x)'");
      break;
    }
    case StmtKind::Assign:
      checkAssign(static_cast<AssignStmt &>(s));
      break;
    case StmtKind::IncDec: {
      auto &st = static_cast<IncDecStmt &>(s);
      Type *t = check(st.target);
      if (!t)
        break;
      if (!isPlace(st.target.get())) {
        error(st.loc, "cannot increment/decrement this expression");
        break;
      }
      checkMutablePlace(st.target.get(), "modify");
      if (t->kind != TypeKind::Int)
        error(st.loc, "'++' and '--' need an int, found '" + t->str() + "'");
      break;
    }
    case StmtKind::If: {
      auto &is = static_cast<IfStmt &>(s);
      checkCond(is.cond);
      checkBlock(*is.then);
      if (is.els)
        checkStmt(*is.els);
      break;
    }
    case StmtKind::For: {
      auto &fs = static_cast<ForStmt &>(s);
      scopes_.emplace_back();
      if (fs.init)
        checkStmt(*fs.init);
      if (fs.cond)
        checkCond(fs.cond);
      if (fs.post)
        checkStmt(*fs.post);
      loopDepth_++;
      checkBlock(*fs.body);
      loopDepth_--;
      scopes_.pop_back();
      break;
    }
    case StmtKind::ForRange: {
      auto &fr = static_cast<ForRangeStmt &>(s);
      Type *t = check(fr.range);
      if (t && t->kind != TypeKind::Int)
        error(fr.range->loc, "range expects an int (iterates 0..n-1), found '" + t->str() + "'");
      scopes_.emplace_back();
      fr.var = newVar(fr.name, tc_.intTy(), fr.nameLoc);
      declare(fr.var);
      loopDepth_++;
      checkBlock(*fr.body);
      loopDepth_--;
      scopes_.pop_back();
      break;
    }
    case StmtKind::Return: {
      auto &rs = static_cast<ReturnStmt &>(s);
      Type *ret = fn_->info->ret;
      if (rs.value) {
        Type *t = check(rs.value);
        if (ret->kind == TypeKind::Void) {
          error(rs.value->loc, "function '" + fn_->name + "' does not return a value");
        } else if (t) {
          coerce(rs.value, ret);
        }
      } else if (ret->kind != TypeKind::Void) {
        error(rs.loc, "missing return value of type '" + ret->str() + "'");
      }
      break;
    }
    case StmtKind::Break:
    case StmtKind::Continue:
      if (loopDepth_ == 0)
        error(s.loc, std::string(s.kind == StmtKind::Break ? "break" : "continue") + " outside of a loop");
      break;
    }
  }

  void checkCond(ExprPtr &e) {
    Type *t = check(e);
    if (t && t->kind != TypeKind::Bool)
      error(e->loc, "condition must be a bool, found '" + t->str() + "'");
  }

  void checkAssign(AssignStmt &as) {
    Type *lt = check(as.lhs);
    if (!lt) {
      check(as.rhs);
      return;
    }
    if (!isPlace(as.lhs.get())) {
      error(as.lhs->loc, "cannot assign to this expression");
      check(as.rhs);
      return;
    }
    checkMutablePlace(as.lhs.get(), "assign to");

    if (as.op == AssignOp::Set) {
      if (as.rhs->kind == ExprKind::Call) {
        auto *call = static_cast<CallExpr *>(as.rhs.get());
        if (call->callee->kind == ExprKind::Ident &&
            static_cast<IdentExpr *>(call->callee.get())->name == "append" && call->args.size() == 2 &&
            !lookup("append")) {
          // Resolve the first argument to see whether it names the same place.
          Type *t0 = check(call->args[0]);
          if (t0 && samePlace(call->args[0].get(), as.lhs.get()))
            as.appendInPlace = true;
        }
      }
      Type *rt = check(as.rhs);
      if (rt)
        coerce(as.rhs, lt);
      return;
    }

    Type *rt = check(as.rhs);
    if (!rt)
      return;
    if (lt->kind == TypeKind::String) {
      if (as.op != AssignOp::Add) {
        error(as.loc, "only '+=' is supported for strings");
        return;
      }
      if (rt->derefAll()->kind != TypeKind::String) {
        error(as.rhs->loc, "cannot append '" + rt->str() + "' to a string; use str(x) to convert");
        return;
      }
      autoRefShared(as.rhs);
      return;
    }
    if (!lt->isNumeric()) {
      error(as.loc, "compound assignment needs an int or float, found '" + lt->str() + "'");
      return;
    }
    if (as.op == AssignOp::Rem && lt->kind != TypeKind::Int) {
      error(as.loc, "'%=' needs an int");
      return;
    }
    coerce(as.rhs, lt);
  }

  // Reports an error if `e` (a place) cannot be modified.
  bool checkMutablePlace(const Expr *e, const char *action) {
    switch (e->kind) {
    case ExprKind::Ident:
      return true;
    case ExprKind::Field:
    case ExprKind::Index: {
      const Expr *base;
      bool autoDeref;
      if (e->kind == ExprKind::Field) {
        base = static_cast<const FieldExpr *>(e)->base.get();
        autoDeref = static_cast<const FieldExpr *>(e)->autoDeref;
      } else {
        base = static_cast<const IndexExpr *>(e)->base.get();
        autoDeref = static_cast<const IndexExpr *>(e)->autoDeref;
      }
      if (autoDeref) {
        if (!base->type->mut) {
          error(e->loc, std::string("cannot ") + action + " '" + exprStr(e) + "' because '" +
                            exprStr(base) + "' is a shared reference '" + base->type->str() +
                            "' (use '&mut' to allow changes)");
          return false;
        }
        return true;
      }
      return isPlace(base) ? checkMutablePlace(base, action) : true;
    }
    case ExprKind::Unary: {
      auto *u = static_cast<const UnaryExpr *>(e);
      if (u->op == UnOp::Deref && !u->operand->type->mut) {
        error(e->loc, std::string("cannot ") + action + " '" + exprStr(e) + "' because '" +
                          exprStr(u->operand.get()) + "' is a shared reference '" +
                          u->operand->type->str() + "'");
        return false;
      }
      return true;
    }
    default:
      return true;
    }
  }

  // ----- expression helpers -----

  static ExprPtr wrap(ExprPtr e, UnOp op, Type *ty) {
    SourceLoc l = e->loc;
    auto u = std::make_unique<UnaryExpr>(l, op, std::move(e));
    u->type = ty;
    return u;
  }

  // Turns `e` into a shared reference: T -> &T, &mut T -> &T, &T unchanged.
  void autoRefShared(ExprPtr &e) {
    Type *t = e->type;
    if (t->isRef()) {
      if (t->mut)
        e = wrap(std::move(e), UnOp::ReborrowShared, tc_.ref(t->inner, false));
      return;
    }
    e = wrap(std::move(e), UnOp::Ref, tc_.ref(t, false));
  }

  // Implicit conversions at assignment/argument/return sites.
  bool coerce(ExprPtr &e, Type *target) {
    Type *t = e->type;
    if (!t || !target)
      return false;
    if (t == target) {
      // Passing a `&mut` place reborrows instead of moving the reference.
      if (t->isMutRef() && isPlace(e.get()))
        e = wrap(std::move(e), UnOp::ReborrowMut, t);
      return true;
    }
    if (t->isMutRef() && target->isRef() && !target->mut && t->inner == target->inner) {
      e = wrap(std::move(e), UnOp::ReborrowShared, target);
      return true;
    }
    std::string hint;
    if (target->isRef() && !t->isRef() && target->inner == t)
      hint = std::string(" (add '") + (target->mut ? "&mut" : "&") + "' to pass a reference)";
    else if (t->isRef() && !target->isRef() && t->inner == target)
      hint = target->isCopy() ? " (use '*' to dereference)" : " (use clone(x) to copy the value)";
    else if (t->isRef() && !t->mut && target->isMutRef() && t->inner == target->inner)
      hint = " (a shared reference cannot be turned into a mutable one)";
    error(e->loc, "mismatched types: expected '" + target->str() + "', found '" + t->str() + "'" + hint);
    return false;
  }

  Type *check(ExprPtr &e) {
    Type *t = checkInner(e);
    e->type = t;
    return t;
  }

  Type *checkInner(ExprPtr &e) {
    switch (e->kind) {
    case ExprKind::IntLit: return tc_.intTy();
    case ExprKind::FloatLit: return tc_.floatTy();
    case ExprKind::StrLit: return tc_.stringTy();
    case ExprKind::BoolLit: return tc_.boolTy();
    case ExprKind::Ident: {
      auto *id = static_cast<IdentExpr *>(e.get());
      if (id->var) // already resolved (e.g. append rewrite looked at it)
        return id->var->type;
      id->var = lookup(id->name);
      if (!id->var) {
        if (funcs_.count(id->name) || kBuiltins.count(id->name))
          error(id->loc, "function '" + id->name + "' can only be called");
        else
          error(id->loc, "undefined: '" + id->name + "'");
        return nullptr;
      }
      return id->var->type;
    }
    case ExprKind::Unary:
      return checkUnary(*static_cast<UnaryExpr *>(e.get()));
    case ExprKind::Binary:
      return checkBinary(*static_cast<BinaryExpr *>(e.get()));
    case ExprKind::Call:
      return checkCall(*static_cast<CallExpr *>(e.get()));
    case ExprKind::Field: {
      auto *fe = static_cast<FieldExpr *>(e.get());
      Type *bt = check(fe->base);
      if (!bt)
        return nullptr;
      fe->autoDeref = bt->isRef();
      Type *st = bt->derefAll();
      if (st->kind != TypeKind::Struct) {
        error(fe->loc, "type '" + bt->str() + "' has no field '" + fe->name + "'");
        return nullptr;
      }
      auto it = st->st->fieldIndex.find(fe->name);
      if (it == st->st->fieldIndex.end()) {
        if (st->st->methods.count(fe->name))
          error(fe->loc, "'" + fe->name + "' is a method; call it with " + fe->name + "()");
        else
          error(fe->loc, "struct '" + st->st->name + "' has no field '" + fe->name + "'");
        return nullptr;
      }
      fe->index = it->second;
      return st->st->fields[fe->index].type;
    }
    case ExprKind::Index: {
      auto *ie = static_cast<IndexExpr *>(e.get());
      Type *bt = check(ie->base);
      Type *it = check(ie->index);
      if (!bt || !it)
        return nullptr;
      ie->autoDeref = bt->isRef();
      Type *st = bt->derefAll();
      if (st->kind != TypeKind::Slice) {
        error(ie->loc, "cannot index a value of type '" + bt->str() + "'");
        return nullptr;
      }
      if (it->kind != TypeKind::Int) {
        error(ie->index->loc, "index must be an int, found '" + it->str() + "'");
        return nullptr;
      }
      return st->inner;
    }
    case ExprKind::StructLit: {
      auto *sl = static_cast<StructLitExpr *>(e.get());
      auto it = structs_.find(sl->name);
      if (it == structs_.end()) {
        error(sl->loc, "unknown struct type '" + sl->name + "'");
        for (auto &f : sl->fields)
          check(f.value);
        return nullptr;
      }
      sl->st = it->second;
      std::unordered_set<std::string> seen;
      for (auto &f : sl->fields) {
        Type *vt = check(f.value);
        auto fi = sl->st->fieldIndex.find(f.name);
        if (fi == sl->st->fieldIndex.end()) {
          error(f.loc, "struct '" + sl->name + "' has no field '" + f.name + "'");
          continue;
        }
        if (!seen.insert(f.name).second) {
          error(f.loc, "field '" + f.name + "' is given twice");
          continue;
        }
        f.index = fi->second;
        if (vt)
          coerce(f.value, sl->st->fields[f.index].type);
      }
      return tc_.structTy(sl->st);
    }
    case ExprKind::SliceLit: {
      auto *sl = static_cast<SliceLitExpr *>(e.get());
      Type *et = resolveType(*sl->elemType);
      for (auto &el : sl->elems)
        if (check(el) && et)
          coerce(el, et);
      if (!et)
        return nullptr;
      if (et->isRef()) {
        error(sl->loc, "slices cannot hold references");
        return nullptr;
      }
      return tc_.slice(et);
    }
    }
    return nullptr;
  }

  Type *checkUnary(UnaryExpr &u) {
    Type *t = check(u.operand);
    if (!t)
      return nullptr;
    switch (u.op) {
    case UnOp::Neg:
      if (!t->isNumeric()) {
        error(u.loc, "cannot negate '" + t->str() + "'");
        return nullptr;
      }
      return t;
    case UnOp::Not:
      if (t->kind != TypeKind::Bool) {
        error(u.loc, "'!' needs a bool, found '" + t->str() + "'");
        return nullptr;
      }
      return t;
    case UnOp::Deref:
      if (!t->isRef()) {
        error(u.loc, "cannot dereference '" + t->str() + "' (it is not a reference)");
        return nullptr;
      }
      return t->inner;
    case UnOp::Ref:
    case UnOp::RefMut:
      if (t->kind == TypeKind::Void) {
        error(u.loc, "cannot borrow a call that returns nothing");
        return nullptr;
      }
      if (t->isRef()) {
        error(u.loc, "cannot take a reference to a reference ('" + exprStr(u.operand.get()) +
                         "' is already '" + t->str() + "')");
        return nullptr;
      }
      if (u.op == UnOp::RefMut && isPlace(u.operand.get()))
        checkMutablePlace(u.operand.get(), "borrow as mutable");
      return tc_.ref(t, u.op == UnOp::RefMut);
    case UnOp::ReborrowShared:
    case UnOp::ReborrowMut:
      return u.type;
    }
    return nullptr;
  }

  Type *checkBinary(BinaryExpr &b) {
    Type *lt = check(b.lhs);
    Type *rt = check(b.rhs);
    if (!lt || !rt)
      return nullptr;
    auto mismatch = [&]() -> Type * {
      error(b.loc, "mismatched types '" + lt->str() + "' and '" + rt->str() + "' in binary operation");
      return nullptr;
    };
    switch (b.op) {
    case BinOp::And:
    case BinOp::Or:
      if (lt->kind != TypeKind::Bool || rt->kind != TypeKind::Bool) {
        error(b.loc, "'&&' and '||' need bool operands");
        return nullptr;
      }
      return tc_.boolTy();
    case BinOp::Add:
    case BinOp::Sub:
    case BinOp::Mul:
    case BinOp::Div:
    case BinOp::Rem:
      if (b.op == BinOp::Add && lt->derefAll()->kind == TypeKind::String &&
          rt->derefAll()->kind == TypeKind::String) {
        // String concatenation borrows both sides and produces a new string.
        autoRefShared(b.lhs);
        autoRefShared(b.rhs);
        return tc_.stringTy();
      }
      if (lt != rt)
        return mismatch();
      if (!lt->isNumeric()) {
        error(b.loc, "arithmetic needs int or float operands, found '" + lt->str() + "'");
        return nullptr;
      }
      if (b.op == BinOp::Rem && lt->kind != TypeKind::Int) {
        error(b.loc, "'%' needs int operands");
        return nullptr;
      }
      return lt;
    case BinOp::Eq:
    case BinOp::Ne:
    case BinOp::Lt:
    case BinOp::Le:
    case BinOp::Gt:
    case BinOp::Ge: {
      if (lt->derefAll()->kind == TypeKind::String && rt->derefAll()->kind == TypeKind::String) {
        autoRefShared(b.lhs);
        autoRefShared(b.rhs);
        return tc_.boolTy();
      }
      if (lt != rt)
        return mismatch();
      bool ordered = b.op != BinOp::Eq && b.op != BinOp::Ne;
      if (!(lt->isNumeric() || (!ordered && lt->kind == TypeKind::Bool))) {
        error(b.loc, "cannot compare values of type '" + lt->str() + "'");
        return nullptr;
      }
      return tc_.boolTy();
    }
    }
    return nullptr;
  }

  Type *checkCall(CallExpr &c) {
    if (c.callee->kind == ExprKind::Field)
      return checkMethodCall(c);
    if (c.callee->kind != ExprKind::Ident) {
      error(c.loc, "this expression cannot be called");
      return nullptr;
    }
    auto *id = static_cast<IdentExpr *>(c.callee.get());
    if (lookup(id->name)) {
      error(id->loc, "'" + id->name + "' is a variable, not a function");
      return nullptr;
    }
    auto bi = kBuiltins.find(id->name);
    if (bi != kBuiltins.end()) {
      c.builtin = bi->second;
      return checkBuiltin(c);
    }
    auto fi = funcs_.find(id->name);
    if (fi == funcs_.end()) {
      error(id->loc, "undefined function '" + id->name + "'");
      for (auto &a : c.args)
        check(a);
      return nullptr;
    }
    c.func = fi->second;
    if (c.func->name == "main") {
      error(id->loc, "main cannot be called");
      return nullptr;
    }
    checkArgs(c, 0);
    return c.func->ret;
  }

  // Checks c.args[first..] against c.func->params[first..].
  void checkArgs(CallExpr &c, size_t first) {
    FuncInfo *f = c.func;
    size_t expected = f->params.size() - first;
    size_t given = c.args.size() - first;
    for (size_t i = first; i < c.args.size(); i++) {
      if (!c.args[i]->type)
        check(c.args[i]);
    }
    if (expected != given) {
      error(c.loc, "'" + f->name + "' expects " + std::to_string(expected) + " argument" +
                       (expected == 1 ? "" : "s") + ", but " + std::to_string(given) + " were given");
      return;
    }
    for (size_t i = first; i < c.args.size(); i++)
      if (c.args[i]->type)
        coerce(c.args[i], f->params[i]);
  }

  Type *checkMethodCall(CallExpr &c) {
    auto *fe = static_cast<FieldExpr *>(c.callee.get());
    Type *bt = check(fe->base);
    if (!bt) {
      for (auto &a : c.args)
        check(a);
      return nullptr;
    }
    Type *st = bt->derefAll();
    FuncInfo *m = nullptr;
    if (st->kind == TypeKind::Struct) {
      auto it = st->st->methods.find(fe->name);
      if (it != st->st->methods.end())
        m = it->second;
    }
    if (!m) {
      error(fe->loc, "type '" + bt->str() + "' has no method '" + fe->name + "'");
      for (auto &a : c.args)
        check(a);
      return nullptr;
    }
    c.func = m;
    Type *rt = m->params[0];
    ExprPtr recv = std::move(fe->base);
    // Auto-reference / auto-dereference the receiver, like Rust.
    if (!rt->isRef()) {
      if (bt->isRef())
        recv = wrap(std::move(recv), UnOp::Deref, st);
    } else if (!rt->mut) {
      if (!bt->isRef())
        recv = wrap(std::move(recv), UnOp::Ref, rt);
      else if (bt->mut)
        recv = wrap(std::move(recv), UnOp::ReborrowShared, rt);
    } else {
      if (!bt->isRef()) {
        if (isPlace(recv.get()))
          checkMutablePlace(recv.get(), "borrow as mutable");
        recv = wrap(std::move(recv), UnOp::RefMut, rt);
      } else if (bt->mut) {
        if (isPlace(recv.get()))
          recv = wrap(std::move(recv), UnOp::ReborrowMut, rt);
      } else {
        error(fe->loc, "method '" + m->name + "' needs '&mut " + st->str() + "', but '" +
                           exprStr(recv.get()) + "' is a shared reference '" + bt->str() + "'");
      }
    }
    c.args.insert(c.args.begin(), std::move(recv));
    c.receiverLast = true;
    checkArgs(c, 1);
    return m->ret;
  }

  bool argCount(CallExpr &c, const char *name, size_t n) {
    for (auto &a : c.args)
      check(a);
    if (c.args.size() != n) {
      error(c.loc, std::string(name) + " expects " + std::to_string(n) + " argument" + (n == 1 ? "" : "s"));
      return false;
    }
    for (auto &a : c.args)
      if (!a->type)
        return false;
    return true;
  }

  Type *checkBuiltin(CallExpr &c) {
    switch (c.builtin) {
    case Builtin::Print:
    case Builtin::Println:
      for (auto &a : c.args) {
        Type *t = check(a);
        if (!t)
          continue;
        Type *base = t->derefAll();
        if (base->kind == TypeKind::Int || base->kind == TypeKind::Float || base->kind == TypeKind::Bool) {
          continue;
        } else if (base->kind == TypeKind::String) {
          autoRefShared(a);
        } else {
          error(a->loc, "cannot print a value of type '" + t->str() + "'");
        }
      }
      return tc_.voidTy();
    case Builtin::Len: {
      if (!argCount(c, "len", 1))
        return nullptr;
      Type *base = c.args[0]->type->derefAll();
      if (base->kind != TypeKind::String && base->kind != TypeKind::Slice) {
        error(c.args[0]->loc, "len needs a string or slice, found '" + c.args[0]->type->str() + "'");
        return nullptr;
      }
      autoRefShared(c.args[0]);
      return tc_.intTy();
    }
    case Builtin::Append: {
      if (!argCount(c, "append", 2))
        return nullptr;
      Type *st = c.args[0]->type;
      if (st->kind != TypeKind::Slice) {
        if (st->derefAll()->kind == TypeKind::Slice)
          error(c.args[0]->loc, "append takes the slice by value; write 'v = append(v, x)' where v is the "
                                "slice place itself (e.g. 'p.items = append(p.items, x)')");
        else
          error(c.args[0]->loc, "append needs a slice, found '" + st->str() + "'");
        return nullptr;
      }
      coerce(c.args[1], st->inner);
      return st;
    }
    case Builtin::Clone: {
      if (!argCount(c, "clone", 1))
        return nullptr;
      Type *t = c.args[0]->type->derefAll();
      if (t->kind == TypeKind::Void)
        return nullptr;
      autoRefShared(c.args[0]);
      return t;
    }
    case Builtin::ToInt:
    case Builtin::ToFloat: {
      const char *n = c.builtin == Builtin::ToInt ? "int" : "float";
      if (!argCount(c, n, 1))
        return nullptr;
      if (!c.args[0]->type->isNumeric()) {
        error(c.args[0]->loc, std::string("cannot convert '") + c.args[0]->type->str() + "' to " + n);
        return nullptr;
      }
      return c.builtin == Builtin::ToInt ? tc_.intTy() : tc_.floatTy();
    }
    case Builtin::ToStr: {
      if (!argCount(c, "str", 1))
        return nullptr;
      TypeKind k = c.args[0]->type->kind;
      if (k != TypeKind::Int && k != TypeKind::Float && k != TypeKind::Bool) {
        error(c.args[0]->loc, "str() converts int, float or bool, found '" + c.args[0]->type->str() + "'");
        return nullptr;
      }
      return tc_.stringTy();
    }
    case Builtin::Panic: {
      if (!argCount(c, "panic", 1))
        return nullptr;
      if (c.args[0]->type->derefAll()->kind != TypeKind::String) {
        error(c.args[0]->loc, "panic needs a string message");
        return nullptr;
      }
      autoRefShared(c.args[0]);
      return tc_.voidTy();
    }
    case Builtin::None:
      break;
    }
    return nullptr;
  }
};

} // namespace

void analyze(Program &prog, TypeContext &tc, Diagnostics &diag) {
  Sema s(prog, tc, diag);
  s.run();
}

} // namespace co
