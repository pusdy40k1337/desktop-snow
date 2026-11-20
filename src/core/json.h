// Minimal JSON value + parser + writer (UTF-8). No external dependencies.
#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace snow::json {

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
 public:
  Value() = default;
  Value(std::nullptr_t) {}
  Value(bool b) : t_(Type::Bool), b_(b) {}
  Value(double n) : t_(Type::Number), n_(n) {}
  Value(float n) : t_(Type::Number), n_(n) {}
  Value(int n) : t_(Type::Number), n_(n) {}
  Value(unsigned n) : t_(Type::Number), n_(n) {}
  Value(long long n) : t_(Type::Number), n_(static_cast<double>(n)) {}
  Value(unsigned long long n) : t_(Type::Number), n_(static_cast<double>(n)) {}
  Value(const char* s) : t_(Type::String), s_(s) {}
  Value(std::string s) : t_(Type::String), s_(std::move(s)) {}
  static Value array() { Value v; v.t_ = Type::Array; return v; }
  static Value object() { Value v; v.t_ = Type::Object; return v; }

  Type type() const { return t_; }
  bool isNull() const { return t_ == Type::Null; }
  bool isBool() const { return t_ == Type::Bool; }
  bool isNumber() const { return t_ == Type::Number; }
  bool isString() const { return t_ == Type::String; }
  bool isArray() const { return t_ == Type::Array; }
  bool isObject() const { return t_ == Type::Object; }

  bool asBool(bool def = false) const { return t_ == Type::Bool ? b_ : def; }
  double asNumber(double def = 0) const { return t_ == Type::Number ? n_ : def; }
  int asInt(int def = 0) const;
  std::string asString(const std::string& def = std::string()) const { return t_ == Type::String ? s_ : def; }

  // Object access. Missing keys / wrong types yield a shared null value.
  const Value& operator[](std::string_view key) const;
  bool has(std::string_view key) const;
  Value& set(std::string_view key, Value v);
  const std::vector<std::string>& keys() const { return keys_; }

  // Array access.
  size_t size() const { return t_ == Type::Array ? vals_.size() : (t_ == Type::Object ? keys_.size() : 0); }
  const Value& at(size_t i) const;
  Value& push(Value v);
  const std::vector<Value>& elements() const { return vals_; }

 private:
  friend class Parser;
  friend void dumpTo(const Value& v, std::string& out, int indent, int depth);
  Type t_ = Type::Null;
  bool b_ = false;
  double n_ = 0;
  std::string s_;
  std::vector<std::string> keys_;  // object keys (ordered)
  std::vector<Value> vals_;        // object values or array elements
};

// Returns false on malformed input; `error` gets a message with the byte offset.
bool parse(std::string_view text, Value& out, std::string* error = nullptr);
std::string dump(const Value& v, int indent = 2);

}  // namespace snow::json
