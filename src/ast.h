#pragma once
#include "diag.h"
#include "types.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace co {

// ----- Type syntax -----

struct TypeExpr {
  enum Kind { Name, Ref, Slice } kind = Name;
  std::string name;  // Name
  bool mut = false;  // Ref
  std::unique_ptr<TypeExpr> inner;
  SourceLoc loc;
};
using TypeExprPtr = std::unique_ptr<TypeExpr>;

// A local variable or parameter, created by semantic analysis.
struct LocalVar {
  std::string name;
  Type *type = nullptr;
  SourceLoc loc;
};

// ----- Expressions -----

enum class ExprKind { IntLit, FloatLit, StrLit, BoolLit, Ident, Unary, Binary, Call, Field, Index, StructLit, SliceLit };

enum class UnOp {
  Neg,
  Not,
  Deref,
  Ref,
  RefMut,
  // Inserted by sema: re-borrow through an existing reference (`&*r` / `&mut *r`).
  ReborrowShared,
  ReborrowMut,
};

enum class BinOp { Add, Sub, Mul, Div, Rem, Eq, Ne, Lt, Le, Gt, Ge, And, Or };

enum class Builtin { None, Print, Println, Len, Append, Clone, ToInt, ToFloat, ToStr, Panic };

struct Expr {
  ExprKind kind;
  SourceLoc loc;
  Type *type = nullptr; // filled by sema
  explicit Expr(ExprKind k, SourceLoc l) : kind(k), loc(l) {}
  virtual ~Expr() = default;
};
using ExprPtr = std::unique_ptr<Expr>;

struct IntLitExpr : Expr {
  int64_t value;
  IntLitExpr(SourceLoc l, int64_t v) : Expr(ExprKind::IntLit, l), value(v) {}
};
struct FloatLitExpr : Expr {
  double value;
  FloatLitExpr(SourceLoc l, double v) : Expr(ExprKind::FloatLit, l), value(v) {}
};
struct StrLitExpr : Expr {
  std::string value;
  StrLitExpr(SourceLoc l, std::string v) : Expr(ExprKind::StrLit, l), value(std::move(v)) {}
};
struct BoolLitExpr : Expr {
  bool value;
  BoolLitExpr(SourceLoc l, bool v) : Expr(ExprKind::BoolLit, l), value(v) {}
};
struct IdentExpr : Expr {
  std::string name;
  LocalVar *var = nullptr;
  IdentExpr(SourceLoc l, std::string n) : Expr(ExprKind::Ident, l), name(std::move(n)) {}
};
struct UnaryExpr : Expr {
  UnOp op;
  ExprPtr operand;
  UnaryExpr(SourceLoc l, UnOp o, ExprPtr e) : Expr(ExprKind::Unary, l), op(o), operand(std::move(e)) {}
};
struct BinaryExpr : Expr {
  BinOp op;
  ExprPtr lhs, rhs;
  BinaryExpr(SourceLoc l, BinOp o, ExprPtr a, ExprPtr b)
      : Expr(ExprKind::Binary, l), op(o), lhs(std::move(a)), rhs(std::move(b)) {}
};
struct CallExpr : Expr {
  ExprPtr callee;
  std::vector<ExprPtr> args;
  // Resolved by sema. For method calls the receiver becomes args[0].
  FuncInfo *func = nullptr;
  Builtin builtin = Builtin::None;
  bool receiverLast = false; // evaluate args[0] after the others (two-phase borrow)
  CallExpr(SourceLoc l, ExprPtr c) : Expr(ExprKind::Call, l), callee(std::move(c)) {}
};
struct FieldExpr : Expr {
  ExprPtr base;
  std::string name;
  int index = -1;
  bool autoDeref = false; // base is a reference
  FieldExpr(SourceLoc l, ExprPtr b, std::string n)
      : Expr(ExprKind::Field, l), base(std::move(b)), name(std::move(n)) {}
};
struct IndexExpr : Expr {
  ExprPtr base, index;
  bool autoDeref = false;
  IndexExpr(SourceLoc l, ExprPtr b, ExprPtr i)
      : Expr(ExprKind::Index, l), base(std::move(b)), index(std::move(i)) {}
};
struct FieldInit {
  std::string name;
  SourceLoc loc;
  ExprPtr value;
  int index = -1;
};
struct StructLitExpr : Expr {
  std::string name;
  std::vector<FieldInit> fields;
  StructInfo *st = nullptr;
  StructLitExpr(SourceLoc l, std::string n) : Expr(ExprKind::StructLit, l), name(std::move(n)) {}
};
struct SliceLitExpr : Expr {
  TypeExprPtr elemType;
  std::vector<ExprPtr> elems;
  SliceLitExpr(SourceLoc l, TypeExprPtr t) : Expr(ExprKind::SliceLit, l), elemType(std::move(t)) {}
};

// ----- Statements -----

enum class StmtKind { Block, VarDecl, Expr, Assign, IncDec, If, For, ForRange, Return, Break, Continue };

struct Stmt {
  StmtKind kind;
  SourceLoc loc;
  explicit Stmt(StmtKind k, SourceLoc l) : kind(k), loc(l) {}
  virtual ~Stmt() = default;
};
using StmtPtr = std::unique_ptr<Stmt>;

struct BlockStmt : Stmt {
  std::vector<StmtPtr> stmts;
  SourceLoc endLoc;
  explicit BlockStmt(SourceLoc l) : Stmt(StmtKind::Block, l) {}
};
struct VarDeclStmt : Stmt {
  std::string name;
  TypeExprPtr typeExpr; // optional
  ExprPtr init;         // optional: zero value if absent
  LocalVar *var = nullptr;
  VarDeclStmt(SourceLoc l, std::string n) : Stmt(StmtKind::VarDecl, l), name(std::move(n)) {}
};
struct ExprStmt : Stmt {
  ExprPtr expr;
  ExprStmt(SourceLoc l, ExprPtr e) : Stmt(StmtKind::Expr, l), expr(std::move(e)) {}
};
enum class AssignOp { Set, Add, Sub, Mul, Div, Rem };
struct AssignStmt : Stmt {
  ExprPtr lhs;
  AssignOp op;
  ExprPtr rhs;
  bool appendInPlace = false; // `x = append(x, v)` rewritten to an in-place push
  AssignStmt(SourceLoc l, ExprPtr a, AssignOp o, ExprPtr b)
      : Stmt(StmtKind::Assign, l), lhs(std::move(a)), op(o), rhs(std::move(b)) {}
};
struct IncDecStmt : Stmt {
  ExprPtr target;
  bool inc;
  IncDecStmt(SourceLoc l, ExprPtr t, bool i) : Stmt(StmtKind::IncDec, l), target(std::move(t)), inc(i) {}
};
struct IfStmt : Stmt {
  ExprPtr cond;
  std::unique_ptr<BlockStmt> then;
  StmtPtr els; // BlockStmt or IfStmt, optional
  explicit IfStmt(SourceLoc l) : Stmt(StmtKind::If, l) {}
};
struct ForStmt : Stmt {
  StmtPtr init; // optional
  ExprPtr cond; // optional
  StmtPtr post; // optional
  std::unique_ptr<BlockStmt> body;
  explicit ForStmt(SourceLoc l) : Stmt(StmtKind::For, l) {}
};
struct ForRangeStmt : Stmt {
  std::string name;
  SourceLoc nameLoc;
  ExprPtr range; // an int: iterates 0..n
  std::unique_ptr<BlockStmt> body;
  LocalVar *var = nullptr;
  explicit ForRangeStmt(SourceLoc l) : Stmt(StmtKind::ForRange, l) {}
};
struct ReturnStmt : Stmt {
  ExprPtr value;
  explicit ReturnStmt(SourceLoc l) : Stmt(StmtKind::Return, l) {}
};
struct BranchStmt : Stmt {
  explicit BranchStmt(StmtKind k, SourceLoc l) : Stmt(k, l) {}
};

// ----- Declarations -----

struct Param {
  std::string name;
  TypeExprPtr type;
  SourceLoc loc;
  LocalVar *var = nullptr;
};

struct FuncDecl {
  std::string name;
  SourceLoc loc;
  std::optional<Param> receiver;
  std::vector<Param> params;
  TypeExprPtr ret; // optional
  std::unique_ptr<BlockStmt> body;
  FuncInfo *info = nullptr;
  std::vector<std::unique_ptr<LocalVar>> locals; // owned storage for all LocalVars
};

struct StructFieldDecl {
  std::string name;
  TypeExprPtr type;
  SourceLoc loc;
};

struct StructDecl {
  std::string name;
  SourceLoc loc;
  std::vector<StructFieldDecl> fields;
  StructInfo *info = nullptr;
};

struct FuncInfo {
  std::string name;    // source name (method name for methods)
  std::string symbol;  // LLVM symbol name
  SourceLoc loc;
  std::vector<Type *> params; // includes receiver for methods
  std::vector<std::string> paramNames;
  Type *ret = nullptr;
  StructInfo *recvStruct = nullptr;
  FuncDecl *decl = nullptr;
};

struct Program {
  std::vector<std::unique_ptr<StructDecl>> structs;
  std::vector<std::unique_ptr<FuncDecl>> funcs;
  // Filled by sema:
  std::vector<std::unique_ptr<StructInfo>> structInfos;
  std::vector<std::unique_ptr<FuncInfo>> funcInfos;
};

} // namespace co
