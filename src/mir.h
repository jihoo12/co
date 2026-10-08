#pragma once
#include "ast.h"

#include <string>
#include <vector>

namespace co::mir {

// A MIR function is a control-flow graph of basic blocks operating on
// numbered locals. Local 0 is the return value; params follow.

struct Local {
  std::string name; // empty for compiler temporaries
  Type *type = nullptr;
  SourceLoc loc;
};

struct Proj {
  enum Kind { Deref, Field, Index } kind;
  int field = -1;      // Field
  int indexLocal = -1; // Index: local holding the index
};

struct Place {
  int local = -1;
  std::vector<Proj> proj;

  bool isLocal() const { return proj.empty(); }
  Place withProj(Proj p) const {
    Place q = *this;
    q.proj.push_back(p);
    return q;
  }
};

struct Constant {
  enum Kind { Int, Float, Bool, Zero } kind = Int;
  int64_t i = 0;
  double f = 0;
  bool b = false;
};

struct Operand {
  enum Kind { Copy, Move, Const } kind = Const;
  Place place;
  Constant c;
  Type *type = nullptr;
};

enum class BuiltinOp {
  Print,     // print args without separators
  Println,   // print args separated by spaces, then newline
  Len,       // len(&string | &[]T)
  Append,    // append([]T, T) -> []T
  Push,      // push(&mut []T, T)  (in-place append)
  Clone,     // clone(&T) -> T
  ToInt,
  ToFloat,
  ToStr,
  Panic,     // panic(&string)
  StrLit,    // creates an owned string from `strLit`
  StrConcat, // (&string, &string) -> string
  StrCmp,    // (&string, &string) -> bool using `cmp`
};

struct Rvalue {
  enum Kind { Use, BinaryOp, UnaryOp, Ref, Aggregate, SliceLit, Call, Builtin } kind = Use;
  BinOp bop = BinOp::Add;
  UnOp uop = UnOp::Neg;
  bool mut = false;  // Ref
  Place place;       // Ref
  std::vector<Operand> ops;
  FuncInfo *func = nullptr; // Call
  BuiltinOp builtin = BuiltinOp::Print;
  BinOp cmp = BinOp::Eq;    // StrCmp
  std::string strLit;       // StrLit
  Type *type = nullptr;     // result type
};

struct Statement {
  enum Kind { Assign, Drop, StorageDead, Nop } kind = Nop;
  Place place; // Assign: destination; Drop: dropped place
  Rvalue rv;
  int local = -1; // StorageDead
  SourceLoc loc;
};

struct Terminator {
  enum Kind { Goto, If, Return, Unreachable } kind = Unreachable;
  int target = -1;              // Goto
  Operand cond;                 // If
  int thenBB = -1, elseBB = -1; // If
  SourceLoc loc;
};

struct BasicBlock {
  std::vector<Statement> stmts;
  Terminator term;
  bool terminated = false;
};

struct Function {
  FuncInfo *info = nullptr;
  std::vector<Local> locals;
  int numParams = 0; // params are locals 1..numParams
  std::vector<BasicBlock> blocks;
  SourceLoc endLoc;

  std::string placeName(const Place &p) const;
};

struct Module {
  std::vector<Function> funcs;
};

void print(const Function &f, std::string &out);

} // namespace co::mir
