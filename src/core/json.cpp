#include "core/json.h"

#include <cmath>
#include <cstdlib>
#include <sstream>

namespace nova::json {
namespace {

struct Parser {
  std::string_view text;
  size_t pos = 0;
  bool failed = false;

  void skip() {
    while (pos < text.size()) {
      char c = text[pos];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos;
      } else {
        break;
      }
    }
  }

  bool consume(char c) {
    skip();
    if (pos < text.size() && text[pos] == c) {
      ++pos;
      return true;
    }
    return false;
  }

  bool literal(std::string_view word) {
    skip();
    if (text.compare(pos, word.size(), word) == 0) {
      pos += word.size();
      return true;
    }
    return false;
  }

  void appendUtf8(std::string& out, uint32_t code) {
    if (code < 0x80) {
      out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (code >> 6)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (code >> 12)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (code >> 18)));
      out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }

  uint32_t hex4() {
    uint32_t value = 0;
    for (int i = 0; i < 4 && pos < text.size(); ++i, ++pos) {
      char c = text[pos];
      value <<= 4;
      if (c >= '0' && c <= '9') {
        value |= static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        value |= static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        value |= static_cast<uint32_t>(c - 'A' + 10);
      } else {
        failed = true;
        return 0;
      }
    }
    return value;
  }

  std::string parseString() {
    std::string out;
    if (!consume('"')) {
      failed = true;
      return out;
    }
    while (pos < text.size()) {
      char c = text[pos++];
      if (c == '"') return out;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos >= text.size()) break;
      char esc = text[pos++];
      switch (esc) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t code = hex4();
          if (code >= 0xD800 && code <= 0xDBFF && pos + 1 < text.size() &&
              text[pos] == '\\' && text[pos + 1] == 'u') {
            pos += 2;
            uint32_t low = hex4();
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
          }
          appendUtf8(out, code);
          break;
        }
        default: failed = true; return out;
      }
    }
    failed = true;
    return out;
  }

  Value parseValue() {
    skip();
    if (failed || pos >= text.size()) {
      failed = true;
      return Value{};
    }
    char c = text[pos];
    if (c == '{') {
      ++pos;
      Object object;
      skip();
      if (consume('}')) return Value{std::move(object)};
      while (!failed) {
        skip();
        std::string key = parseString();
        if (failed || !consume(':')) {
          failed = true;
          break;
        }
        object.emplace(std::move(key), parseValue());
        if (consume(',')) continue;
        if (consume('}')) return Value{std::move(object)};
        failed = true;
      }
      return Value{};
    }
    if (c == '[') {
      ++pos;
      Array array;
      skip();
      if (consume(']')) return Value{std::move(array)};
      while (!failed) {
        array.push_back(parseValue());
        if (consume(',')) continue;
        if (consume(']')) return Value{std::move(array)};
        failed = true;
      }
      return Value{};
    }
    if (c == '"') return Value{parseString()};
    if (literal("true")) return Value{true};
    if (literal("false")) return Value{false};
    if (literal("null")) return Value{};

    // Number.
    size_t start = pos;
    if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) ++pos;
    while (pos < text.size()) {
      char n = text[pos];
      if ((n >= '0' && n <= '9') || n == '.' || n == 'e' || n == 'E' || n == '+' || n == '-') {
        ++pos;
      } else {
        break;
      }
    }
    if (start == pos) {
      failed = true;
      return Value{};
    }
    std::string chunk(text.substr(start, pos - start));
    double parsed = std::strtod(chunk.c_str(), nullptr);
    if (!std::isfinite(parsed)) parsed = 0;
    return Value{parsed};
  }
};

}  // namespace

std::vector<double> Value::numberArray() const {
  std::vector<double> out;
  if (const Array* items = array()) {
    out.reserve(items->size());
    for (const Value& item : *items) {
      if (auto n = item.number()) out.push_back(*n);
    }
  }
  return out;
}

std::vector<std::string> Value::stringArray() const {
  std::vector<std::string> out;
  if (const Array* items = array()) {
    out.reserve(items->size());
    for (const Value& item : *items) {
      if (auto s = item.string()) out.push_back(*s);
    }
  }
  return out;
}

std::optional<Value> Value::parse(std::string_view text) {
  Parser parser{text};
  Value value = parser.parseValue();
  if (parser.failed) return std::nullopt;
  parser.skip();
  return value;
}

std::string escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
          out += buffer;
        } else {
          out.push_back(c);
        }
    }
  }
  return out;
}

std::string Value::dump() const {
  std::ostringstream out;
  switch (type_) {
    case Type::Null: out << "null"; break;
    case Type::Bool: out << (number_ != 0 ? "true" : "false"); break;
    case Type::Number: {
      if (number_ == static_cast<long long>(number_)) {
        out << static_cast<long long>(number_);
      } else {
        out.precision(10);
        out << number_;
      }
      break;
    }
    case Type::String: out << '"' << escape(string_) << '"'; break;
    case Type::Array: {
      out << '[';
      bool first = true;
      for (const Value& item : *array_) {
        if (!first) out << ',';
        first = false;
        out << item.dump();
      }
      out << ']';
      break;
    }
    case Type::Object: {
      out << '{';
      bool first = true;
      for (const auto& [key, item] : *object_) {
        if (!first) out << ',';
        first = false;
        out << '"' << escape(key) << "\":" << item.dump();
      }
      out << '}';
      break;
    }
  }
  return out.str();
}

}  // namespace nova::json
