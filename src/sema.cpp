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
    {"error", Builtin::MakeError},
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

// Strings and errors (a message) can be joined with `+`.
bool textual(Type *t) {
  TypeKind k = t->derefAll()->kind;
  return k == TypeKind::String || k == TypeKind::Error;
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
  std::unordered_map<std::string, EnumInfo *> enums_;
  // Variant names usable without the enum prefix; nullptr if ambiguous.
  std::unordered_map<std::string, std::pair<EnumInfo *, int>> variants_;
  std::unordered_map<std::string, FuncInfo *> funcs_;

  FuncDecl *fn_ = nullptr;
  std::vector<std::unordered_map<std::string, LocalVar *>> scopes_;
  int loopDepth_ = 0;
  int breakDepth_ = 0; // loops and switches

  void error(SourceLoc l, std::string msg) { diag_.error(l, std::move(msg)); }

  // ----- types -----

  Type *resolveType(const TypeExpr &t) {
    switch (t.kind) {
    case TypeExpr::Name: {
      if (t.name == "int") return tc_.intTy();
      if (t.name == "float") return tc_.floatTy();
      if (t.name == "bool") return tc_.boolTy();
      if (t.name == "string") return tc_.stringTy();
      if (t.name == "error") return tc_.errorTy();
      auto it = structs_.find(t.name);
      if (it != structs_.end())
        return tc_.structTy(it->second);
      auto en = enums_.find(t.name);
      if (en != enums_.end())
        return tc_.enumTy(en->second);
      error(t.loc, "unknown type '" + t.name + "'");
      return nullptr;
    }
    case TypeExpr::Optional: {
      Type *inner = resolveType(*t.inner);
      if (!inner)
        return nullptr;
      return tc_.optional(inner);
    }
    case TypeExpr::Result: {
      Type *inner = t.inner ? resolveType(*t.inner) : tc_.voidTy();
      if (!inner)
        return nullptr;
      return tc_.result(inner);
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
      if (inner->containsRef()) {
        error(t.loc, "slices cannot hold references (borrowed data can't be stored in containers)");
        return nullptr;
      }
      return tc_.slice(inner);
    }
    }
    return nullptr;
  }

  bool typeNameTaken(const std::string &n) {
    return structs_.count(n) || enums_.count(n) || n == "int" || n == "float" || n == "bool" ||
           n == "string" || n == "error";
  }

  void declareStructs() {
    for (auto &ed : prog_.enums) {
      if (typeNameTaken(ed->name)) {
        error(ed->loc, "type '" + ed->name + "' is already defined");
        continue;
      }
      auto info = std::make_unique<EnumInfo>();
      info->name = ed->name;
      info->loc = ed->loc;
      ed->info = info.get();
      enums_[ed->name] = info.get();
      prog_.enumInfos.push_back(std::move(info));
    }
    for (auto &sd : prog_.structs) {
      if (typeNameTaken(sd->name)) {
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
        if (ty->containsRef()) {
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
    for (auto &ed : prog_.enums) {
      if (!ed->info)
        continue;
      EnumInfo *en = ed->info;
      for (auto &vd : ed->variants) {
        if (en->variantIndex.count(vd.name)) {
          error(vd.loc, "duplicate variant '" + vd.name + "'");
          continue;
        }
        Variant v;
        v.name = vd.name;
        v.loc = vd.loc;
        for (auto &fte : vd.fields) {
          Type *ft = resolveType(*fte);
          if (ft && ft->containsRef()) {
            error(fte->loc, "enum variants cannot hold references; store an owned value instead");
            ft = nullptr;
          }
          v.fields.push_back(ft ? ft : tc_.intTy());
        }
        en->variantIndex[v.name] = (int)en->variants.size();
        auto it = variants_.find(v.name);
        if (it == variants_.end())
          variants_[v.name] = {en, (int)en->variants.size()};
        else
          it->second.first = nullptr; // ambiguous: needs `Enum.Variant`
        en->variants.push_back(std::move(v));
      }
      if (en->variants.empty())
        error(ed->loc, "enum '" + en->name + "' needs at least one variant");
    }

    // Reject infinitely sized types and compute which ones need drop glue.
    std::unordered_map<const void *, int> state; // 0 = new, 1 = visiting, 2 = done
    std::function<bool(Type *)> visitType;
    auto enter = [&](const void *key, const std::string &name, SourceLoc loc) -> int {
      if (state[key] == 2)
        return 2;
      if (state[key] == 1) {
        error(loc, "recursive type '" + name + "' has infinite size; use a slice ([]" + name +
                       ") for indirection");
        return 1;
      }
      state[key] = 1;
      return 0;
    };
    std::function<bool(StructInfo *)> visitStruct = [&](StructInfo *st) -> bool {
      int s = enter(st, st->name, st->loc);
      if (s)
        return s == 2;
      bool ok = true;
      for (auto &f : st->fields)
        ok = visitType(f.type) && ok;
      st->needsDrop = false;
      for (auto &f : st->fields)
        st->needsDrop |= f.type->needsDrop();
      state[st] = 2;
      return ok;
    };
    std::function<bool(EnumInfo *)> visitEnum = [&](EnumInfo *en) -> bool {
      int s = enter(en, en->name, en->loc);
      if (s)
        return s == 2;
      bool ok = true;
      for (auto &v : en->variants)
        for (Type *ft : v.fields)
          ok = visitType(ft) && ok;
      en->needsDrop = false;
      for (auto &v : en->variants)
        for (Type *ft : v.fields)
          en->needsDrop |= ft->needsDrop();
      en->isCopy = !en->needsDrop;
      state[en] = 2;
      return ok;
    };
    visitType = [&](Type *t) -> bool {
      switch (t->kind) {
      case TypeKind::Struct: return visitStruct(t->st);
      case TypeKind::Enum: return t->en->valueType() ? visitType(t->en->valueType()) : visitEnum(t->en);
      default: return true; // slices and references add indirection
      }
    };
    for (auto &info : prog_.structInfos)
      if (!visitStruct(info.get())) {
        info->fields.clear(); // prevent further cascades
        info->fieldIndex.clear();
      }
    for (auto &info : prog_.enumInfos)
      if (!visitEnum(info.get()))
        for (auto &v : info->variants)
          v.fields.clear();
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
          refParams += rt->containsRef();
        }
      }
      for (auto &p : fd->params) {
        Type *pt = resolveType(*p.type);
        if (!pt)
          ok = false;
        else
          refParams += pt->containsRef();
        info->params.push_back(pt);
        info->paramNames.push_back(p.name);
      }
      info->ret = tc_.voidTy();
      if (fd->ret) {
        info->ret = resolveType(*fd->ret);
        if (!info->ret) {
          ok = false;
        } else if (info->ret->containsRef() && refParams == 0) {
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
          // `func main() !` is allowed: an error returned from main is printed.
          bool fails = info->ret == tc_.result(tc_.voidTy());
          info->symbol = fails ? "co.main" : "co_main";
          if (!info->params.empty() || (info->ret->kind != TypeKind::Void && !fails))
            error(fd->loc, "func main must take no parameters and return nothing (or '!' to allow errors)");
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
        if (ty && ty->kind == TypeKind::None) {
          error(vd.init->loc, "cannot tell the type of 'none' here; write 'var " + vd.name + " ?T = none'");
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
      if (es.expr->kind == ExprKind::Unary && static_cast<UnaryExpr *>(es.expr.get())->op == UnOp::Try) {
        check(es.expr);
        break;
      }
      if (es.expr->kind != ExprKind::Call) {
        check(es.expr);
        error(es.expr->loc, "expression is not used (only calls can be statements)");
        break;
      }
      check(es.expr);
      if (es.expr->kind != ExprKind::Call)
        break;
      auto *call = static_cast<CallExpr *>(es.expr.get());
      if (call->builtin == Builtin::Append)
        error(call->loc, "result of append must be assigned: write 'v = append(v, x)'");
      if (call->type && call->type->isResult() && call->func)
        error(call->loc, "the error from '" + call->func->name + "' is ignored; write 'try " + call->func->name +
                             "(...)' to pass it on, or handle it with 'or' or a switch");
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
      breakDepth_++;
      checkBlock(*fs.body);
      breakDepth_--;
      loopDepth_--;
      scopes_.pop_back();
      break;
    }
    case StmtKind::ForRange: {
      auto &fr = static_cast<ForRangeStmt &>(s);
      Type *t = check(fr.range);
      Type *elemTy = nullptr;
      if (t && t->derefAll()->kind == TypeKind::Slice) {
        fr.overSlice = true;
        Type *et = t->derefAll()->inner;
        fr.valueByRef = !et->isCopy();
        elemTy = fr.valueByRef ? tc_.ref(et, false) : et;
        autoRefShared(fr.range); // iterating borrows the slice; it is not consumed
      } else if (t && t->kind != TypeKind::Int) {
        error(fr.range->loc, "range expects an int (iterates 0..n-1) or a slice, found '" + t->str() + "'");
      } else if (t && !fr.valueName.empty()) {
        error(fr.valueLoc, "range over an int gives only one value: write 'for i := range n'");
      }
      scopes_.emplace_back();
      fr.var = newVar(fr.name, tc_.intTy(), fr.nameLoc);
      if (fr.name != "_")
        declare(fr.var);
      if (!fr.valueName.empty() && elemTy) {
        fr.valueVar = newVar(fr.valueName, elemTy, fr.valueLoc);
        if (fr.valueName != "_")
          declare(fr.valueVar);
      }
      loopDepth_++;
      breakDepth_++;
      checkBlock(*fr.body);
      breakDepth_--;
      loopDepth_--;
      scopes_.pop_back();
      break;
    }
    case StmtKind::Switch:
      checkSwitch(static_cast<SwitchStmt &>(s));
      break;
    case StmtKind::Return: {
      auto &rs = static_cast<ReturnStmt &>(s);
      Type *ret = fn_->info->ret;
      bool okIfEmpty = ret->isResult() && ret->en->resultOf->kind == TypeKind::Void;
      if (rs.value) {
        Type *t = check(rs.value);
        if (ret->kind == TypeKind::Void) {
          error(rs.value->loc, "function '" + fn_->name + "' does not return a value");
        } else if (t) {
          coerce(rs.value, ret);
        }
      } else if (ret->kind != TypeKind::Void && !okIfEmpty) {
        error(rs.loc, "missing return value of type '" + ret->str() + "'");
      }
      break;
    }
    case StmtKind::Break:
      if (breakDepth_ == 0)
        error(s.loc, "break outside of a loop or switch");
      break;
    case StmtKind::Continue:
      if (loopDepth_ == 0)
        error(s.loc, "continue outside of a loop");
      break;
    }
  }

  void checkCaseBody(SwitchCase &c) {
    breakDepth_++;
    for (auto &st : c.body)
      checkStmt(*st);
    breakDepth_--;
  }

  // Resolves a case pattern like `Circle(r, _)`, `Empty`, `some(x)` or `none`
  // against enum `en`. Returns the variant index, or -1. `binds` receives the
  // names (and locations) to bind, "_" for ignored fields.
  int resolvePattern(Expr *e, EnumInfo *en, std::vector<std::pair<std::string, SourceLoc>> &binds) {
    std::string name;
    std::vector<ExprPtr> *args = nullptr;
    Expr *callee = e;
    if (e->kind == ExprKind::Call) {
      auto *c = static_cast<CallExpr *>(e);
      callee = c->callee.get();
      args = &c->args;
    }
    if (callee->kind == ExprKind::Ident) {
      name = static_cast<IdentExpr *>(callee)->name;
    } else if (callee->kind == ExprKind::Field) {
      auto *fe = static_cast<FieldExpr *>(callee);
      if (fe->base->kind != ExprKind::Ident || static_cast<IdentExpr *>(fe->base.get())->name != en->name) {
        error(e->loc, "expected a variant of '" + en->name + "' here");
        return -1;
      }
      name = fe->name;
    } else if (callee->kind == ExprKind::NoneLit && en->optionalOf) {
      name = "none";
    } else {
      error(e->loc, "expected a variant of '" + en->name + "' here, like 'case " + en->variants[0].name + "'");
      return -1;
    }
    auto it = en->variantIndex.find(name);
    if (it == en->variantIndex.end()) {
      error(callee->loc, "'" + en->name + "' has no variant '" + name + "'");
      return -1;
    }
    const Variant &v = en->variants[it->second];
    size_t given = args ? args->size() : 0;
    if (given != v.fields.size()) {
      std::string want = v.name;
      if (!v.fields.empty()) {
        want += "(";
        for (size_t i = 0; i < v.fields.size(); i++)
          want += std::string(i ? ", " : "") + "x" + std::to_string(i + 1);
        want += ")";
      }
      error(e->loc, "variant '" + v.name + "' has " + std::to_string(v.fields.size()) +
                        " value(s); write 'case " + want + "'");
      return -1;
    }
    for (size_t i = 0; i < given; i++) {
      Expr *a = (*args)[i].get();
      if (a->kind != ExprKind::Ident) {
        error(a->loc, "expected a name to bind (or '_') in pattern");
        return -1;
      }
      binds.push_back({static_cast<IdentExpr *>(a)->name, a->loc});
    }
    return it->second;
  }

  void checkSwitch(SwitchStmt &sw) {
    int defaults = 0;
    for (auto &c : sw.cases)
      if (c.isDefault && ++defaults == 2)
        error(c.loc, "switch has more than one 'default'");

    if (!sw.tag) {
      sw.mode = SwitchStmt::Conditions;
      for (auto &c : sw.cases) {
        for (auto &v : c.values)
          checkCond(v);
        scopes_.emplace_back();
        checkCaseBody(c);
        scopes_.pop_back();
      }
      return;
    }

    Type *tt = check(sw.tag);
    if (!tt)
      return;
    Type *base = tt->derefAll();
    if (base->kind == TypeKind::Enum) {
      sw.mode = SwitchStmt::Enum;
      EnumInfo *en = base->en;
      std::vector<bool> covered(en->variants.size(), false);
      for (auto &c : sw.cases) {
        scopes_.emplace_back();
        for (auto &v : c.values) {
          std::vector<std::pair<std::string, SourceLoc>> binds;
          int vi = resolvePattern(v.get(), en, binds);
          if (vi < 0)
            continue;
          if (covered[vi])
            error(v->loc, "variant '" + en->variants[vi].name + "' is already handled by an earlier case");
          covered[vi] = true;
          c.variants.push_back(vi);
          if (c.values.size() > 1) {
            for (auto &b : binds)
              if (b.first != "_")
                error(b.second, "a case with several variants cannot bind values; use '_'");
            continue;
          }
          for (size_t i = 0; i < binds.size(); i++) {
            Type *ft = en->variants[vi].fields[i];
            // Switching never consumes the value: owned payloads are borrowed.
            bool byRef = !ft->isCopy();
            c.bindByRef.push_back(byRef);
            if (binds[i].first == "_") {
              c.bindings.push_back(nullptr);
              continue;
            }
            LocalVar *lv = newVar(binds[i].first, byRef ? tc_.ref(ft, false) : ft, binds[i].second);
            declare(lv);
            c.bindings.push_back(lv);
          }
        }
        checkCaseBody(c);
        scopes_.pop_back();
      }
      if (!defaults) {
        std::string missing;
        for (size_t i = 0; i < covered.size(); i++)
          if (!covered[i])
            missing += std::string(missing.empty() ? "" : ", ") + en->variants[i].name;
        if (!missing.empty())
          error(sw.loc, "switch on '" + en->name + "' does not handle: " + missing + " (add the cases or a 'default')");
        else
          sw.exhaustive = true;
      }
      return;
    }

    sw.mode = SwitchStmt::Values;
    bool isStr = base->kind == TypeKind::String;
    if (!isStr && !(tt->kind == TypeKind::Int || tt->kind == TypeKind::Float || tt->kind == TypeKind::Bool)) {
      error(sw.tag->loc, "cannot switch on a value of type '" + tt->str() + "'");
      return;
    }
    if (isStr)
      autoRefShared(sw.tag);
    for (auto &c : sw.cases) {
      for (auto &v : c.values) {
        Type *vt = check(v);
        if (!vt)
          continue;
        if (isStr) {
          if (vt->derefAll()->kind != TypeKind::String)
            error(v->loc, "case value must be a string, found '" + vt->str() + "'");
          else
            autoRefShared(v);
        } else {
          coerce(v, tt);
        }
      }
      scopes_.emplace_back();
      checkCaseBody(c);
      scopes_.pop_back();
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

  ExprPtr makeVariant(SourceLoc l, Type *enumTy, int variant, std::vector<ExprPtr> args) {
    auto lit = std::make_unique<EnumLitExpr>(l, enumTy->en, variant);
    lit->args = std::move(args);
    lit->type = enumTy;
    return lit;
  }

  // Implicit conversions at assignment/argument/return sites. Arguments
  // (`isArg`) are also borrowed automatically when the parameter is `&T`.
  bool coerce(ExprPtr &e, Type *target, bool isArg = false) {
    Type *t = e->type;
    if (!t || !target)
      return false;
    if (target->isResult() && t != target) {
      int vi = 0;
      if (t->kind == TypeKind::Error) {
        vi = 1;
      } else if (target->en->resultOf->kind == TypeKind::Void) {
        error(e->loc, "expected an error here, found '" + t->str() + "' (this function returns only '!')");
        return false;
      } else if (!coerce(e, target->en->resultOf, isArg)) {
        return false;
      }
      SourceLoc l = e->loc;
      std::vector<ExprPtr> args;
      args.push_back(std::move(e));
      e = makeVariant(l, target, vi, std::move(args));
      return true;
    }
    if (target->isOptional() && t != target) {
      if (t->kind == TypeKind::None) {
        e = makeVariant(e->loc, target, 0, {});
        return true;
      }
      if (!coerce(e, target->en->optionalOf, isArg))
        return false;
      SourceLoc l = e->loc;
      std::vector<ExprPtr> args;
      args.push_back(std::move(e));
      e = makeVariant(l, target, 1, std::move(args));
      return true;
    }
    if (isArg && target->isRef() && !t->isRef() && target->inner == t) {
      if (!target->mut) {
        e = wrap(std::move(e), UnOp::Ref, target);
        return true;
      }
      error(e->loc, "this function may change its argument, so pass it as '&mut " + exprStr(e.get()) +
                        "' to allow that");
      return false;
    }
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
    if (t->kind == TypeKind::None)
      hint = " ('none' can only be used where an optional '?T' is expected)";
    else if (t->isOptional() && t->en->optionalOf == target)
      hint = " (the value may be missing: use 'x or default', or a switch with 'case some(v)')";
    else if (t->isResult() && t->en->resultOf == target)
      hint = " (this may be an error: use 'try x' to pass it on, 'x or default', or a switch with 'case ok(v)')";
    else if (target->isRef() && !t->isRef() && target->inner == t)
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
    case ExprKind::NoneLit: return tc_.noneTy();
    case ExprKind::EnumLit: return e->type;
    case ExprKind::Ident: {
      auto *id = static_cast<IdentExpr *>(e.get());
      if (id->var) // already resolved (e.g. append rewrite looked at it)
        return id->var->type;
      id->var = lookup(id->name);
      if (!id->var) {
        Type *vt = nullptr;
        if (enumConstructor(e, nullptr, id->name, id->loc, nullptr, vt))
          return vt;
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
      return checkCall(*static_cast<CallExpr *>(e.get()), e);
    case ExprKind::Field: {
      auto *fe = static_cast<FieldExpr *>(e.get());
      Type *vt = nullptr;
      if (EnumInfo *en = enumPrefix(fe->base.get()))
        if (enumConstructor(e, en, fe->name, fe->loc, nullptr, vt))
          return vt;
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
    case UnOp::Try: {
      Type *ret = fn_->info->ret;
      std::string fname = fn_->name == "main" ? "main" : fn_->name;
      if (t->isResult()) {
        if (!ret->isResult()) {
          std::string want = ret->kind == TypeKind::Void ? "!" : "!" + ret->str();
          std::string sig = fname == "main" ? "func main() !" : "func " + fname + "(...) " + want;
          error(u.loc, "'try' passes errors to the caller, but '" + fname + "' can't return errors; declare it as '" +
                           sig + "'");
          return nullptr;
        }
        return t->en->resultOf;
      }
      if (t->isOptional()) {
        if (!ret->isOptional()) {
          error(u.loc, "'try' on an optional returns 'none' from '" + fname +
                           "', so it must return an optional '?T'; use 'x or default' instead");
          return nullptr;
        }
        return t->en->optionalOf;
      }
      error(u.loc, "'try' needs a value that may fail (!T) or be missing (?T), found '" + t->str() + "'");
      return nullptr;
    }
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
    if (b.op == BinOp::OrElse) {
      Type *opt = lt->derefAll();
      if (!opt->isOptional() && !opt->isResult()) {
        error(b.loc, "'or' needs a value that may be missing (?T) or fail (!T) on its left, found '" +
                         lt->str() + "'");
        return nullptr;
      }
      Type *inner = opt->en->valueType();
      if (inner->kind == TypeKind::Void) {
        error(b.loc, "'or' needs a value, but '" + lt->str() + "' has none; use 'try' or a switch");
        return nullptr;
      }
      // Like switch, `or` on a variable (or through a reference) only looks:
      // owned payloads come out borrowed.
      if (!inner->isCopy() && (lt->isRef() || isPlace(b.lhs.get()))) {
        b.orBorrows = true;
        Type *res = inner->isRef() ? inner : tc_.ref(inner, false);
        if (rt == inner && !inner->isRef())
          b.rhs = wrap(std::move(b.rhs), UnOp::Ref, res);
        else if (!coerce(b.rhs, res))
          return nullptr;
        return res;
      }
      if (rt == lt)
        return lt;
      if (!coerce(b.rhs, inner))
        return nullptr;
      return inner;
    }
    if ((b.op == BinOp::Eq || b.op == BinOp::Ne) &&
        (lt->kind == TypeKind::None || rt->kind == TypeKind::None)) {
      Type *other = lt->kind == TypeKind::None ? rt : lt;
      if (!other->derefAll()->isOptional()) {
        error(b.loc, "only optional values (?T) can be compared with 'none', not '" + other->str() + "'");
        return nullptr;
      }
      b.noneCheck = true;
      return tc_.boolTy();
    }
    if ((b.op == BinOp::Eq || b.op == BinOp::Ne) && lt->derefAll()->kind == TypeKind::Enum &&
        lt->derefAll() == rt->derefAll()) {
      EnumInfo *en = lt->derefAll()->en;
      for (auto &v : en->variants)
        if (!v.fields.empty()) {
          error(b.loc, "'" + en->name + "' values can't be compared with '=='; use a switch to look inside them");
          return nullptr;
        }
      b.noneCheck = true; // compares variants only
      return tc_.boolTy();
    }
    switch (b.op) {
    case BinOp::OrElse:
      return nullptr;
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
      if (b.op == BinOp::Add && textual(lt) && textual(rt) &&
          (lt->derefAll()->kind == TypeKind::String || rt->derefAll()->kind == TypeKind::String)) {
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

  // `Shape.Circle`: is `e` the name of an enum type (and not a variable)?
  EnumInfo *enumPrefix(Expr *e) {
    if (e->kind != ExprKind::Ident)
      return nullptr;
    auto *id = static_cast<IdentExpr *>(e);
    if (lookup(id->name))
      return nullptr;
    auto it = enums_.find(id->name);
    return it == enums_.end() ? nullptr : it->second;
  }

  // Tries to build an enum value from `name` (a variant of `en`, or an
  // unqualified variant when en is null). `args` is null when used without
  // parentheses. Replaces `e` and returns true if `name` is a variant.
  bool enumConstructor(ExprPtr &e, EnumInfo *en, const std::string &name, SourceLoc loc,
                       std::vector<ExprPtr> *args, Type *&out) {
    out = nullptr;
    int vi;
    if (en) {
      auto it = en->variantIndex.find(name);
      if (it == en->variantIndex.end()) {
        error(loc, "enum '" + en->name + "' has no variant '" + name + "'");
        return true;
      }
      vi = it->second;
    } else {
      auto it = variants_.find(name);
      if (it == variants_.end())
        return false;
      if (!it->second.first) {
        error(loc, "'" + name + "' is a variant of several enums; write 'EnumName." + name + "'");
        return true;
      }
      en = it->second.first;
      vi = it->second.second;
    }
    const Variant &v = en->variants[vi];
    size_t given = args ? args->size() : 0;
    if (given != v.fields.size()) {
      if (args)
        for (auto &a : *args)
          check(a);
      error(loc, "variant '" + v.name + "' takes " + std::to_string(v.fields.size()) + " value(s), but " +
                     std::to_string(given) + " were given");
      return true;
    }
    Type *et = tc_.enumTy(en);
    std::vector<ExprPtr> moved;
    bool ok = true;
    for (size_t i = 0; i < given; i++) {
      ExprPtr &a = (*args)[i];
      if (!check(a) || !coerce(a, v.fields[i]))
        ok = false;
      moved.push_back(std::move(a));
    }
    e = makeVariant(e->loc, et, vi, std::move(moved));
    out = ok ? et : nullptr;
    return true;
  }

  Type *checkCall(CallExpr &c, ExprPtr &self) {
    Type *vt = nullptr;
    if (c.callee->kind == ExprKind::Field) {
      auto *fe = static_cast<FieldExpr *>(c.callee.get());
      if (EnumInfo *en = enumPrefix(fe->base.get())) {
        std::string name = fe->name;
        enumConstructor(self, en, name, fe->loc, &c.args, vt);
        return vt;
      }
      return checkMethodCall(c);
    }
    if (c.callee->kind == ExprKind::Ident) {
      auto *id = static_cast<IdentExpr *>(c.callee.get());
      if (!lookup(id->name) && !kBuiltins.count(id->name) && !funcs_.count(id->name)) {
        if (id->name == "some") {
          if (c.args.size() != 1) {
            error(c.loc, "some(x) takes one value");
            return nullptr;
          }
          Type *at = check(c.args[0]);
          if (!at)
            return nullptr;
          if (at->kind == TypeKind::None || at->kind == TypeKind::Void) {
            error(c.args[0]->loc, "some(...) needs a value");
            return nullptr;
          }
          Type *opt = tc_.optional(at);
          std::vector<ExprPtr> args;
          args.push_back(std::move(c.args[0]));
          self = makeVariant(c.loc, opt, 1, std::move(args));
          return opt;
        }
        std::string name = id->name;
        if (enumConstructor(self, nullptr, name, id->loc, &c.args, vt))
          return vt;
      }
    }
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
        coerce(c.args[i], f->params[i], true);
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
        if (base->kind == TypeKind::Void || base->kind == TypeKind::None) {
          error(a->loc, "cannot print a value of type '" + t->str() + "'");
        } else if (base->kind == TypeKind::Int || base->kind == TypeKind::Float || base->kind == TypeKind::Bool) {
          continue;
        } else {
          autoRefShared(a); // printing never consumes a value
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
      TypeKind k = c.args[0]->type->derefAll()->kind;
      if (k == TypeKind::Error) {
        autoRefShared(c.args[0]);
        return tc_.stringTy(); // the error's message
      }
      if (c.args[0]->type->isRef()) {
        error(c.args[0]->loc, "str() needs a value; use '*x'");
        return nullptr;
      }
      if (k != TypeKind::Int && k != TypeKind::Float && k != TypeKind::Bool) {
        error(c.args[0]->loc, "str() converts int, float, bool or error, found '" + c.args[0]->type->str() + "'");
        return nullptr;
      }
      return tc_.stringTy();
    }
    case Builtin::MakeError: {
      if (!argCount(c, "error", 1))
        return nullptr;
      if (c.args[0]->type->derefAll()->kind != TypeKind::String) {
        error(c.args[0]->loc, "error(...) needs a message string; use str(x) to convert");
        return nullptr;
      }
      autoRefShared(c.args[0]);
      return tc_.errorTy();
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
