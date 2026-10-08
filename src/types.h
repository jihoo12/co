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

// `None` is the type of the `none` literal before it is converted to some `?T`,
// and `Nil` that of `nil` before it becomes a C pointer `*T`. `IntN` are the
// sized integers (int8 ... uint64; `int` itself is Int), and `Ptr` is a raw C
// pointer, which is not borrow-checked and only meant for talking to C.
// `Func` is a function pointer `func(T...) R` (R in `inner`, void if none),
// as C has them: only numbers, bool and pointers go in and out.
// Optionals `?T` are enums with variants `none` and `some(T)`; results `!T`
// are enums with variants `ok(T)` and `err(error)`. `error` holds a message.
// `Array` is a fixed-size array `[N]T`, stored inline like a C array.
enum class TypeKind { Void, Int, Float, Bool, String, Error, Struct, Enum, Ref, Slice, Map, None, IntN, Float32, Ptr, Nil, Func, Array };

// Types are interned by TypeContext, so they can be compared by pointer.
struct Type {
  TypeKind kind;
  bool mut = false;       // for Ref: &mut T
  int bits = 64;          // for IntN
  bool isUnsigned = false; // for IntN
  Type *inner = nullptr;  // for Ref, Slice, Array and Ptr; the value type of a Map
  int64_t len = 0;        // for Array
  Type *key = nullptr;    // for Map
  std::vector<Type *> params; // for Func
  StructInfo *st = nullptr;
  EnumInfo *en = nullptr;

  bool isRef() const { return kind == TypeKind::Ref; }
  bool isMutRef() const { return kind == TypeKind::Ref && mut; }
  bool isInteger() const { return kind == TypeKind::Int || kind == TypeKind::IntN; }
  bool isFloat() const { return kind == TypeKind::Float || kind == TypeKind::Float32; }
  bool isNumeric() const { return isInteger() || isFloat(); }
  bool isSigned() const { return kind == TypeKind::Int || (kind == TypeKind::IntN && !isUnsigned); }
  bool isPtr() const { return kind == TypeKind::Ptr; }
  bool isFunc() const { return kind == TypeKind::Func; }
  // Can C see this type as is? Numbers, bool, pointers, and structs of those.
  bool isCCompatible() const;
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
  Type *array(Type *elem, int64_t len);
  Type *map(Type *key, Type *value);
  Type *structTy(StructInfo *st);
  Type *enumTy(EnumInfo *en);
  Type *optional(Type *inner);
  Type *noneTy() { return &none_; }
  Type *errorTy() { return &error_; }
  Type *result(Type *inner);
  Type *intN(int bits, bool isUnsigned); // intN(64, false) is int
  Type *float32Ty() { return &float32_; }
  Type *ptr(Type *inner);
  Type *func(const std::vector<Type *> &params, Type *ret);
  Type *nilTy() { return &nil_; }
  // The numeric type named `name` (int, uint8, byte, float32, ...), or null.
  Type *numericByName(const std::string &name);

private:
  Type void_, int_, float_, bool_, string_, none_, error_, float32_, nil_;
  std::map<std::pair<int, bool>, std::unique_ptr<Type>> intNs_;
  std::map<Type *, std::unique_ptr<Type>> ptrs_;
  std::map<std::pair<std::vector<Type *>, Type *>, std::unique_ptr<Type>> funcs_;
  std::map<std::pair<Type *, bool>, std::unique_ptr<Type>> refs_;
  std::map<Type *, std::unique_ptr<Type>> slices_;
  std::map<std::pair<Type *, int64_t>, std::unique_ptr<Type>> arrays_;
  std::map<std::pair<Type *, Type *>, std::unique_ptr<Type>> maps_;
  std::map<StructInfo *, std::unique_ptr<Type>> structs_;
  std::map<EnumInfo *, std::unique_ptr<Type>> enums_;
  std::map<Type *, std::unique_ptr<EnumInfo>> optionals_, results_;
};

} // namespace co
