#include "types.h"

namespace co {

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
  case TypeKind::Slice:
    return false;
  case TypeKind::Struct:
    // Structs cannot hold references, so "copy" is exactly "owns nothing".
    return !st->needsDrop;
  }
  return false;
}

bool Type::needsDrop() const {
  switch (kind) {
  case TypeKind::String:
  case TypeKind::Slice:
    return true;
  case TypeKind::Struct:
    return st->needsDrop;
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
  case TypeKind::Struct: return st->name;
  case TypeKind::Ref: return (mut ? "&mut " : "&") + inner->str();
  case TypeKind::Slice: return "[]" + inner->str();
  }
  return "?";
}

TypeContext::TypeContext() {
  void_.kind = TypeKind::Void;
  int_.kind = TypeKind::Int;
  float_.kind = TypeKind::Float;
  bool_.kind = TypeKind::Bool;
  string_.kind = TypeKind::String;
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
