#pragma once
#include "diag.h"

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace co {

struct StructInfo;
struct EnumInfo;
struct FuncInfo;

// `None` is the type of the `none` literal before it is converted to some `?T`.
// Optionals `?T` are enums with variants `none` and `some(T)`.
enum class TypeKind { Void, Int, Float, Bool, String, Struct, Enum, Ref, Slice, None };

// Types are interned by TypeContext, so they can be compared by pointer.
struct Type {
  TypeKind kind;
  bool mut = false;       // for Ref: &mut T
  Type *inner = nullptr;  // for Ref and Slice
  StructInfo *st = nullptr;
  EnumInfo *en = nullptr;

  bool isRef() const { return kind == TypeKind::Ref; }
  bool isMutRef() const { return kind == TypeKind::Ref && mut; }
  bool isNumeric() const { return kind == TypeKind::Int || kind == TypeKind::Float; }
  // Values of Copy types are duplicated on use; everything else is moved.
  bool isCopy() const;
  // Types that own heap memory and must be freed when they go out of scope.
  bool needsDrop() const;
  std::string str() const;
  bool isOptional() const;
  // References, possibly nested in optionals or slices.
  bool containsRef() const;
  // Strips one level of reference, if any.
  Type *derefAll() { return isRef() ? inner : this; }
};

struct FieldInfo {
  std::string name;
  Type *type = nullptr;
  SourceLoc loc;
};

struct StructInfo {
  std::string name;
  SourceLoc loc;
  std::vector<FieldInfo> fields;
  std::unordered_map<std::string, int> fieldIndex;
  std::unordered_map<std::string, FuncInfo *> methods;
  bool needsDrop = false;
};

struct Variant {
  std::string name;
  std::vector<Type *> fields;
  SourceLoc loc;
};

struct EnumInfo {
  std::string name;
  SourceLoc loc;
  std::vector<Variant> variants;
  std::unordered_map<std::string, int> variantIndex;
  bool needsDrop = false;
  bool isCopy = true;
  Type *optionalOf = nullptr; // set for `?T`: variants are none (0) and some(T) (1)
};

class TypeContext {
public:
  TypeContext();
  Type *voidTy() { return &void_; }
  Type *intTy() { return &int_; }
  Type *floatTy() { return &float_; }
  Type *boolTy() { return &bool_; }
  Type *stringTy() { return &string_; }
  Type *ref(Type *inner, bool mut);
  Type *slice(Type *elem);
  Type *structTy(StructInfo *st);
  Type *enumTy(EnumInfo *en);
  Type *optional(Type *inner);
  Type *noneTy() { return &none_; }

private:
  Type void_, int_, float_, bool_, string_, none_;
  std::map<std::pair<Type *, bool>, std::unique_ptr<Type>> refs_;
  std::map<Type *, std::unique_ptr<Type>> slices_;
  std::map<StructInfo *, std::unique_ptr<Type>> structs_;
  std::map<EnumInfo *, std::unique_ptr<Type>> enums_;
  std::map<Type *, std::unique_ptr<EnumInfo>> optionals_;
};

} // namespace co
