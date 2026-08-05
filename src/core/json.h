// Minimal JSON value + parser.
//
// Deliberately self-contained: the core engine must build with no third-party
// dependency so the conformance runner is trivial to run anywhere. The value
// type mirrors tvOS `PhonoscopeJSONValue` accessor-for-accessor, because the
// simulation port below relies on the same "missing/whong type yields nullopt"
// behaviour that the Swift optional chaining gives.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nova::json {

class Value;
using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
 public:
  Value() = default;
  explicit Value(bool v) : type_(Type::Bool), number_(v ? 1 : 0) {}
  explicit Value(double v) : type_(Type::Number), number_(v) {}
  explicit Value(std::string v) : type_(Type::String), string_(std::move(v)) {}
  explicit Value(Array v) : type_(Type::Array), array_(std::make_shared<Array>(std::move(v))) {}
  explicit Value(Object v) : type_(Type::Object), object_(std::make_shared<Object>(std::move(v))) {}

  Type type() const { return type_; }
  bool isNull() const { return type_ == Type::Null; }

  std::optional<double> number() const {
    if (type_ == Type::Number) return number_;
    return std::nullopt;
  }
  std::optional<bool> boolean() const {
    if (type_ == Type::Bool) return number_ != 0;
    return std::nullopt;
  }
  std::optional<std::string> string() const {
    if (type_ == Type::String) return string_;
    return std::nullopt;
  }
  const Array* array() const { return type_ == Type::Array ? array_.get() : nullptr; }
  const Object* object() const { return type_ == Type::Object ? object_.get() : nullptr; }

  // Object member access. Returns nullptr when absent or when this is not an
  // object -- the direct analogue of Swift's `value["key"]` on an enum.
  const Value* find(std::string_view key) const {
    if (type_ != Type::Object) return nullptr;
    auto it = object_->find(std::string(key));
    return it == object_->end() ? nullptr : &it->second;
  }

  // Convenience readers used throughout the module decode. Each falls back
  // exactly the way the Swift code does.
  double numberOr(double fallback) const { return number().value_or(fallback); }
  std::string stringOr(std::string_view fallback) const {
    auto v = string();
    return v ? *v : std::string(fallback);
  }
  std::vector<double> numberArray() const;
  std::vector<std::string> stringArray() const;

  // `$expr` source text of an expression object, or "" when this is not one.
  std::string exprSource() const {
    if (const Value* e = find("$expr")) return e->stringOr("");
    return {};
  }

  std::string dump() const;

  static std::optional<Value> parse(std::string_view text);

 private:
  Type type_ = Type::Null;
  double number_ = 0;
  std::string string_;
  std::shared_ptr<Array> array_;
  std::shared_ptr<Object> object_;
};

// Convenience: member of a possibly-null value.
inline const Value* member(const Value* value, std::string_view key) {
  return value ? value->find(key) : nullptr;
}

std::string escape(std::string_view text);

}  // namespace nova::json
