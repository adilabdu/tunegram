// Minimal JSON value, parser and serializer for the Listen Together core.
// No exceptions, no locale dependence, no third-party code. Integers are kept
// exactly as int64; non-integral numbers are parsed approximately as double
// (the protocol never uses them).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lt::json {

enum class Type : uint8_t { Null, Bool, Int, Double, String, Array, Object };

class Value {
 public:
  using Member = std::pair<std::string, Value>;

  Value() = default;
  Value(std::nullptr_t) {}
  Value(bool b) : t_(Type::Bool), b_(b) {}
  Value(int v) : t_(Type::Int), i_(v) {}
  Value(int64_t v) : t_(Type::Int), i_(v) {}
  Value(uint32_t v) : t_(Type::Int), i_(static_cast<int64_t>(v)) {}
  Value(double d) : t_(Type::Double), d_(d) {}
  Value(const char* s) : t_(Type::String), s_(s ? s : "") {}
  Value(std::string s) : t_(Type::String), s_(std::move(s)) {}
  Value(std::string_view s) : t_(Type::String), s_(s) {}

  static Value array() { Value v; v.t_ = Type::Array; return v; }
  static Value object() { Value v; v.t_ = Type::Object; return v; }

  Type type() const { return t_; }
  bool is_null() const { return t_ == Type::Null; }
  bool is_bool() const { return t_ == Type::Bool; }
  bool is_int() const { return t_ == Type::Int; }
  bool is_number() const { return t_ == Type::Int || t_ == Type::Double; }
  bool is_string() const { return t_ == Type::String; }
  bool is_array() const { return t_ == Type::Array; }
  bool is_object() const { return t_ == Type::Object; }

  bool as_bool(bool def = false) const { return t_ == Type::Bool ? b_ : def; }
  int64_t as_int(int64_t def = 0) const {
    if (t_ == Type::Int) return i_;
    if (t_ == Type::Double) return static_cast<int64_t>(d_);
    return def;
  }
  double as_double(double def = 0) const {
    if (t_ == Type::Double) return d_;
    if (t_ == Type::Int) return static_cast<double>(i_);
    return def;
  }
  const std::string& as_str() const;  // empty string if not a string
  std::string& str_ref() { return s_; }

  // Arrays
  const std::vector<Value>& items() const { return a_; }
  std::vector<Value>& items() { return a_; }
  void push(Value v) {
    if (t_ != Type::Array) { *this = array(); }
    a_.push_back(std::move(v));
  }
  size_t size() const { return t_ == Type::Array ? a_.size() : t_ == Type::Object ? o_.size() : 0; }

  // Objects (insertion ordered; duplicate keys: last one wins on lookup)
  const std::vector<Member>& members() const { return o_; }
  std::vector<Member>& members() { return o_; }
  const Value* get(std::string_view key) const;
  Value* get(std::string_view key);
  bool has(std::string_view key) const { return get(key) != nullptr; }
  Value& set(std::string_view key, Value v);
  bool erase(std::string_view key);

  // Convenience typed lookups for objects.
  std::string get_str(std::string_view key, std::string_view def = {}) const;
  int64_t get_int(std::string_view key, int64_t def = 0) const;
  bool get_bool(std::string_view key, bool def = false) const;

 private:
  Type t_ = Type::Null;
  bool b_ = false;
  int64_t i_ = 0;
  double d_ = 0;
  std::string s_;
  std::vector<Value> a_;
  std::vector<Member> o_;
};

// Parses exactly one JSON value (surrounding whitespace allowed). Returns false
// on any syntax error, depth > max_depth or trailing garbage.
bool parse(std::string_view in, Value& out, int max_depth = 32);

void dump_to(const Value& v, std::string& out);
std::string dump(const Value& v);
void append_quoted(std::string& out, std::string_view s);

}  // namespace lt::json
