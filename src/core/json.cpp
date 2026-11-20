#include "json.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace snow::json {

static const Value& NullValue() {
  static const Value v;
  return v;
}

int Value::asInt(int def) const {
  if (t_ != Type::Number || !std::isfinite(n_)) return def;
  if (n_ > 2147483647.0) return 2147483647;
  if (n_ < -2147483648.0) return -2147483647 - 1;
  return static_cast<int>(std::lround(n_));
}

const Value& Value::operator[](std::string_view key) const {
  if (t_ != Type::Object) return NullValue();
  for (size_t i = 0; i < keys_.size(); ++i)
    if (keys_[i] == key) return vals_[i];
  return NullValue();
}

bool Value::has(std::string_view key) const {
  if (t_ != Type::Object) return false;
  for (const auto& k : keys_)
    if (k == key) return true;
  return false;
}

Value& Value::set(std::string_view key, Value v) {
  if (t_ != Type::Object) {
    *this = object();
  }
  for (size_t i = 0; i < keys_.size(); ++i) {
    if (keys_[i] == key) {
      vals_[i] = std::move(v);
      return vals_[i];
    }
  }
  keys_.emplace_back(key);
  vals_.push_back(std::move(v));
  return vals_.back();
}

const Value& Value::at(size_t i) const {
  if (t_ != Type::Array || i >= vals_.size()) return NullValue();
  return vals_[i];
}

Value& Value::push(Value v) {
  if (t_ != Type::Array) *this = array();
  vals_.push_back(std::move(v));
  return vals_.back();
}

class Parser {
 public:
  Parser(std::string_view s) : s_(s) {}
  bool run(Value& out, std::string* err) {
    skipWs();
    if (!parseValue(out, 0)) return fail(err);
    skipWs();
    if (pos_ != s_.size()) {
      msg_ = "trailing characters";
      return fail(err);
    }
    return true;
  }

 private:
  bool fail(std::string* err) {
    if (err) *err = msg_ + " at offset " + std::to_string(pos_);
    return false;
  }
  void skipWs() {
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
  }
  bool literal(const char* lit) {
    size_t n = std::strlen(lit);
    if (s_.substr(pos_, n) != lit) {
      msg_ = "invalid literal";
      return false;
    }
    pos_ += n;
    return true;
  }
  bool parseValue(Value& v, int depth) {
    if (depth > 64) {
      msg_ = "nesting too deep";
      return false;
    }
    if (pos_ >= s_.size()) {
      msg_ = "unexpected end";
      return false;
    }
    char c = s_[pos_];
    if (c == '{') return parseObject(v, depth);
    if (c == '[') return parseArray(v, depth);
    if (c == '"') {
      std::string str;
      if (!parseString(str)) return false;
      v = Value(std::move(str));
      return true;
    }
    if (c == 't') {
      if (!literal("true")) return false;
      v = Value(true);
      return true;
    }
    if (c == 'f') {
      if (!literal("false")) return false;
      v = Value(false);
      return true;
    }
    if (c == 'n') {
      if (!literal("null")) return false;
      v = Value();
      return true;
    }
    return parseNumber(v);
  }
  bool parseNumber(Value& v) {
    size_t start = pos_;
    if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
    auto digits = [&]() {
      size_t b = pos_;
      while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
      return pos_ > b;
    };
    if (!digits()) {
      msg_ = "invalid number";
      return false;
    }
    if (pos_ < s_.size() && s_[pos_] == '.') {
      ++pos_;
      if (!digits()) {
        msg_ = "invalid number";
        return false;
      }
    }
    if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
      if (!digits()) {
        msg_ = "invalid number";
        return false;
      }
    }
    double d = 0;
    auto res = std::from_chars(s_.data() + start, s_.data() + pos_, d);
    if (res.ec != std::errc() || !std::isfinite(d)) {
      msg_ = "invalid number";
      return false;
    }
    v = Value(d);
    return true;
  }
  static void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  bool hex4(uint32_t& cp) {
    if (pos_ + 4 > s_.size()) return false;
    cp = 0;
    for (int i = 0; i < 4; ++i) {
      char c = s_[pos_++];
      cp <<= 4;
      if (c >= '0' && c <= '9') cp |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') cp |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') cp |= static_cast<uint32_t>(c - 'A' + 10);
      else return false;
    }
    return true;
  }
  bool parseString(std::string& out) {
    ++pos_;  // opening quote
    while (pos_ < s_.size()) {
      char c = s_[pos_++];
      if (c == '"') return true;
      if (static_cast<unsigned char>(c) < 0x20) {
        msg_ = "control character in string";
        return false;
      }
      if (c != '\\') {
        out += c;
        continue;
      }
      if (pos_ >= s_.size()) break;
      char e = s_[pos_++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          uint32_t cp = 0;
          if (!hex4(cp)) {
            msg_ = "invalid \\u escape";
            return false;
          }
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            uint32_t lo = 0;
            if (pos_ + 2 <= s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
              pos_ += 2;
              if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) {
                msg_ = "invalid surrogate pair";
                return false;
              }
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else {
              cp = 0xFFFD;
            }
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;
          }
          appendUtf8(out, cp);
          break;
        }
        default:
          msg_ = "invalid escape";
          return false;
      }
    }
    msg_ = "unterminated string";
    return false;
  }
  bool parseArray(Value& v, int depth) {
    ++pos_;
    v = Value::array();
    skipWs();
    if (pos_ < s_.size() && s_[pos_] == ']') {
      ++pos_;
      return true;
    }
    while (true) {
      skipWs();
      Value item;
      if (!parseValue(item, depth + 1)) return false;
      v.vals_.push_back(std::move(item));
      skipWs();
      if (pos_ >= s_.size()) {
        msg_ = "unterminated array";
        return false;
      }
      if (s_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (s_[pos_] == ']') {
        ++pos_;
        return true;
      }
      msg_ = "expected , or ]";
      return false;
    }
  }
  bool parseObject(Value& v, int depth) {
    ++pos_;
    v = Value::object();
    skipWs();
    if (pos_ < s_.size() && s_[pos_] == '}') {
      ++pos_;
      return true;
    }
    while (true) {
      skipWs();
      if (pos_ >= s_.size() || s_[pos_] != '"') {
        msg_ = "expected key";
        return false;
      }
      std::string key;
      if (!parseString(key)) return false;
      skipWs();
      if (pos_ >= s_.size() || s_[pos_] != ':') {
        msg_ = "expected :";
        return false;
      }
      ++pos_;
      skipWs();
      Value item;
      if (!parseValue(item, depth + 1)) return false;
      v.set(key, std::move(item));  // duplicate keys: last one wins
      skipWs();
      if (pos_ >= s_.size()) {
        msg_ = "unterminated object";
        return false;
      }
      if (s_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (s_[pos_] == '}') {
        ++pos_;
        return true;
      }
      msg_ = "expected , or }";
      return false;
    }
  }

  std::string_view s_;
  size_t pos_ = 0;
  std::string msg_ = "parse error";
};

bool parse(std::string_view text, Value& out, std::string* error) {
  // Tolerate a UTF-8 BOM (Notepad likes to add one).
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
      static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
    text.remove_prefix(3);
  Parser p(text);
  Value v;
  if (!p.run(v, error)) return false;
  out = std::move(v);
  return true;
}

static void dumpString(const std::string& s, std::string& out) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
}

void dumpTo(const Value& v, std::string& out, int indent, int depth) {
  auto newline = [&](int d) {
    if (indent <= 0) return;
    out += '\n';
    out.append(static_cast<size_t>(d * indent), ' ');
  };
  switch (v.t_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += v.b_ ? "true" : "false"; break;
    case Type::Number: {
      double n = v.n_;
      if (!std::isfinite(n)) n = 0;
      char buf[64];
      if (std::fabs(n) < 1e15 && n == std::floor(n)) {
        std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(n));
        out += buf;
      } else {
        auto res = std::to_chars(buf, buf + sizeof buf, n);
        out.append(buf, res.ptr);
      }
      break;
    }
    case Type::String: dumpString(v.s_, out); break;
    case Type::Array: {
      if (v.vals_.empty()) {
        out += "[]";
        break;
      }
      out += '[';
      for (size_t i = 0; i < v.vals_.size(); ++i) {
        if (i) out += ',';
        newline(depth + 1);
        dumpTo(v.vals_[i], out, indent, depth + 1);
      }
      newline(depth);
      out += ']';
      break;
    }
    case Type::Object: {
      if (v.keys_.empty()) {
        out += "{}";
        break;
      }
      out += '{';
      for (size_t i = 0; i < v.keys_.size(); ++i) {
        if (i) out += ',';
        newline(depth + 1);
        dumpString(v.keys_[i], out);
        out += indent > 0 ? ": " : ":";
        dumpTo(v.vals_[i], out, indent, depth + 1);
      }
      newline(depth);
      out += '}';
      break;
    }
  }
}

std::string dump(const Value& v, int indent) {
  std::string out;
  dumpTo(v, out, indent, 0);
  if (indent > 0) out += '\n';
  return out;
}

}  // namespace snow::json
