#include "json.hpp"

#include <cstdio>

namespace lt::json {

namespace {
const std::string kEmpty;

struct Parser {
  std::string_view in;
  size_t i = 0;
  int max_depth;

  void ws() {
    while (i < in.size()) {
      char c = in[i];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i;
      else break;
    }
  }

  bool lit(std::string_view w) {
    if (in.substr(i, w.size()) != w) return false;
    i += w.size();
    return true;
  }

  static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }

  bool hex4(uint32_t& out) {
    if (i + 4 > in.size()) return false;
    uint32_t v = 0;
    for (int k = 0; k < 4; ++k) {
      int h = hexv(in[i + k]);
      if (h < 0) return false;
      v = (v << 4) | static_cast<uint32_t>(h);
    }
    i += 4;
    out = v;
    return true;
  }

  static void utf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) {
      s.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool string(std::string& s) {
    if (i >= in.size() || in[i] != '"') return false;
    ++i;
    while (i < in.size()) {
      char c = in[i++];
      if (c == '"') return true;
      if (static_cast<unsigned char>(c) < 0x20) return false;
      if (c != '\\') {
        s.push_back(c);
        continue;
      }
      if (i >= in.size()) return false;
      char e = in[i++];
      switch (e) {
        case '"': s.push_back('"'); break;
        case '\\': s.push_back('\\'); break;
        case '/': s.push_back('/'); break;
        case 'b': s.push_back('\b'); break;
        case 'f': s.push_back('\f'); break;
        case 'n': s.push_back('\n'); break;
        case 'r': s.push_back('\r'); break;
        case 't': s.push_back('\t'); break;
        case 'u': {
          uint32_t cp;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            uint32_t lo;
            if (i + 2 <= in.size() && in[i] == '\\' && in[i + 1] == 'u') {
              i += 2;
              if (!hex4(lo)) return false;
              if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              } else {
                utf8(s, 0xFFFD);
                cp = lo;
              }
            } else {
              cp = 0xFFFD;
            }
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;
          }
          utf8(s, cp);
          break;
        }
        default: return false;
      }
    }
    return false;
  }

  bool number(Value& out) {
    size_t start = i;
    bool neg = false;
    if (i < in.size() && in[i] == '-') { neg = true; ++i; }
    if (i >= in.size()) return false;
    if (in[i] == '0') {
      ++i;
    } else if (in[i] >= '1' && in[i] <= '9') {
      while (i < in.size() && in[i] >= '0' && in[i] <= '9') ++i;
    } else {
      return false;
    }
    size_t int_end = i;
    bool is_double = false;
    if (i < in.size() && in[i] == '.') {
      is_double = true;
      ++i;
      if (i >= in.size() || in[i] < '0' || in[i] > '9') return false;
      while (i < in.size() && in[i] >= '0' && in[i] <= '9') ++i;
    }
    if (i < in.size() && (in[i] == 'e' || in[i] == 'E')) {
      is_double = true;
      ++i;
      if (i < in.size() && (in[i] == '+' || in[i] == '-')) ++i;
      if (i >= in.size() || in[i] < '0' || in[i] > '9') return false;
      while (i < in.size() && in[i] >= '0' && in[i] <= '9') ++i;
    }
    size_t digits_start = start + (neg ? 1 : 0);
    if (!is_double) {
      // Exact int64 when it fits, otherwise fall through to double.
      uint64_t v = 0;
      bool overflow = false;
      for (size_t k = digits_start; k < int_end; ++k) {
        uint64_t d = static_cast<uint64_t>(in[k] - '0');
        if (v > (UINT64_MAX - d) / 10) { overflow = true; break; }
        v = v * 10 + d;
      }
      uint64_t lim = neg ? (static_cast<uint64_t>(INT64_MAX) + 1) : static_cast<uint64_t>(INT64_MAX);
      if (!overflow && v <= lim) {
        out = neg ? Value(static_cast<int64_t>(0 - v)) : Value(static_cast<int64_t>(v));
        return true;
      }
    }
    // Approximate, locale-independent double conversion.
    double mant = 0;
    int exp10 = 0;
    size_t k = digits_start;
    for (; k < int_end; ++k) mant = mant * 10 + (in[k] - '0');
    if (k < in.size() && in[k] == '.') {
      ++k;
      while (k < in.size() && in[k] >= '0' && in[k] <= '9') {
        mant = mant * 10 + (in[k] - '0');
        --exp10;
        ++k;
      }
    }
    if (k < in.size() && (in[k] == 'e' || in[k] == 'E')) {
      ++k;
      bool eneg = false;
      if (in[k] == '+' || in[k] == '-') { eneg = in[k] == '-'; ++k; }
      int e = 0;
      while (k < in.size() && in[k] >= '0' && in[k] <= '9') {
        if (e < 100000) e = e * 10 + (in[k] - '0');
        ++k;
      }
      exp10 += eneg ? -e : e;
    }
    double scale = 1;
    int ae = exp10 < 0 ? -exp10 : exp10;
    if (ae > 400) ae = 400;
    for (int n = 0; n < ae; ++n) scale *= 10;
    double d = exp10 < 0 ? mant / scale : mant * scale;
    out = Value(neg ? -d : d);
    return true;
  }

  bool value(Value& out, int depth) {
    if (depth > max_depth) return false;
    ws();
    if (i >= in.size()) return false;
    char c = in[i];
    if (c == '{') {
      ++i;
      out = Value::object();
      ws();
      if (i < in.size() && in[i] == '}') { ++i; return true; }
      while (true) {
        ws();
        std::string key;
        if (!string(key)) return false;
        ws();
        if (i >= in.size() || in[i] != ':') return false;
        ++i;
        Value v;
        if (!value(v, depth + 1)) return false;
        out.members().emplace_back(std::move(key), std::move(v));
        ws();
        if (i >= in.size()) return false;
        if (in[i] == ',') { ++i; continue; }
        if (in[i] == '}') { ++i; return true; }
        return false;
      }
    }
    if (c == '[') {
      ++i;
      out = Value::array();
      ws();
      if (i < in.size() && in[i] == ']') { ++i; return true; }
      while (true) {
        Value v;
        if (!value(v, depth + 1)) return false;
        out.items().push_back(std::move(v));
        ws();
        if (i >= in.size()) return false;
        if (in[i] == ',') { ++i; continue; }
        if (in[i] == ']') { ++i; return true; }
        return false;
      }
    }
    if (c == '"') {
      std::string s;
      if (!string(s)) return false;
      out = Value(std::move(s));
      return true;
    }
    if (c == 't') { if (!lit("true")) return false; out = Value(true); return true; }
    if (c == 'f') { if (!lit("false")) return false; out = Value(false); return true; }
    if (c == 'n') { if (!lit("null")) return false; out = Value(); return true; }
    return number(out);
  }
};

void dump_int(std::string& out, int64_t v) {
  char buf[24];
  int n = 0;
  uint64_t u = v < 0 ? (0 - static_cast<uint64_t>(v)) : static_cast<uint64_t>(v);
  do {
    buf[n++] = static_cast<char>('0' + (u % 10));
    u /= 10;
  } while (u);
  if (v < 0) out.push_back('-');
  while (n) out.push_back(buf[--n]);
}
}  // namespace

const std::string& Value::as_str() const { return t_ == Type::String ? s_ : kEmpty; }

const Value* Value::get(std::string_view key) const {
  if (t_ != Type::Object) return nullptr;
  for (size_t k = o_.size(); k-- > 0;) {
    if (o_[k].first == key) return &o_[k].second;
  }
  return nullptr;
}

Value* Value::get(std::string_view key) {
  return const_cast<Value*>(static_cast<const Value*>(this)->get(key));
}

Value& Value::set(std::string_view key, Value v) {
  if (t_ != Type::Object) *this = object();
  if (Value* existing = get(key)) {
    *existing = std::move(v);
    return *existing;
  }
  o_.emplace_back(std::string(key), std::move(v));
  return o_.back().second;
}

bool Value::erase(std::string_view key) {
  if (t_ != Type::Object) return false;
  bool any = false;
  for (size_t k = o_.size(); k-- > 0;) {
    if (o_[k].first == key) {
      o_.erase(o_.begin() + static_cast<std::ptrdiff_t>(k));
      any = true;
    }
  }
  return any;
}

std::string Value::get_str(std::string_view key, std::string_view def) const {
  const Value* v = get(key);
  return v && v->is_string() ? v->s_ : std::string(def);
}

int64_t Value::get_int(std::string_view key, int64_t def) const {
  const Value* v = get(key);
  return v && v->is_number() ? v->as_int() : def;
}

bool Value::get_bool(std::string_view key, bool def) const {
  const Value* v = get(key);
  return v && v->is_bool() ? v->b_ : def;
}

bool parse(std::string_view in, Value& out, int max_depth) {
  Parser p{in, 0, max_depth};
  Value v;
  if (!p.value(v, 0)) return false;
  p.ws();
  if (p.i != in.size()) return false;
  out = std::move(v);
  return true;
}

void append_quoted(std::string& out, std::string_view s) {
  static const char* hex = "0123456789abcdef";
  out.push_back('"');
  for (char c : s) {
    unsigned char u = static_cast<unsigned char>(c);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (u < 0x20) {
          out += "\\u00";
          out.push_back(hex[u >> 4]);
          out.push_back(hex[u & 15]);
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
}

void dump_to(const Value& v, std::string& out) {
  switch (v.type()) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += v.as_bool() ? "true" : "false"; break;
    case Type::Int: dump_int(out, v.as_int()); break;
    case Type::Double: {
      double d = v.as_double();
      if (d != d || d > 1e300 || d < -1e300) {
        out += "null";
      } else if (d == static_cast<double>(static_cast<int64_t>(d)) && d < 9e15 && d > -9e15) {
        dump_int(out, static_cast<int64_t>(d));
      } else {
        char buf[40];
        int n = std::snprintf(buf, sizeof buf, "%.17g", d);
        for (int k = 0; k < n; ++k) out.push_back(buf[k] == ',' ? '.' : buf[k]);
      }
      break;
    }
    case Type::String: append_quoted(out, v.as_str()); break;
    case Type::Array: {
      out.push_back('[');
      bool first = true;
      for (const auto& e : v.items()) {
        if (!first) out.push_back(',');
        first = false;
        dump_to(e, out);
      }
      out.push_back(']');
      break;
    }
    case Type::Object: {
      out.push_back('{');
      bool first = true;
      for (const auto& m : v.members()) {
        if (!first) out.push_back(',');
        first = false;
        append_quoted(out, m.first);
        out.push_back(':');
        dump_to(m.second, out);
      }
      out.push_back('}');
      break;
    }
  }
}

std::string dump(const Value& v) {
  std::string s;
  dump_to(v, s);
  return s;
}

}  // namespace lt::json
