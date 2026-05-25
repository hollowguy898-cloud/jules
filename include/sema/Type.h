#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <optional>
#include <cstdint>
#include <cassert>
#include <sstream>
#include <algorithm>
#include <type_traits>

namespace jules {

// ============================================================================
// Primitive Kind Enumeration
// ============================================================================
enum class PrimitiveKind : uint8_t {
    U8,
    U16,
    U32,
    U64,
    USize,
    I8,
    I16,
    I32,
    I64,
    ISize,
    F32,
    F64,
    Bool,
    Void,
    Count // sentinel
};

// ============================================================================
// Smart Pointer Kind Enumeration
// ============================================================================
enum class SmartPointerKind : uint8_t {
    Box,
    Rc,
    Arc
};

// ============================================================================
// Type Kind Enumeration (for RTTI on Type objects)
// ============================================================================
enum class TypeKind : uint8_t {
    Primitive,
    Struct,
    Enum,
    Pointer,
    Reference,
    MutReference,
    Slice,
    Fn,
    SmartPointer,
    Poison,
    Error,
    Allocator
};

// ============================================================================
// TypeId - Lightweight handle to an interned type
// ============================================================================
class Type;

class TypeId {
public:
    TypeId() : ptr_(nullptr) {}
    explicit TypeId(const Type* p) : ptr_(p) {}

    const Type* operator->() const { return ptr_; }
    const Type& operator*() const { return *ptr_; }

    bool operator==(TypeId other) const { return ptr_ == other.ptr_; }
    bool operator!=(TypeId other) const { return ptr_ != other.ptr_; }
    bool operator<(TypeId other) const { return ptr_ < other.ptr_; }
    bool operator<=(TypeId other) const { return ptr_ <= other.ptr_; }
    bool operator>(TypeId other) const { return ptr_ > other.ptr_; }
    bool operator>=(TypeId other) const { return ptr_ >= other.ptr_; }

    explicit operator bool() const { return ptr_ != nullptr; }
    bool isNull() const { return ptr_ == nullptr; }

    const Type* raw() const { return ptr_; }

private:
    const Type* ptr_;
};

// Hash functor for TypeId so it can be used in unordered containers
struct TypeIdHash {
    size_t operator()(TypeId tid) const noexcept {
        return std::hash<const Type*>()(tid.raw());
    }
};

// ============================================================================
// Forward declarations of all Type subclasses
// ============================================================================
class Type;
class PrimitiveType;
class StructType;
class EnumType;
class PointerType;
class ReferenceType;
class MutReferenceType;
class SliceType;
class FnType;
class SmartPointerType;
class PoisonType;
class ErrorType;
class AllocatorType;

// ============================================================================
// Type base class
// ============================================================================
class Type {
public:
    virtual ~Type() = default;

    TypeKind getKind() const { return kind_; }

    // Produce a unique canonical string for this type (used for interning)
    virtual std::string toString() const = 0;

    // Return the size in bits of this type (0 if not applicable)
    virtual uint64_t bitWidth() const { return 0; }

    // Check if this is an integer type
    virtual bool isInteger() const { return false; }

    // Check if this is a floating-point type
    virtual bool isFloat() const { return false; }

    // Check if this is a numeric type (integer or float)
    virtual bool isNumeric() const { return false; }

    // Check if this is a signed integer type
    virtual bool isSigned() const { return false; }

    // Check if this is an unsigned integer type
    virtual bool isUnsigned() const { return false; }

    // Check if this is a boolean type
    virtual bool isBool() const { return false; }

    // Check if this is a void type
    virtual bool isVoid() const { return false; }

    // Check if this is a pointer-like type (pointer, reference, smart pointer)
    virtual bool isPointerLike() const { return false; }

    // Check if this is an error type
    virtual bool isError() const { return false; }

    // Check if this is a poison type (from erroneous expressions)
    virtual bool isPoison() const { return false; }

    // LLVM-style RTTI support
    static bool classof(const Type*) { return true; }

protected:
    explicit Type(TypeKind kind) : kind_(kind) {}

private:
    TypeKind kind_;
};

// ============================================================================
// PrimitiveType - represents u8, u16, ..., f32, f64, bool, void
// ============================================================================
class PrimitiveType : public Type {
public:
    static bool classof(const Type* t) {
        return t->getKind() == TypeKind::Primitive;
    }

    explicit PrimitiveType(PrimitiveKind kind)
        : Type(TypeKind::Primitive), kind_(kind) {}

    PrimitiveKind primitiveKind() const { return kind_; }

    std::string toString() const override {
        return kindToString(kind_);
    }

    uint64_t bitWidth() const override {
        switch (kind_) {
            case PrimitiveKind::U8:    return 8;
            case PrimitiveKind::U16:   return 16;
            case PrimitiveKind::U32:   return 32;
            case PrimitiveKind::U64:   return 64;
            case PrimitiveKind::USize: return 64;
            case PrimitiveKind::I8:    return 8;
            case PrimitiveKind::I16:   return 16;
            case PrimitiveKind::I32:   return 32;
            case PrimitiveKind::I64:   return 64;
            case PrimitiveKind::ISize: return 64;
            case PrimitiveKind::F32:   return 32;
            case PrimitiveKind::F64:   return 64;
            case PrimitiveKind::Bool:  return 8;
            case PrimitiveKind::Void:  return 0;
            default:                   return 0;
        }
    }

    bool isInteger() const override {
        return isIntegerKind(kind_);
    }

    bool isFloat() const override {
        return isFloatKind(kind_);
    }

    bool isNumeric() const override {
        return isInteger() || isFloat();
    }

    bool isSigned() const override {
        return isSignedKind(kind_);
    }

    bool isUnsigned() const override {
        return isUnsignedKind(kind_);
    }

    bool isBool() const override {
        return kind_ == PrimitiveKind::Bool;
    }

    bool isVoid() const override {
        return kind_ == PrimitiveKind::Void;
    }

    // Static helpers for PrimitiveKind classification
    static bool isIntegerKind(PrimitiveKind k) {
        return isUnsignedKind(k) || isSignedKind(k);
    }

    static bool isFloatKind(PrimitiveKind k) {
        return k == PrimitiveKind::F32 || k == PrimitiveKind::F64;
    }

    static bool isSignedKind(PrimitiveKind k) {
        return k == PrimitiveKind::I8 || k == PrimitiveKind::I16 ||
               k == PrimitiveKind::I32 || k == PrimitiveKind::I64 ||
               k == PrimitiveKind::ISize;
    }

    static bool isUnsignedKind(PrimitiveKind k) {
        return k == PrimitiveKind::U8 || k == PrimitiveKind::U16 ||
               k == PrimitiveKind::U32 || k == PrimitiveKind::U64 ||
               k == PrimitiveKind::USize;
    }

    static std::string kindToString(PrimitiveKind k) {
        switch (k) {
            case PrimitiveKind::U8:    return "u8";
            case PrimitiveKind::U16:   return "u16";
            case PrimitiveKind::U32:   return "u32";
            case PrimitiveKind::U64:   return "u64";
            case PrimitiveKind::USize: return "usize";
            case PrimitiveKind::I8:    return "i8";
            case PrimitiveKind::I16:   return "i16";
            case PrimitiveKind::I32:   return "i32";
            case PrimitiveKind::I64:   return "i64";
            case PrimitiveKind::ISize: return "isize";
            case PrimitiveKind::F32:   return "f32";
            case PrimitiveKind::F64:   return "f64";
            case PrimitiveKind::Bool:  return "bool";
            case PrimitiveKind::Void:  return "void";
            default:                   return "unknown";
        }
    }

private:
    PrimitiveKind kind_;
};

// ============================================================================
// StructField - a named field within a struct type
// ============================================================================
struct StructField {
    std::string name;
    TypeId type;
};

// ============================================================================
// StructType - represents a named struct with ordered fields
// ============================================================================
class StructType : public Type {
public:
    static bool classof(const Type* t) {
        return t->getKind() == TypeKind::Struct;
    }

    StructType(std::string name, std::vector<StructField> fields)
        : Type(TypeKind::Struct)
        , name_(std::move(name))
        , fields_(std::move(fields))
    {}

    const std::string& name() const { return name_; }
    const std::vector<StructField>& fields() const { return fields_; }
    size_t fieldCount() const { return fields_.size(); }

    // Look up a field by name. Returns nullptr if not found.
    const StructField* findField(const std::string& fname) const {
        for (const auto& f : fields_) {
            if (f.name == fname) return &f;
        }
        return nullptr;
    }

    // Get the index of a field by name. Returns -1 if not found.
    int fieldIndex(const std::string& fname) const {
        for (int i = 0; i < static_cast<int>(fields_.size()); ++i) {
            if (fields_[i].name == fname) return i;
        }
        return -1;
    }

    std::string toString() const override {
        std::string result = "struct:" + name_ + "{";
        for (size_t i = 0; i < fields_.size(); ++i) {
            if (i > 0) result += ",";
            result += fields_[i].name + ":" + fields_[i].type->toString();
        }
        result += "}";
        return result;
    }

    uint64_t bitWidth() const override {
        uint64_t total = 0;
        for (const auto& f : fields_) {
            total += f.type->bitWidth();
        }
        return total;
    }

private:
    std::string name_;
    std::vector<StructField> fields_;
};

// ============================================================================
// EnumVariant - a named variant in an enum, with optional explicit value
// ============================================================================
struct EnumVariant {
    std::string name;
    std::optional<int64_t> value;
};

// ============================================================================
// EnumType - represents a named enum with variants
// ============================================================================
class EnumType : public Type {
public:
    static bool classof(const Type* t) {
        return t->getKind() == TypeKind::Enum;
    }

    EnumType(std::string name, std::vector<EnumVariant> variants)
        : Type(TypeKind::Enum)
        , name_(std::move(name))
        , variants_(std::move(variants))
    {}

    const std::string& name() const { return name_; }
    const std::vector<EnumVariant>& variants() const { return variants_; }
    size_t variantCount() const { return variants_.size(); }

    // Look up a variant by name. Returns nullptr if not found.
    const EnumVariant* findVariant(const std::string& vname) const {
        for (const auto& v : variants_) {
            if (v.name == vname) return &v;
        }
        return nullptr;
    }

    // Get the index of a variant by name. Returns -1 if not found.
    int variantIndex(const std::string& vname) const {
        for (int i = 0; i < static_cast<int>(variants_.size()); ++i) {
            if (variants_[i].name == vname) return i;
        }
        return -1;
    }

    std::string toString() const override {
        std::string result = "enum:" + name_ + "{";
        for (size_t i = 0; i < variants_.size(); ++i) {
            if (i > 0) result += ",";
            result += variants_[i].name;
            if (variants_[i].value.has_value()) {
                result += "=" + std::to_string(variants_[i].value.value());
            }
        }
        result += "}";
        return result;
    }

    bool isInteger() const override { return true; }
    bool isNumeric() const override { return true; }
    bool isSigned() const override { return true; }

private:
    std::string name_;
    std::vector<EnumVariant> variants_;
};

// ============================================================================
// PointerType - raw pointer *T
// ============================================================================
class PointerType : public Type {
public:
    static bool classof(const Type* t) {
        return t->getKind() == TypeKind::Pointer;
    }

    explicit PointerType(TypeId pointee)
        : Type(TypeKind::Pointer)
        , pointee_(pointee)
    {}

    TypeId pointee() const { return pointee_; }

    std::string toString() const override {
        return "*" + pointee_->toString();
    }

    uint64_t bitWidth() const override { return 64; }
    bool isPointerLike() const override { return true; }

private:
    TypeId pointee_;
};

// ============================================================================
// ReferenceType - shared borrow &T
// ============================================================================
class ReferenceType : public Type {
public:
    static bool classof(const Type* t) {
        return t->getKind() == TypeKind::Reference;
    }

    explicit ReferenceType(TypeId referent)
        : Type(TypeKind::Reference)
        , referent_(referent)
    {}

    TypeId referent() const { return referent_; }

    std::string toString() const override {
        return "&" + referent_->toString();
    }

    uint64_t bitWidth() const override { return 64; }
    bool isPointerLike() const override { return true; }

private:
    TypeId referent_;
};

// ============================================================================
// MutReferenceType - exclusive borrow &mut T
// ============================================================================
class MutReferenceType : public Type {
public:
    static bool classof(const Type* t) {
        return t->getKind() == TypeKind::MutReference;
    }

    explicit MutReferenceType(TypeId referent)
        : Type(TypeKind::MutReference)
        , referent_(referent)
    {}

    TypeId referent() const { return referent_; }

    std::string toString() const override {
        return "&mut " + referent_->toString();
    }

    uint64_t bitWidth() const override { return 64; }
    bool isPointerLike() const override { return true; }

private:
    TypeId referent_;
};

// ============================================================================
// SliceType - slice []T
// ============================================================================
class SliceType : public Type {
public:
    static bool classof(const Type* t) {
        return t->getKind() == TypeKind::Slice;
    }

    explicit SliceType(TypeId element)
        : Type(TypeKind::Slice)
        , element_(element)
    {}

    TypeId element() const { return element_; }

    std::string toString() const override {
        return "[]" + element_->toString();
    }

    uint64_t bitWidth() const override { return 128; } // ptr + len
    bool isPointerLike() const override { return true; }

private:
    TypeId element_;
};

// ============================================================================
// FnParam - a parameter in a function type
// ============================================================================
struct FnParam {
    std::string name;
    TypeId type;
    bool is_mutable;
};

// ============================================================================
// FnType - function type with parameters, return type, purity, and error info
// ============================================================================
class FnType : public Type {
public:
    static bool classof(const Type* t) {
        return t->getKind() == TypeKind::Fn;
    }

    FnType(std::vector<FnParam> params, TypeId return_type,
           bool is_pure, TypeId error_type = TypeId())
        : Type(TypeKind::Fn)
        , params_(std::move(params))
        , return_type_(return_type)
        , is_pure_(is_pure)
        , error_type_(error_type)
    {}

    const std::vector<FnParam>& params() const { return params_; }
    TypeId returnType() const { return return_type_; }
    bool isPure() const { return is_pure_; }
    TypeId errorType() const { return error_type_; }
    bool canError() const { return !error_type_.isNull(); }

    size_t paramCount() const { return params_.size(); }

    std::string toString() const override {
        std::string result = "fn(";
        for (size_t i = 0; i < params_.size(); ++i) {
            if (i > 0) result += ",";
            if (params_[i].is_mutable) result += "var ";
            result += params_[i].name + ":" + params_[i].type->toString();
        }
        result += ")";
        if (is_pure_) result += " pure";
        result += " -> " + return_type_->toString();
        if (canError()) {
            result += " !" + error_type_->toString();
        }
        return result;
    }

private:
    std::vector<FnParam> params_;
    TypeId return_type_;
    bool is_pure_;
    TypeId error_type_;
};

// ============================================================================
// SmartPointerType - Box<T>, Rc<T>, Arc<T>
// ============================================================================
class SmartPointerType : public Type {
public:
    static bool classof(const Type* t) {
        return t->getKind() == TypeKind::SmartPointer;
    }

    SmartPointerType(TypeId pointee, SmartPointerKind kind)
        : Type(TypeKind::SmartPointer)
        , pointee_(pointee)
        , kind_(kind)
    {}

    TypeId pointee() const { return pointee_; }
    SmartPointerKind smartPointerKind() const { return kind_; }

    std::string kindToString() const {
        switch (kind_) {
            case SmartPointerKind::Box: return "Box";
            case SmartPointerKind::Rc:  return "Rc";
            case SmartPointerKind::Arc: return "Arc";
            default:                    return "Unknown";
        }
    }

    std::string toString() const override {
        return kindToString() + "<" + pointee_->toString() + ">";
    }

    uint64_t bitWidth() const override { return 64; }
    bool isPointerLike() const override { return true; }

private:
    TypeId pointee_;
    SmartPointerKind kind_;
};

// ============================================================================
// PoisonType - represents an erroneous/unresolvable type
//
// When the parser or type-checker encounters an invalid piece of code, it
// replaces the type with PoisonType instead of aborting. This allows the
// compiler to continue analyzing the rest of the file and collect all errors
// in a single pass (error-resilient compilation).
// ============================================================================
class PoisonType : public Type {
public:
    static bool classof(const Type* t) {
        return t->getKind() == TypeKind::Poison;
    }

    PoisonType() : Type(TypeKind::Poison) {}

    std::string toString() const override {
        return "<poison>";
    }

    bool isError() const override { return true; }
    bool isPoison() const override { return true; }
};

// ============================================================================
// ErrorType - represents T! or !T (error union type)
// ============================================================================
class ErrorType : public Type {
public:
    static bool classof(const Type* t) {
        return t->getKind() == TypeKind::Error;
    }

    explicit ErrorType(TypeId success_type)
        : Type(TypeKind::Error)
        , success_type_(success_type)
    {}

    TypeId successType() const { return success_type_; }

    std::string toString() const override {
        return "!" + success_type_->toString();
    }

    bool isError() const override { return true; }

private:
    TypeId success_type_;
};

// ============================================================================
// AllocatorType - type for explicit allocator passing
// ============================================================================
class AllocatorType : public Type {
public:
    static bool classof(const Type* t) {
        return t->getKind() == TypeKind::Allocator;
    }

    AllocatorType() : Type(TypeKind::Allocator) {}

    std::string toString() const override {
        return "Allocator";
    }

    uint64_t bitWidth() const override { return 64; }
};

// ============================================================================
// Type casting helpers (LLVM-style RTTI)
// ============================================================================
template<typename T>
bool isa(const Type& type) {
    return T::classof(&type);
}

template<typename T>
const T& cast(const Type& type) {
    assert(T::classof(&type) && "Invalid type cast");
    return static_cast<const T&>(type);
}

template<typename T>
const T* dyn_cast(const Type* type) {
    if (type && T::classof(type)) {
        return static_cast<const T*>(type);
    }
    return nullptr;
}

template<typename T>
T* dyn_cast(Type* type) {
    if (type && T::classof(type)) {
        return static_cast<T*>(type);
    }
    return nullptr;
}

// Overloads that work with TypeId
template<typename T>
bool isa(TypeId tid) {
    return tid && T::classof(tid.raw());
}

template<typename T>
const T& cast(TypeId tid) {
    assert(tid && "Null TypeId in cast");
    assert(T::classof(tid.raw()) && "Invalid TypeId cast");
    return static_cast<const T&>(*tid.raw());
}

template<typename T>
const T* dyn_cast(TypeId tid) {
    if (tid && T::classof(tid.raw())) {
        return static_cast<const T*>(tid.raw());
    }
    return nullptr;
}

// ============================================================================
// TypeTable - interns types using the flyweight pattern
// ============================================================================
class TypeTable {
public:
    TypeTable() {
        // Pre-intern all primitive types
        for (uint8_t i = 0; i < static_cast<uint8_t>(PrimitiveKind::Count); ++i) {
            auto kind = static_cast<PrimitiveKind>(i);
            auto type = std::make_unique<PrimitiveType>(kind);
            std::string key = type->toString();
            primitive_cache_[i] = type.get();
            type_map_[std::move(key)] = std::move(type);
        }

        // Pre-intern the poison type
        {
            auto type = std::make_unique<PoisonType>();
            poison_type_ = type.get();
            type_map_[type->toString()] = std::move(type);
        }

        // Pre-intern the allocator type
        {
            auto type = std::make_unique<AllocatorType>();
            allocator_type_ = type.get();
            type_map_[type->toString()] = std::move(type);
        }
    }

    ~TypeTable() = default;

    // Prevent copying - the TypeTable owns all types
    TypeTable(const TypeTable&) = delete;
    TypeTable& operator=(const TypeTable&) = delete;

    // Move is allowed
    TypeTable(TypeTable&&) = default;
    TypeTable& operator=(TypeTable&&) = default;

    // -----------------------------------------------------------------------
    // Primitive type accessors (pre-interned)
    // -----------------------------------------------------------------------
    TypeId getU8()    const { return TypeId(primitive_cache_[static_cast<uint8_t>(PrimitiveKind::U8)]); }
    TypeId getU16()   const { return TypeId(primitive_cache_[static_cast<uint8_t>(PrimitiveKind::U16)]); }
    TypeId getU32()   const { return TypeId(primitive_cache_[static_cast<uint8_t>(PrimitiveKind::U32)]); }
    TypeId getU64()   const { return TypeId(primitive_cache_[static_cast<uint8_t>(PrimitiveKind::U64)]); }
    TypeId getUSize() const { return TypeId(primitive_cache_[static_cast<uint8_t>(PrimitiveKind::USize)]); }
    TypeId getI8()    const { return TypeId(primitive_cache_[static_cast<uint8_t>(PrimitiveKind::I8)]); }
    TypeId getI16()   const { return TypeId(primitive_cache_[static_cast<uint8_t>(PrimitiveKind::I16)]); }
    TypeId getI32()   const { return TypeId(primitive_cache_[static_cast<uint8_t>(PrimitiveKind::I32)]); }
    TypeId getI64()   const { return TypeId(primitive_cache_[static_cast<uint8_t>(PrimitiveKind::I64)]); }
    TypeId getISize() const { return TypeId(primitive_cache_[static_cast<uint8_t>(PrimitiveKind::ISize)]); }
    TypeId getF32()   const { return TypeId(primitive_cache_[static_cast<uint8_t>(PrimitiveKind::F32)]); }
    TypeId getF64()   const { return TypeId(primitive_cache_[static_cast<uint8_t>(PrimitiveKind::F64)]); }
    TypeId getBool()  const { return TypeId(primitive_cache_[static_cast<uint8_t>(PrimitiveKind::Bool)]); }
    TypeId getVoid()  const { return TypeId(primitive_cache_[static_cast<uint8_t>(PrimitiveKind::Void)]); }

    TypeId getPrimitive(PrimitiveKind kind) const {
        return TypeId(primitive_cache_[static_cast<uint8_t>(kind)]);
    }

    // -----------------------------------------------------------------------
    // Poison type accessor
    // -----------------------------------------------------------------------
    TypeId getPoison() const { return TypeId(poison_type_); }

    // -----------------------------------------------------------------------
    // Allocator type accessor
    // -----------------------------------------------------------------------
    TypeId getAllocator() const { return TypeId(allocator_type_); }

    // -----------------------------------------------------------------------
    // Intern a struct type
    // -----------------------------------------------------------------------
    TypeId getStruct(std::string name, std::vector<StructField> fields) {
        // Compute the key by building a temporary StructType
        StructType temp{name, fields};
        std::string key = temp.toString();

        auto it = type_map_.find(key);
        if (it != type_map_.end()) {
            return TypeId(it->second.get());
        }

        auto type = std::make_unique<StructType>(std::move(name), std::move(fields));
        Type* raw = type.get();
        type_map_[std::move(key)] = std::move(type);
        return TypeId(raw);
    }

    // -----------------------------------------------------------------------
    // Intern an enum type
    // -----------------------------------------------------------------------
    TypeId getEnum(std::string name, std::vector<EnumVariant> variants) {
        EnumType temp{name, variants};
        std::string key = temp.toString();

        auto it = type_map_.find(key);
        if (it != type_map_.end()) {
            return TypeId(it->second.get());
        }

        auto type = std::make_unique<EnumType>(std::move(name), std::move(variants));
        Type* raw = type.get();
        type_map_[std::move(key)] = std::move(type);
        return TypeId(raw);
    }

    // -----------------------------------------------------------------------
    // Intern a pointer type
    // -----------------------------------------------------------------------
    TypeId getPointer(TypeId pointee) {
        std::string key = "*" + pointee->toString();

        auto it = type_map_.find(key);
        if (it != type_map_.end()) {
            return TypeId(it->second.get());
        }

        auto type = std::make_unique<PointerType>(pointee);
        Type* raw = type.get();
        type_map_[std::move(key)] = std::move(type);
        return TypeId(raw);
    }

    // -----------------------------------------------------------------------
    // Intern a shared reference type (&T)
    // -----------------------------------------------------------------------
    TypeId getReference(TypeId referent) {
        std::string key = "&" + referent->toString();

        auto it = type_map_.find(key);
        if (it != type_map_.end()) {
            return TypeId(it->second.get());
        }

        auto type = std::make_unique<ReferenceType>(referent);
        Type* raw = type.get();
        type_map_[std::move(key)] = std::move(type);
        return TypeId(raw);
    }

    // -----------------------------------------------------------------------
    // Intern a mutable reference type (&mut T)
    // -----------------------------------------------------------------------
    TypeId getMutReference(TypeId referent) {
        std::string key = "&mut " + referent->toString();

        auto it = type_map_.find(key);
        if (it != type_map_.end()) {
            return TypeId(it->second.get());
        }

        auto type = std::make_unique<MutReferenceType>(referent);
        Type* raw = type.get();
        type_map_[std::move(key)] = std::move(type);
        return TypeId(raw);
    }

    // -----------------------------------------------------------------------
    // Intern a slice type ([]T)
    // -----------------------------------------------------------------------
    TypeId getSlice(TypeId element) {
        std::string key = "[]" + element->toString();

        auto it = type_map_.find(key);
        if (it != type_map_.end()) {
            return TypeId(it->second.get());
        }

        auto type = std::make_unique<SliceType>(element);
        Type* raw = type.get();
        type_map_[std::move(key)] = std::move(type);
        return TypeId(raw);
    }

    // -----------------------------------------------------------------------
    // Intern a function type
    // -----------------------------------------------------------------------
    TypeId getFn(std::vector<FnParam> params, TypeId return_type,
                 bool is_pure, TypeId error_type = TypeId()) {
        FnType temp{params, return_type, is_pure, error_type};
        std::string key = temp.toString();

        auto it = type_map_.find(key);
        if (it != type_map_.end()) {
            return TypeId(it->second.get());
        }

        auto type = std::make_unique<FnType>(std::move(params), return_type,
                                              is_pure, error_type);
        Type* raw = type.get();
        type_map_[std::move(key)] = std::move(type);
        return TypeId(raw);
    }

    // -----------------------------------------------------------------------
    // Intern a smart pointer type
    // -----------------------------------------------------------------------
    TypeId getSmartPointer(TypeId pointee, SmartPointerKind kind) {
        SmartPointerType temp(pointee, kind);
        std::string key = temp.toString();

        auto it = type_map_.find(key);
        if (it != type_map_.end()) {
            return TypeId(it->second.get());
        }

        auto type = std::make_unique<SmartPointerType>(pointee, kind);
        Type* raw = type.get();
        type_map_[std::move(key)] = std::move(type);
        return TypeId(raw);
    }

    // -----------------------------------------------------------------------
    // Intern an error type
    // -----------------------------------------------------------------------
    TypeId getError(TypeId success_type) {
        std::string key = "!" + success_type->toString();

        auto it = type_map_.find(key);
        if (it != type_map_.end()) {
            return TypeId(it->second.get());
        }

        auto type = std::make_unique<ErrorType>(success_type);
        Type* raw = type.get();
        type_map_[std::move(key)] = std::move(type);
        return TypeId(raw);
    }

    // -----------------------------------------------------------------------
    // Lookup a type by its canonical string representation
    // -----------------------------------------------------------------------
    std::optional<TypeId> lookup(const std::string& key) const {
        auto it = type_map_.find(key);
        if (it != type_map_.end()) {
            return TypeId(it->second.get());
        }
        return std::nullopt;
    }

    // -----------------------------------------------------------------------
    // Total number of unique interned types
    // -----------------------------------------------------------------------
    size_t size() const { return type_map_.size(); }

    // -----------------------------------------------------------------------
    // Register a short name alias for a type (e.g., "Vec2" -> struct:Vec2{...})
    // -----------------------------------------------------------------------
    void registerAlias(const std::string& alias, TypeId type) {
        name_aliases_[alias] = type;
    }

    // -----------------------------------------------------------------------
    // Lookup a type by its short name alias
    // -----------------------------------------------------------------------
    std::optional<TypeId> lookupAlias(const std::string& alias) const {
        auto it = name_aliases_.find(alias);
        if (it != name_aliases_.end()) return it->second;
        return std::nullopt;
    }

private:
    std::unordered_map<std::string, std::unique_ptr<Type>> type_map_;
    std::unordered_map<std::string, TypeId> name_aliases_;
    const Type* primitive_cache_[static_cast<uint8_t>(PrimitiveKind::Count)] = {};
    const Type* poison_type_ = nullptr;
    const Type* allocator_type_ = nullptr;
};

} // namespace jules
