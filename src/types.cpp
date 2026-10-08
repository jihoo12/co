#include "types.h"

#include "ast.h"

namespace co {

// Types from other packages print as `pkg.Name`, like in source code.
static std::string qualified(const Package *pkg, const std::string &name) {
  return pkg && !pkg->isMain() ? pkg->name + "." + name : name;
}

bool Type::isCopy() const {
  switch (kind) {
  case TypeKind::Void:
  case TypeKind::Int:
  case TypeKind::Float:
  case TypeKind::Bool:
    return true;
  case TypeKind::Ref:
    return !mut; // shared references can be freely copied; &mut is unique
  case TypeKind::String:
  case TypeKind::Error:
  case TypeKind::Slice:
  case TypeKind::Map:
    return false;
  case TypeKind::Struct:
    // Structs cannot hold references, so "copy" is exactly "owns nothing".
    return !st->needsDrop;
  case TypeKind::Enum:
    if (en->resultOf)
      return false;
    return en->optionalOf ? en->optionalOf->isCopy() : en->isCopy;
  case TypeKind::None:
    return true;
  }
  return false;
}

bool Type::isOptional() const { return kind == TypeKind::Enum && en->optionalOf; }
bool Type::isResult() const { return kind == TypeKind::Enum && en->resultOf; }

bool Type::containsRef() const {
  switch (kind) {
  case TypeKind::Ref: return true;
  case TypeKind::Slice: return inner->containsRef();
  case TypeKind::Map: return inner->containsRef();
  case TypeKind::Enum: return en->valueType() && en->valueType()->containsRef();
  default: return false;
  }
}

bool Type::needsDrop() const {
  switch (kind) {
  case TypeKind::String:
  case TypeKind::Error:
  case TypeKind::Slice:
  case TypeKind::Map:
    return true;
  case TypeKind::Struct:
    return st->needsDrop;
  case TypeKind::Enum:
    if (en->resultOf)
      return true;
    return en->optionalOf ? en->optionalOf->needsDrop() : en->needsDrop;
  default:
    return false;
  }
}

std::string Type::str() const {
  switch (kind) {
  case TypeKind::Void: return "void";
  case TypeKind::Int: return "int";
  case TypeKind::Float: return "float";
  case TypeKind::Bool: return "bool";
  case TypeKind::String: return "string";
  case TypeKind::Error: return "error";
  case TypeKind::Struct: return qualified(st->pkg, st->name);
  case TypeKind::Enum: return qualified(en->pkg, en->name);
  case TypeKind::None: return "none";
  case TypeKind::Ref: return (mut ? "&mut " : "&") + inner->str();
  case TypeKind::Slice: return "[]" + inner->str();
  case TypeKind::Map: return "map[" + key->str() + "]" + inner->str();
  }
  return "?";
}

TypeContext::TypeContext() {
  void_.kind = TypeKind::Void;
  int_.kind = TypeKind::Int;
  float_.kind = TypeKind::Float;
  bool_.kind = TypeKind::Bool;
  string_.kind = TypeKind::String;
  none_.kind = TypeKind::None;
  error_.kind = TypeKind::Error;
}

Type *TypeContext::result(Type *inner) {
  auto &slot = results_[inner];
  if (!slot) {
    slot = std::make_unique<EnumInfo>();
    slot->name = inner->kind == TypeKind::Void ? "!" : "!" + inner->str();
    slot->resultOf = inner;
    if (inner->kind == TypeKind::Void)
      slot->variants.push_back({"ok", {}, {}});
    else
      slot->variants.push_back({"ok", {inner}, {}});
    slot->variants.push_back({"err", {&error_}, {}});
    slot->variantIndex["ok"] = 0;
    slot->variantIndex["err"] = 1;
  }
  return enumTy(slot.get());
}

Type *TypeContext::enumTy(EnumInfo *en) {
  auto &slot = enums_[en];
  if (!slot) {
    slot = std::make_unique<Type>();
    slot->kind = TypeKind::Enum;
    slot->en = en;
  }
  return slot.get();
}

Type *TypeContext::optional(Type *inner) {
  auto &slot = optionals_[inner];
  if (!slot) {
    slot = std::make_unique<EnumInfo>();
    slot->name = "?" + inner->str();
    slot->optionalOf = inner;
    slot->variants.push_back({"none", {}, {}});
    slot->variants.push_back({"some", {inner}, {}});
    slot->variantIndex["none"] = 0;
    slot->variantIndex["some"] = 1;
  }
  return enumTy(slot.get());
}

Type *TypeContext::ref(Type *inner, bool mut) {
  auto &slot = refs_[{inner, mut}];
  if (!slot) {
    slot = std::make_unique<Type>();
    slot->kind = TypeKind::Ref;
    slot->mut = mut;
    slot->inner = inner;
  }
  return slot.get();
}

Type *TypeContext::slice(Type *elem) {
  auto &slot = slices_[elem];
  if (!slot) {
    slot = std::make_unique<Type>();
    slot->kind = TypeKind::Slice;
    slot->inner = elem;
  }
  return slot.get();
}

Type *TypeContext::map(Type *key, Type *value) {
  auto &slot = maps_[{key, value}];
  if (!slot) {
    slot = std::make_unique<Type>();
    slot->kind = TypeKind::Map;
    slot->key = key;
    slot->inner = value;
  }
  return slot.get();
}

Type *TypeContext::structTy(StructInfo *st) {
  auto &slot = structs_[st];
  if (!slot) {
    slot = std::make_unique<Type>();
    slot->kind = TypeKind::Struct;
    slot->st = st;
  }
  return slot.get();
}

} // namespace co
