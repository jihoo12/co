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
  enum Kind { Name, Ref, Slice, Optional, Result, Map } kind = Name; // Result: `!T`, or `!` (inner null)
  std::string name;  // Name
  std::string pkg;   // Name: the import name in `pkg.Name`, if qualified
  bool mut = false;  // Ref
  std::unique_ptr<TypeExpr> inner; // for Map: the value type
  std::unique_ptr<TypeExpr> key;   // Map
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

enum class ExprKind {
  IntLit, FloatLit, StrLit, BoolLit, NoneLit, Ident, Unary, Binary, Call, Field, Index, StructLit, SliceLit, EnumLit,
  MapLit
};

enum class UnOp {
  Neg,
  Not,
  Deref,
  Ref,
  RefMut,
  // Inserted by sema: re-borrow through an existing reference (`&*r` / `&mut *r`).
  ReborrowShared,
  ReborrowMut,
  Try, // `try x`: unwrap, or return the error / none to the caller
};

// OrElse is `opt or default`.
enum class BinOp { Add, Sub, Mul, Div, Rem, Eq, Ne, Lt, Le, Gt, Ge, And, Or, OrElse };

enum class Builtin { None, Print, Println, Len, Append, Clone, ToInt, ToFloat, ToStr, Panic, MakeError, Delete };

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
struct NoneLitExpr : Expr {
  explicit NoneLitExpr(SourceLoc l) : Expr(ExprKind::NoneLit, l) {}
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
  bool noneCheck = false; // `opt == none` / `opt != none`
  bool orBorrows = false; // `place or x`: look into the optional without consuming it
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
  // On maps: `m[k]` read gives an optional (MapRead); as an assignment
  // target it is the entry itself, inserted if missing (MapWrite).
  enum Mode { Slice, MapRead, MapWrite } mode = Slice;
  bool writeTarget = false; // set before checking: this is on the left of `=`
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
  std::string pkg; // the import name in `pkg.Name{...}`, if qualified
  std::vector<FieldInit> fields;
  StructInfo *st = nullptr;
  StructLitExpr(SourceLoc l, std::string n) : Expr(ExprKind::StructLit, l), name(std::move(n)) {}
};
struct MapLitExpr : Expr {
  TypeExprPtr mapType;
  std::vector<std::pair<ExprPtr, ExprPtr>> entries;
  MapLitExpr(SourceLoc l, TypeExprPtr t) : Expr(ExprKind::MapLit, l), mapType(std::move(t)) {}
};
struct SliceLitExpr : Expr {
  TypeExprPtr elemType;
  std::vector<ExprPtr> elems;
  SliceLitExpr(SourceLoc l, TypeExprPtr t) : Expr(ExprKind::SliceLit, l), elemType(std::move(t)) {}
};

// Constructs an enum value: `Shape.Circle(1.0)`, `Empty`, or an implicit `some(x)` / `none`.
struct EnumLitExpr : Expr {
  EnumInfo *en = nullptr;
  int variant = 0;
  std::vector<ExprPtr> args;
  EnumLitExpr(SourceLoc l, EnumInfo *e, int v) : Expr(ExprKind::EnumLit, l), en(e), variant(v) {}
};

// ----- Statements -----

enum class StmtKind { Block, VarDecl, Expr, Assign, IncDec, If, For, ForRange, Switch, Return, Break, Continue };

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
  std::string name; // index variable ("_" to ignore)
  SourceLoc nameLoc;
  std::string valueName; // optional element variable: `for i, x := range v`
  SourceLoc valueLoc;
  ExprPtr range; // an int (iterates 0..n-1) or a slice
  std::unique_ptr<BlockStmt> body;
  LocalVar *var = nullptr;
  LocalVar *valueVar = nullptr;
  bool overSlice = false;
  bool overMap = false;    // `for k, v := range m`: keys in insertion order
  bool valueByRef = false; // elements that aren't copyable are borrowed
  bool keyByRef = false;   // map keys that aren't copyable are borrowed
  explicit ForRangeStmt(SourceLoc l) : Stmt(StmtKind::ForRange, l) {}
};
struct SwitchCase {
  SourceLoc loc;
  bool isDefault = false;
  std::vector<ExprPtr> values; // values, conditions or patterns
  std::vector<StmtPtr> body;
  // Filled by sema for enum switches:
  std::vector<int> variants;
  std::vector<LocalVar *> bindings; // per payload field; nullptr for `_`
  std::vector<bool> bindByRef;
};
struct SwitchStmt : Stmt {
  ExprPtr tag; // optional: `switch { case cond: }`
  std::vector<SwitchCase> cases;
  // Filled by sema:
  enum Mode { Conditions, Values, Enum } mode = Conditions;
  bool exhaustive = false;
  explicit SwitchStmt(SourceLoc l) : Stmt(StmtKind::Switch, l) {}
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

struct EnumVariantDecl {
  std::string name;
  SourceLoc loc;
  std::vector<TypeExprPtr> fields;
};

struct EnumDecl {
  std::string name;
  SourceLoc loc;
  std::vector<EnumVariantDecl> variants;
  EnumInfo *info = nullptr;
};

struct FuncInfo {
  Package *pkg = nullptr;
  std::string name;    // source name (method name for methods)
  std::string symbol;  // LLVM symbol name
  SourceLoc loc;
  std::vector<Type *> params; // includes receiver for methods
  std::vector<std::string> paramNames;
  Type *ret = nullptr;
  StructInfo *recvStruct = nullptr;
  FuncDecl *decl = nullptr;
};

// ----- Packages -----

struct Import {
  std::string name; // how the file refers to the package (its last path element, or an alias)
  std::string path;
  SourceLoc loc;
  Package *pkg = nullptr;
};

struct SourceFile {
  std::string path;
  Package *pkg = nullptr;
  std::vector<Import> imports;
};

// A directory of .co files sharing one namespace (or, for the main package,
// possibly a single file). Names starting with an upper-case letter are
// visible to importers.
struct Package {
  std::string path; // import path; empty for the main package
  std::string name; // last element of the path; "main" for the main package
  // Filled by sema:
  std::unordered_map<std::string, StructInfo *> structs;
  std::unordered_map<std::string, EnumInfo *> enums;
  // Variant names usable without the enum prefix; nullptr if ambiguous.
  std::unordered_map<std::string, std::pair<EnumInfo *, int>> variants;
  std::unordered_map<std::string, FuncInfo *> funcs;
  bool isMain() const { return path.empty(); }
};

inline bool isExported(const std::string &name) { return !name.empty() && name[0] >= 'A' && name[0] <= 'Z'; }

// The whole program: every package, with declarations of all of them in one
// list per kind. A declaration's package is that of its file (loc.file).
struct Program {
  std::vector<std::unique_ptr<Package>> packages; // packages[0] is the main package
  std::vector<std::unique_ptr<SourceFile>> files;  // indexed by SourceLoc::file
  std::vector<std::unique_ptr<StructDecl>> structs;
  std::vector<std::unique_ptr<EnumDecl>> enums;
  std::vector<std::unique_ptr<FuncDecl>> funcs;
  // Filled by sema:
  std::vector<std::unique_ptr<StructInfo>> structInfos;
  std::vector<std::unique_ptr<EnumInfo>> enumInfos;
  std::vector<std::unique_ptr<FuncInfo>> funcInfos;
};

} // namespace co
