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
struct Package;

// `None` is the type of the `none` literal before it is converted to some `?T`.
// Optionals `?T` are enums with variants `none` and `some(T)`; results `!T`
// are enums with variants `ok(T)` and `err(error)`. `error` holds a message.
enum class TypeKind { Void, Int, Float, Bool, String, Error, Struct, Enum, Ref, Slice, Map, None };

// Types are interned by TypeContext, so they can be compared by pointer.
struct Type {
  TypeKind kind;
  bool mut = false;       // for Ref: &mut T
  Type *inner = nullptr;  // for Ref and Slice; the value type of a Map
  Type *key = nullptr;    // for Map
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
  bool isResult() const;
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
  Package *pkg = nullptr;
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
  Package *pkg = nullptr; // null for optionals and results
  std::string name;
  SourceLoc loc;
  std::vector<Variant> variants;
  std::unordered_map<std::string, int> variantIndex;
  bool needsDrop = false;
  bool isCopy = true;
  Type *optionalOf = nullptr; // set for `?T`: variants are none (0) and some(T) (1)
  Type *resultOf = nullptr;   // set for `!T`: variants are ok(T) (0) and err(error) (1); T may be void

  // For `?T` / `!T`: the variant holding the value, and the one meaning "no value".
  int valueVariant() const { return optionalOf ? 1 : 0; }
  int failVariant() const { return optionalOf ? 0 : 1; }
  Type *valueType() const { return optionalOf ? optionalOf : resultOf; }
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
  Type *map(Type *key, Type *value);
  Type *structTy(StructInfo *st);
  Type *enumTy(EnumInfo *en);
  Type *optional(Type *inner);
  Type *noneTy() { return &none_; }
  Type *errorTy() { return &error_; }
  Type *result(Type *inner);

private:
  Type void_, int_, float_, bool_, string_, none_, error_;
  std::map<std::pair<Type *, bool>, std::unique_ptr<Type>> refs_;
  std::map<Type *, std::unique_ptr<Type>> slices_;
  std::map<std::pair<Type *, Type *>, std::unique_ptr<Type>> maps_;
  std::map<StructInfo *, std::unique_ptr<Type>> structs_;
  std::map<EnumInfo *, std::unique_ptr<Type>> enums_;
  std::map<Type *, std::unique_ptr<EnumInfo>> optionals_, results_;
};

} // namespace co
