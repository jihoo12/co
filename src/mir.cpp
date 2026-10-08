#include "mir.h"

namespace co::mir {

std::string Function::placeName(const Place &p) const {
  const Local &l = locals[p.local];
  std::string s = l.name.empty() ? "temporary value" : l.name;
  if (l.name.empty() && !p.proj.empty())
    s = "(temporary)";
  Type *ty = l.type;
  for (auto &pr : p.proj) {
    switch (pr.kind) {
    case Proj::Deref:
      // Field access through a reference prints like the source (`p.x`).
      ty = ty->inner;
      if (&pr == &p.proj.back())
        s = "*" + s;
      break;
    case Proj::Field:
      s += "." + ty->st->fields[pr.field].name;
      ty = ty->st->fields[pr.field].type;
      break;
    case Proj::Index:
      s += "[..]";
      ty = ty->inner;
      break;
    case Proj::VariantField:
      ty = ty->en->variants[pr.variant].fields[pr.field];
      break;
    }
  }
  return s;
}

static std::string placeStr(const Function &f, const Place &p) {
  std::string s = "_" + std::to_string(p.local);
  for (auto &pr : p.proj) {
    switch (pr.kind) {
    case Proj::Deref: s = "(*" + s + ")"; break;
    case Proj::Field: s += "." + std::to_string(pr.field); break;
    case Proj::Index: s += "[_" + std::to_string(pr.indexLocal) + "]"; break;
    case Proj::VariantField:
      s = "(" + s + " as " + std::to_string(pr.variant) + ")." + std::to_string(pr.field);
      break;
    }
  }
  return s;
}

static std::string opStr(const Function &f, const Operand &o) {
  switch (o.kind) {
  case Operand::Copy: return "copy " + placeStr(f, o.place);
  case Operand::Move: return "move " + placeStr(f, o.place);
  case Operand::Const:
    switch (o.c.kind) {
    case Constant::Int: return std::to_string(o.c.i);
    case Constant::Float: return std::to_string(o.c.f);
    case Constant::Bool: return o.c.b ? "true" : "false";
    case Constant::Zero: return "zeroed";
    }
  }
  return "?";
}

static const char *bopStr(BinOp op) {
  switch (op) {
  case BinOp::Add: return "Add";
  case BinOp::Sub: return "Sub";
  case BinOp::Mul: return "Mul";
  case BinOp::Div: return "Div";
  case BinOp::Rem: return "Rem";
  case BinOp::Eq: return "Eq";
  case BinOp::Ne: return "Ne";
  case BinOp::Lt: return "Lt";
  case BinOp::Le: return "Le";
  case BinOp::Gt: return "Gt";
  case BinOp::Ge: return "Ge";
  case BinOp::And: return "And";
  case BinOp::Or: return "Or";
  case BinOp::OrElse: return "OrElse";
  }
  return "?";
}

static const char *builtinStr(BuiltinOp b) {
  switch (b) {
  case BuiltinOp::Print: return "print";
  case BuiltinOp::Println: return "println";
  case BuiltinOp::Len: return "len";
  case BuiltinOp::Append: return "append";
  case BuiltinOp::Push: return "push";
  case BuiltinOp::Clone: return "clone";
  case BuiltinOp::Convert: return "convert";
  case BuiltinOp::CStr: return "cstr";
  case BuiltinOp::ToStr: return "str";
  case BuiltinOp::Panic: return "panic";
  case BuiltinOp::StrLit: return "str_lit";
  case BuiltinOp::StrConcat: return "str_concat";
  case BuiltinOp::StrCmp: return "str_cmp";
  case BuiltinOp::MakeError: return "error";
  case BuiltinOp::MapGet: return "map_get";
  case BuiltinOp::MapSlot: return "map_slot";
  case BuiltinOp::MapDelete: return "map_delete";
  case BuiltinOp::MapUsed: return "map_used";
  case BuiltinOp::MapAlive: return "map_alive";
  case BuiltinOp::MapKeyAt: return "map_key_at";
  case BuiltinOp::MapValAt: return "map_val_at";
  }
  return "?";
}

void print(const Function &f, std::string &out) {
  out += "fn " + f.info->symbol + " {\n";
  for (size_t i = 0; i < f.locals.size(); i++) {
    out += "  let _" + std::to_string(i) + ": " + f.locals[i].type->str();
    if (!f.locals[i].name.empty())
      out += "  // " + f.locals[i].name;
    out += "\n";
  }
  for (size_t b = 0; b < f.blocks.size(); b++) {
    out += "  bb" + std::to_string(b) + ":\n";
    for (auto &s : f.blocks[b].stmts) {
      out += "    ";
      switch (s.kind) {
      case Statement::Assign: {
        out += placeStr(f, s.place) + " = ";
        const Rvalue &rv = s.rv;
        auto ops = [&]() {
          std::string r;
          for (size_t i = 0; i < rv.ops.size(); i++)
            r += (i ? ", " : "") + opStr(f, rv.ops[i]);
          return r;
        };
        switch (rv.kind) {
        case Rvalue::Use: out += opStr(f, rv.ops[0]); break;
        case Rvalue::BinaryOp: out += std::string(bopStr(rv.bop)) + "(" + ops() + ")"; break;
        case Rvalue::UnaryOp: out += std::string(rv.uop == UnOp::Neg ? "Neg" : "Not") + "(" + ops() + ")"; break;
        case Rvalue::Ref: out += std::string(rv.mut ? "&mut " : "&") + placeStr(f, rv.place); break;
        case Rvalue::Discriminant: out += "discriminant(" + placeStr(f, rv.place) + ")"; break;
        case Rvalue::Aggregate:
          out += rv.type->str();
          if (rv.variant >= 0)
            out += "::" + rv.type->en->variants[rv.variant].name;
          out += " { " + ops() + " }";
          break;
        case Rvalue::SliceLit: out += rv.type->str() + " [" + ops() + "]"; break;
        case Rvalue::Call: out += rv.func->symbol + "(" + ops() + ")"; break;
        case Rvalue::Builtin:
          out += std::string(builtinStr(rv.builtin)) + "(" + ops() + ")";
          if (rv.builtin == BuiltinOp::StrLit)
            out += " \"" + rv.strLit + "\"";
          break;
        }
        break;
      }
      case Statement::Drop: out += "drop(" + placeStr(f, s.place) + ")"; break;
      case Statement::StorageDead: out += "StorageDead(_" + std::to_string(s.local) + ")"; break;
      case Statement::Nop: out += "nop"; break;
      }
      out += "\n";
    }
    const Terminator &t = f.blocks[b].term;
    out += "    ";
    switch (t.kind) {
    case Terminator::Goto: out += "goto bb" + std::to_string(t.target); break;
    case Terminator::If:
      out += "if " + opStr(f, t.cond) + " -> [bb" + std::to_string(t.thenBB) + ", bb" +
             std::to_string(t.elseBB) + "]";
      break;
    case Terminator::Return: out += "return"; break;
    case Terminator::Unreachable: out += "unreachable"; break;
    }
    out += "\n";
  }
  out += "}\n\n";
}

} // namespace co::mir
