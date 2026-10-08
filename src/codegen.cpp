#include "codegen.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <functional>
#include <unordered_map>

namespace co {
using namespace mir;

namespace {

class CodeGen {
public:
  CodeGen(llvm::LLVMContext &ctx, llvm::Module &mod)
      : ctx_(ctx), mod_(mod), b_(ctx), dl_(mod.getDataLayout()) {
    ptrTy_ = llvm::PointerType::getUnqual(ctx_);
    i64_ = llvm::Type::getInt64Ty(ctx_);
    i32_ = llvm::Type::getInt32Ty(ctx_);
    i1_ = llvm::Type::getInt1Ty(ctx_);
    f64_ = llvm::Type::getDoubleTy(ctx_);
    void_ = llvm::Type::getVoidTy(ctx_);
    vecTy_ = llvm::StructType::create(ctx_, {ptrTy_, i64_, i64_}, "co.vec");
  }

  void run(const Module &m) {
    for (auto &f : m.funcs)
      declare(f.info);
    for (auto &f : m.funcs)
      emitFunction(f);
  }

private:
  llvm::LLVMContext &ctx_;
  llvm::Module &mod_;
  llvm::IRBuilder<> b_;
  const llvm::DataLayout &dl_;
  llvm::PointerType *ptrTy_;
  llvm::Type *i64_, *i32_, *i1_, *f64_, *void_;
  llvm::StructType *vecTy_;
  std::unordered_map<StructInfo *, llvm::StructType *> structTys_;
  std::unordered_map<FuncInfo *, llvm::Function *> fns_;
  std::unordered_map<Type *, llvm::Function *> dropFns_, cloneFns_;

  // per-function state
  const Function *f_ = nullptr;
  llvm::Function *fn_ = nullptr;
  std::vector<llvm::Value *> slots_;
  std::vector<llvm::Value *> flags_; // drop flags
  std::vector<llvm::BasicBlock *> bbs_;
  llvm::BasicBlock *entry_ = nullptr;

  // ----- types -----

  llvm::Type *lt(Type *t) {
    switch (t->kind) {
    case TypeKind::Void: return void_;
    case TypeKind::Int: return i64_;
    case TypeKind::Float: return f64_;
    case TypeKind::Bool: return i1_;
    case TypeKind::String:
    case TypeKind::Slice: return vecTy_;
    case TypeKind::Ref: return ptrTy_;
    case TypeKind::Struct: {
      auto it = structTys_.find(t->st);
      if (it != structTys_.end())
        return it->second;
      auto *st = llvm::StructType::create(ctx_, "co." + t->st->name);
      structTys_[t->st] = st;
      std::vector<llvm::Type *> fields;
      for (auto &f : t->st->fields)
        fields.push_back(lt(f.type));
      st->setBody(fields);
      return st;
    }
    }
    return void_;
  }

  llvm::Value *sizeOf(Type *t) { return llvm::ConstantInt::get(i64_, dl_.getTypeAllocSize(lt(t))); }

  // ----- runtime -----

  llvm::FunctionCallee rt(const char *name, llvm::Type *ret, std::vector<llvm::Type *> params) {
    auto callee = mod_.getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
    if (auto *fn = llvm::dyn_cast<llvm::Function>(callee.getCallee())) {
      if (std::string(name).rfind("co_panic", 0) == 0)
        fn->setDoesNotReturn();
    }
    return callee;
  }

  void declare(FuncInfo *info) {
    std::vector<llvm::Type *> params;
    for (Type *p : info->params)
      params.push_back(lt(p));
    auto *fty = llvm::FunctionType::get(lt(info->ret), params, false);
    auto linkage = info->symbol == "co_main" ? llvm::Function::ExternalLinkage : llvm::Function::InternalLinkage;
    fns_[info] = llvm::Function::Create(fty, linkage, info->symbol, mod_);
  }

  llvm::AllocaInst *entryAlloca(llvm::Type *ty, const llvm::Twine &name = "") {
    llvm::IRBuilder<> eb(entry_, entry_->getFirstInsertionPt());
    return eb.CreateAlloca(ty, nullptr, name);
  }

  // ----- glue: loops, drop, clone -----

  // Emits `for i in 0..n { body(i) }` at builder `ib`.
  static void emitLoop(llvm::IRBuilder<> &ib, llvm::Value *n, const std::function<void(llvm::Value *)> &body) {
    llvm::Function *fn = ib.GetInsertBlock()->getParent();
    auto &ctx = fn->getContext();
    auto *i64 = llvm::Type::getInt64Ty(ctx);
    llvm::BasicBlock *pre = ib.GetInsertBlock();
    auto *head = llvm::BasicBlock::Create(ctx, "loop", fn);
    auto *bodyBB = llvm::BasicBlock::Create(ctx, "loop.body", fn);
    auto *done = llvm::BasicBlock::Create(ctx, "loop.done", fn);
    ib.CreateBr(head);
    ib.SetInsertPoint(head);
    auto *i = ib.CreatePHI(i64, 2, "i");
    i->addIncoming(llvm::ConstantInt::get(i64, 0), pre);
    ib.CreateCondBr(ib.CreateICmpSLT(i, n), bodyBB, done);
    ib.SetInsertPoint(bodyBB);
    body(i);
    auto *next = ib.CreateAdd(i, llvm::ConstantInt::get(i64, 1));
    i->addIncoming(next, ib.GetInsertBlock());
    ib.CreateBr(head);
    ib.SetInsertPoint(done);
  }

  llvm::Function *dropFn(Type *t) {
    auto it = dropFns_.find(t);
    if (it != dropFns_.end())
      return it->second;
    auto *fty = llvm::FunctionType::get(void_, {ptrTy_}, false);
    auto *fn = llvm::Function::Create(fty, llvm::Function::InternalLinkage, "co.drop." + t->str(), mod_);
    dropFns_[t] = fn;
    llvm::IRBuilder<> ib(llvm::BasicBlock::Create(ctx_, "entry", fn));
    llvm::Value *p = fn->getArg(0);
    switch (t->kind) {
    case TypeKind::String:
    case TypeKind::Slice: {
      llvm::Value *data = ib.CreateLoad(ptrTy_, ib.CreateStructGEP(vecTy_, p, 0));
      if (t->kind == TypeKind::Slice && t->inner->needsDrop()) {
        llvm::Value *len = ib.CreateLoad(i64_, ib.CreateStructGEP(vecTy_, p, 1));
        llvm::Function *elemDrop = dropFn(t->inner);
        llvm::Type *et = lt(t->inner);
        emitLoop(ib, len, [&](llvm::Value *i) { ib.CreateCall(elemDrop, {ib.CreateGEP(et, data, i)}); });
      }
      ib.CreateCall(rt("co_free", void_, {ptrTy_}), {data});
      break;
    }
    case TypeKind::Struct: {
      auto *st = lt(t);
      for (size_t i = 0; i < t->st->fields.size(); i++) {
        Type *ft = t->st->fields[i].type;
        if (ft->needsDrop())
          ib.CreateCall(dropFn(ft), {ib.CreateStructGEP(st, p, (unsigned)i)});
      }
      break;
    }
    default:
      break;
    }
    ib.CreateRetVoid();
    return fn;
  }

  llvm::Function *cloneFn(Type *t) {
    auto it = cloneFns_.find(t);
    if (it != cloneFns_.end())
      return it->second;
    auto *fty = llvm::FunctionType::get(void_, {ptrTy_, ptrTy_}, false);
    auto *fn = llvm::Function::Create(fty, llvm::Function::InternalLinkage, "co.clone." + t->str(), mod_);
    cloneFns_[t] = fn;
    llvm::IRBuilder<> ib(llvm::BasicBlock::Create(ctx_, "entry", fn));
    llvm::Value *dst = fn->getArg(0), *src = fn->getArg(1);
    if (t->isCopy()) {
      ib.CreateStore(ib.CreateLoad(lt(t), src), dst);
    } else if (t->kind == TypeKind::String) {
      ib.CreateCall(rt("co_str_clone", void_, {ptrTy_, ptrTy_}), {dst, src});
    } else if (t->kind == TypeKind::Slice) {
      ib.CreateCall(rt("co_vec_clone_bits", void_, {ptrTy_, ptrTy_, i64_}), {dst, src, sizeOf(t->inner)});
      if (!t->inner->isCopy()) {
        llvm::Value *len = ib.CreateLoad(i64_, ib.CreateStructGEP(vecTy_, src, 1));
        llvm::Value *sd = ib.CreateLoad(ptrTy_, ib.CreateStructGEP(vecTy_, src, 0));
        llvm::Value *dd = ib.CreateLoad(ptrTy_, ib.CreateStructGEP(vecTy_, dst, 0));
        llvm::Function *ec = cloneFn(t->inner);
        llvm::Type *et = lt(t->inner);
        emitLoop(ib, len, [&](llvm::Value *i) {
          ib.CreateCall(ec, {ib.CreateGEP(et, dd, i), ib.CreateGEP(et, sd, i)});
        });
      }
    } else if (t->kind == TypeKind::Struct) {
      auto *st = lt(t);
      for (size_t i = 0; i < t->st->fields.size(); i++) {
        Type *ft = t->st->fields[i].type;
        llvm::Value *d = ib.CreateStructGEP(st, dst, (unsigned)i);
        llvm::Value *s = ib.CreateStructGEP(st, src, (unsigned)i);
        if (ft->isCopy())
          ib.CreateStore(ib.CreateLoad(lt(ft), s), d);
        else
          ib.CreateCall(cloneFn(ft), {d, s});
      }
    }
    ib.CreateRetVoid();
    return fn;
  }

  // ----- functions -----

  void emitFunction(const Function &f) {
    f_ = &f;
    fn_ = fns_.at(f.info);
    entry_ = llvm::BasicBlock::Create(ctx_, "entry", fn_);
    b_.SetInsertPoint(entry_);
    slots_.assign(f.locals.size(), nullptr);
    flags_.assign(f.locals.size(), nullptr);
    for (size_t i = 0; i < f.locals.size(); i++) {
      Type *t = f.locals[i].type;
      if (t->kind == TypeKind::Void)
        continue;
      const std::string &n = f.locals[i].name;
      slots_[i] = b_.CreateAlloca(lt(t), nullptr, n.empty() ? "_" + std::to_string(i) : n);
      if (t->needsDrop()) {
        flags_[i] = b_.CreateAlloca(i1_, nullptr, "dropflag");
        b_.CreateStore(b_.getFalse(), flags_[i]);
      }
    }
    for (int p = 1; p <= f.numParams; p++) {
      b_.CreateStore(fn_->getArg(p - 1), slots_[p]);
      if (flags_[p])
        b_.CreateStore(b_.getTrue(), flags_[p]);
    }
    bbs_.clear();
    for (size_t i = 0; i < f.blocks.size(); i++)
      bbs_.push_back(llvm::BasicBlock::Create(ctx_, "bb" + std::to_string(i), fn_));
    b_.CreateBr(bbs_[0]);

    for (size_t i = 0; i < f.blocks.size(); i++) {
      b_.SetInsertPoint(bbs_[i]);
      for (auto &s : f.blocks[i].stmts)
        emitStmt(s);
      emitTerm(f.blocks[i].term);
    }
  }

  void panicIf(llvm::Value *cond, const char *what, llvm::Value *a = nullptr, llvm::Value *c = nullptr) {
    auto *bad = llvm::BasicBlock::Create(ctx_, "panic", fn_);
    auto *ok = llvm::BasicBlock::Create(ctx_, "ok", fn_);
    b_.CreateCondBr(cond, bad, ok, llvm::MDBuilder(ctx_).createUnlikelyBranchWeights());
    b_.SetInsertPoint(bad);
    if (a)
      b_.CreateCall(rt("co_panic_bounds", void_, {i64_, i64_}), {a, c});
    else
      b_.CreateCall(rt("co_panic", void_, {ptrTy_}), {b_.CreateGlobalString(what)});
    b_.CreateUnreachable();
    b_.SetInsertPoint(ok);
  }

  llvm::Value *addr(const Place &p, Type *&ty) {
    llvm::Value *v = slots_[p.local];
    ty = f_->locals[p.local].type;
    for (auto &pr : p.proj) {
      switch (pr.kind) {
      case Proj::Deref:
        v = b_.CreateLoad(ptrTy_, v);
        ty = ty->inner;
        break;
      case Proj::Field:
        v = b_.CreateStructGEP(lt(ty), v, (unsigned)pr.field);
        ty = ty->st->fields[pr.field].type;
        break;
      case Proj::Index: {
        llvm::Value *data = b_.CreateLoad(ptrTy_, b_.CreateStructGEP(vecTy_, v, 0));
        llvm::Value *len = b_.CreateLoad(i64_, b_.CreateStructGEP(vecTy_, v, 1));
        llvm::Value *idx = b_.CreateLoad(i64_, slots_[pr.indexLocal]);
        panicIf(b_.CreateICmpUGE(idx, len), nullptr, idx, len);
        ty = ty->inner;
        v = b_.CreateGEP(lt(ty), data, idx);
        break;
      }
      }
    }
    return v;
  }

  llvm::Value *operand(const Operand &o) {
    switch (o.kind) {
    case Operand::Const:
      switch (o.c.kind) {
      case Constant::Int: return llvm::ConstantInt::get(i64_, o.c.i, true);
      case Constant::Float: return llvm::ConstantFP::get(f64_, o.c.f);
      case Constant::Bool: return llvm::ConstantInt::get(i1_, o.c.b);
      case Constant::Zero: return llvm::Constant::getNullValue(lt(o.type));
      }
      return nullptr;
    case Operand::Copy:
    case Operand::Move: {
      Type *ty;
      llvm::Value *a = addr(o.place, ty);
      llvm::Value *v = b_.CreateLoad(lt(ty), a);
      if (o.kind == Operand::Move && o.place.isLocal() && flags_[o.place.local])
        b_.CreateStore(b_.getFalse(), flags_[o.place.local]);
      return v;
    }
    }
    return nullptr;
  }

  llvm::Value *binary(BinOp op, bool isFloat, llvm::Value *l, llvm::Value *r) {
    if (isFloat) {
      switch (op) {
      case BinOp::Add: return b_.CreateFAdd(l, r);
      case BinOp::Sub: return b_.CreateFSub(l, r);
      case BinOp::Mul: return b_.CreateFMul(l, r);
      case BinOp::Div: return b_.CreateFDiv(l, r);
      case BinOp::Eq: return b_.CreateFCmpOEQ(l, r);
      case BinOp::Ne: return b_.CreateFCmpUNE(l, r);
      case BinOp::Lt: return b_.CreateFCmpOLT(l, r);
      case BinOp::Le: return b_.CreateFCmpOLE(l, r);
      case BinOp::Gt: return b_.CreateFCmpOGT(l, r);
      case BinOp::Ge: return b_.CreateFCmpOGE(l, r);
      default: return nullptr;
      }
    }
    switch (op) {
    case BinOp::Add: return b_.CreateAdd(l, r);
    case BinOp::Sub: return b_.CreateSub(l, r);
    case BinOp::Mul: return b_.CreateMul(l, r);
    case BinOp::Div:
    case BinOp::Rem: {
      panicIf(b_.CreateICmpEQ(r, llvm::ConstantInt::get(i64_, 0)), "integer division by zero");
      auto *minInt = llvm::ConstantInt::get(i64_, INT64_MIN, true);
      auto *minus1 = llvm::ConstantInt::get(i64_, -1, true);
      panicIf(b_.CreateAnd(b_.CreateICmpEQ(l, minInt), b_.CreateICmpEQ(r, minus1)), "integer overflow in division");
      return op == BinOp::Div ? b_.CreateSDiv(l, r) : b_.CreateSRem(l, r);
    }
    case BinOp::Eq: return b_.CreateICmpEQ(l, r);
    case BinOp::Ne: return b_.CreateICmpNE(l, r);
    case BinOp::Lt: return b_.CreateICmpSLT(l, r);
    case BinOp::Le: return b_.CreateICmpSLE(l, r);
    case BinOp::Gt: return b_.CreateICmpSGT(l, r);
    case BinOp::Ge: return b_.CreateICmpSGE(l, r);
    default: return nullptr;
    }
  }

  // Calls a runtime function that writes a co.vec result through an out pointer.
  llvm::Value *viaOut(const char *name, std::vector<llvm::Type *> types, std::vector<llvm::Value *> args) {
    llvm::Value *out = entryAlloca(vecTy_);
    types.insert(types.begin(), ptrTy_);
    args.insert(args.begin(), out);
    b_.CreateCall(rt(name, void_, types), args);
    return b_.CreateLoad(vecTy_, out);
  }

  void printValue(Type *t, llvm::Value *v) {
    if (t->isRef()) {
      if (t->inner->kind == TypeKind::String) {
        b_.CreateCall(rt("co_print_str", void_, {ptrTy_}), {v});
        return;
      }
      t = t->inner;
      v = b_.CreateLoad(lt(t), v);
    }
    switch (t->kind) {
    case TypeKind::Int: b_.CreateCall(rt("co_print_int", void_, {i64_}), {v}); break;
    case TypeKind::Float: b_.CreateCall(rt("co_print_float", void_, {f64_}), {v}); break;
    case TypeKind::Bool: b_.CreateCall(rt("co_print_bool", void_, {i32_}), {b_.CreateZExt(v, i32_)}); break;
    default: break;
    }
  }

  llvm::Value *rvalue(const Rvalue &rv) {
    switch (rv.kind) {
    case Rvalue::Use:
      return operand(rv.ops[0]);
    case Rvalue::BinaryOp: {
      llvm::Value *l = operand(rv.ops[0]);
      llvm::Value *r = operand(rv.ops[1]);
      return binary(rv.bop, rv.ops[0].type->kind == TypeKind::Float, l, r);
    }
    case Rvalue::UnaryOp: {
      llvm::Value *v = operand(rv.ops[0]);
      if (rv.uop == UnOp::Not)
        return b_.CreateNot(v);
      return rv.type->kind == TypeKind::Float ? b_.CreateFNeg(v) : b_.CreateNeg(v);
    }
    case Rvalue::Ref: {
      Type *ty;
      return addr(rv.place, ty);
    }
    case Rvalue::Aggregate: {
      llvm::Value *agg = llvm::UndefValue::get(lt(rv.type));
      for (size_t i = 0; i < rv.ops.size(); i++)
        agg = b_.CreateInsertValue(agg, operand(rv.ops[i]), {(unsigned)i});
      return agg;
    }
    case Rvalue::SliceLit: {
      Type *et = rv.type->inner;
      llvm::Value *vec = entryAlloca(vecTy_);
      llvm::Value *tmp = entryAlloca(lt(et));
      b_.CreateCall(rt("co_vec_new", void_, {ptrTy_, i64_, i64_}),
                    {vec, llvm::ConstantInt::get(i64_, rv.ops.size()), sizeOf(et)});
      for (auto &op : rv.ops) {
        b_.CreateStore(operand(op), tmp);
        b_.CreateCall(rt("co_vec_push", void_, {ptrTy_, ptrTy_, i64_}), {vec, tmp, sizeOf(et)});
      }
      return b_.CreateLoad(vecTy_, vec);
    }
    case Rvalue::Call: {
      std::vector<llvm::Value *> args;
      for (auto &op : rv.ops)
        args.push_back(operand(op));
      llvm::Value *v = b_.CreateCall(fns_.at(rv.func), args);
      return rv.func->ret->kind == TypeKind::Void ? nullptr : v;
    }
    case Rvalue::Builtin:
      return builtin(rv);
    }
    return nullptr;
  }

  llvm::Value *builtin(const Rvalue &rv) {
    std::vector<llvm::Value *> a;
    for (auto &op : rv.ops)
      a.push_back(operand(op));
    switch (rv.builtin) {
    case BuiltinOp::Print:
    case BuiltinOp::Println:
      for (size_t i = 0; i < a.size(); i++) {
        if (i && rv.builtin == BuiltinOp::Println)
          b_.CreateCall(rt("co_print_space", void_, {}), {});
        printValue(rv.ops[i].type, a[i]);
      }
      if (rv.builtin == BuiltinOp::Println)
        b_.CreateCall(rt("co_print_newline", void_, {}), {});
      return nullptr;
    case BuiltinOp::Len:
      return b_.CreateLoad(i64_, b_.CreateStructGEP(vecTy_, a[0], 1));
    case BuiltinOp::Append:
    case BuiltinOp::Push: {
      Type *elemTy = rv.ops[1].type;
      llvm::Value *elem = entryAlloca(lt(elemTy));
      b_.CreateStore(a[1], elem);
      llvm::Value *vec = a[0];
      if (rv.builtin == BuiltinOp::Append) {
        vec = entryAlloca(vecTy_);
        b_.CreateStore(a[0], vec);
      }
      b_.CreateCall(rt("co_vec_push", void_, {ptrTy_, ptrTy_, i64_}), {vec, elem, sizeOf(elemTy)});
      return rv.builtin == BuiltinOp::Append ? b_.CreateLoad(vecTy_, vec) : nullptr;
    }
    case BuiltinOp::Clone: {
      Type *t = rv.type;
      if (t->isCopy())
        return b_.CreateLoad(lt(t), a[0]);
      llvm::Value *out = entryAlloca(lt(t));
      b_.CreateCall(cloneFn(t), {out, a[0]});
      return b_.CreateLoad(lt(t), out);
    }
    case BuiltinOp::ToInt:
      return rv.ops[0].type->kind == TypeKind::Float ? b_.CreateFPToSI(a[0], i64_) : a[0];
    case BuiltinOp::ToFloat:
      return rv.ops[0].type->kind == TypeKind::Int ? b_.CreateSIToFP(a[0], f64_) : a[0];
    case BuiltinOp::ToStr:
      switch (rv.ops[0].type->kind) {
      case TypeKind::Int: return viaOut("co_str_from_int", {i64_}, {a[0]});
      case TypeKind::Float: return viaOut("co_str_from_float", {f64_}, {a[0]});
      default: return viaOut("co_str_from_bool", {i32_}, {b_.CreateZExt(a[0], i32_)});
      }
    case BuiltinOp::Panic:
      b_.CreateCall(rt("co_panic_str", void_, {ptrTy_}), {a[0]});
      return nullptr;
    case BuiltinOp::StrLit: {
      llvm::Value *g = b_.CreateGlobalString(rv.strLit, "str");
      return viaOut("co_str_lit", {ptrTy_, i64_}, {g, llvm::ConstantInt::get(i64_, rv.strLit.size())});
    }
    case BuiltinOp::StrConcat:
      return viaOut("co_str_concat", {ptrTy_, ptrTy_}, {a[0], a[1]});
    case BuiltinOp::StrCmp: {
      llvm::Value *c = b_.CreateCall(rt("co_str_cmp", i64_, {ptrTy_, ptrTy_}), {a[0], a[1]});
      llvm::Value *zero = llvm::ConstantInt::get(i64_, 0);
      return binary(rv.cmp, false, c, zero);
    }
    }
    return nullptr;
  }

  void dropLocal(int local) {
    auto *yes = llvm::BasicBlock::Create(ctx_, "drop", fn_);
    auto *done = llvm::BasicBlock::Create(ctx_, "drop.done", fn_);
    b_.CreateCondBr(b_.CreateLoad(i1_, flags_[local]), yes, done);
    b_.SetInsertPoint(yes);
    b_.CreateCall(dropFn(f_->locals[local].type), {slots_[local]});
    b_.CreateStore(b_.getFalse(), flags_[local]);
    b_.CreateBr(done);
    b_.SetInsertPoint(done);
  }

  void emitStmt(const Statement &s) {
    switch (s.kind) {
    case Statement::Assign: {
      llvm::Value *v = rvalue(s.rv);
      Type *ty = f_->locals[s.place.local].type;
      if (ty->kind == TypeKind::Void)
        return;
      if (s.place.isLocal()) {
        if (flags_[s.place.local]) {
          dropLocal(s.place.local); // drop the previous value, if any
          b_.CreateStore(b_.getTrue(), flags_[s.place.local]);
        }
        b_.CreateStore(v, slots_[s.place.local]);
      } else {
        llvm::Value *a = addr(s.place, ty);
        if (ty->needsDrop())
          b_.CreateCall(dropFn(ty), {a});
        b_.CreateStore(v, a);
      }
      return;
    }
    case Statement::Drop:
      if (flags_[s.place.local])
        dropLocal(s.place.local);
      return;
    case Statement::StorageDead:
    case Statement::Nop:
      return;
    }
  }

  void emitTerm(const Terminator &t) {
    switch (t.kind) {
    case Terminator::Goto:
      b_.CreateBr(bbs_[t.target]);
      break;
    case Terminator::If:
      b_.CreateCondBr(operand(t.cond), bbs_[t.thenBB], bbs_[t.elseBB]);
      break;
    case Terminator::Return:
      if (f_->info->ret->kind == TypeKind::Void)
        b_.CreateRetVoid();
      else
        b_.CreateRet(b_.CreateLoad(lt(f_->info->ret), slots_[0]));
      break;
    case Terminator::Unreachable:
      b_.CreateUnreachable();
      break;
    }
  }
};

} // namespace

bool emitObject(const mir::Module &m, const std::string &objPath, const CodegenOptions &opts, std::string &error) {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();

  llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
  const llvm::Target *target = llvm::TargetRegistry::lookupTarget(triple, error);
  if (!target)
    return false;
  llvm::TargetOptions topts;
  auto level = opts.optLevel == 0 ? llvm::CodeGenOptLevel::None
             : opts.optLevel == 1 ? llvm::CodeGenOptLevel::Less
                                  : llvm::CodeGenOptLevel::Default;
  std::unique_ptr<llvm::TargetMachine> tm(
      target->createTargetMachine(triple, "generic", "", topts, llvm::Reloc::PIC_, std::nullopt, level));

  llvm::LLVMContext ctx;
  llvm::Module mod("co", ctx);
  mod.setTargetTriple(triple);
  mod.setDataLayout(tm->createDataLayout());

  CodeGen cg(ctx, mod);
  cg.run(m);

  std::string verr;
  llvm::raw_string_ostream vos(verr);
  if (llvm::verifyModule(mod, &vos)) {
    error = "internal compiler error: invalid LLVM IR generated:\n" + verr;
    return false;
  }

  if (opts.optLevel > 0) {
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;
    llvm::PassBuilder pb(tm.get());
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    auto ol = opts.optLevel == 1 ? llvm::OptimizationLevel::O1
            : opts.optLevel == 2 ? llvm::OptimizationLevel::O2
                                 : llvm::OptimizationLevel::O3;
    llvm::ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(ol);
    mpm.run(mod, mam);
  }

  if (!opts.llvmIrPath.empty()) {
    std::error_code ec;
    llvm::raw_fd_ostream irOut(opts.llvmIrPath, ec, llvm::sys::fs::OF_Text);
    if (ec) {
      error = "cannot write " + opts.llvmIrPath + ": " + ec.message();
      return false;
    }
    mod.print(irOut, nullptr);
  }

  std::error_code ec;
  llvm::raw_fd_ostream out(objPath, ec, llvm::sys::fs::OF_None);
  if (ec) {
    error = "cannot write " + objPath + ": " + ec.message();
    return false;
  }
  llvm::legacy::PassManager pm;
  if (tm->addPassesToEmitFile(pm, out, nullptr, llvm::CodeGenFileType::ObjectFile)) {
    error = "target cannot emit object files";
    return false;
  }
  pm.run(mod);
  out.flush();
  return true;
}

} // namespace co
