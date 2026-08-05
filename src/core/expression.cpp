#include "core/expression.h"

#include <algorithm>
#include <cmath>

namespace nova {
namespace {

bool isIdentifierChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

}  // namespace

Expression::Instruction::Op Expression::opFor(const std::string& name) {
  using Op = Expression::Instruction::Op;
  if (name == "const") return Op::Const;
  if (name == "load") return Op::Load;
  if (name == "neg") return Op::Neg;
  if (name == "not") return Op::Not;
  if (name == "add") return Op::Add;
  if (name == "sub") return Op::Sub;
  if (name == "mul") return Op::Mul;
  if (name == "div") return Op::Div;
  if (name == "mod") return Op::Mod;
  if (name == "pow") return Op::Pow;
  if (name == "lt") return Op::Lt;
  if (name == "lte") return Op::Lte;
  if (name == "gt") return Op::Gt;
  if (name == "gte") return Op::Gte;
  if (name == "eq") return Op::Eq;
  if (name == "neq") return Op::Neq;
  if (name == "and") return Op::And;
  if (name == "or") return Op::Or;
  if (name == "call") return Op::Call;
  return Op::Nop;
}

bool Expression::compile(const json::Value& value, Expression& out) {
  const json::Value* source = value.find("$expr");
  const json::Value* code = value.find("code");
  if (source == nullptr || code == nullptr) return false;
  const json::Array* items = code->array();
  if (items == nullptr) return false;

  out.source_ = source->stringOr("");
  out.code_.clear();
  out.code_.reserve(items->size());
  for (const json::Value& item : *items) {
    Instruction instruction;
    if (const json::Value* op = item.find("op")) instruction.op = opFor(op->stringOr(""));
    if (const json::Value* v = item.find("value")) instruction.value = v->numberOr(0);
    if (const json::Value* k = item.find("key")) instruction.key = k->stringOr("");
    if (const json::Value* f = item.find("fn")) instruction.fn = f->stringOr("");
    if (const json::Value* a = item.find("argc")) instruction.argc = static_cast<int>(a->numberOr(0));
    out.code_.push_back(std::move(instruction));
  }
  return true;
}

double Expression::evaluate(const ExpressionInputs& inputs) const {
  std::vector<double> stack;
  stack.reserve(16);
  auto pop = [&stack]() -> double {
    if (stack.empty()) return 0;
    double value = stack.back();
    stack.pop_back();
    return value;
  };

  for (const Instruction& instruction : code_) {
    using Op = Instruction::Op;
    switch (instruction.op) {
      case Op::Const: stack.push_back(instruction.value); break;
      case Op::Load: {
        auto it = inputs.find(instruction.key);
        stack.push_back(it == inputs.end() ? 0 : it->second);
        break;
      }
      case Op::Neg: stack.push_back(-pop()); break;
      case Op::Not: stack.push_back(pop() == 0 ? 1 : 0); break;
      case Op::Add: { double b = pop(), a = pop(); stack.push_back(a + b); break; }
      case Op::Sub: { double b = pop(), a = pop(); stack.push_back(a - b); break; }
      case Op::Mul: { double b = pop(), a = pop(); stack.push_back(a * b); break; }
      case Op::Div: {
        double b = pop(), a = pop();
        stack.push_back(std::fabs(b) < 0.000001 ? 0 : a / b);
        break;
      }
      case Op::Mod: {
        double b = pop(), a = pop();
        stack.push_back(std::fabs(b) < 0.000001 ? 0 : std::fmod(a, b));
        break;
      }
      case Op::Pow: { double b = pop(), a = pop(); stack.push_back(std::pow(a, b)); break; }
      case Op::Lt: { double b = pop(), a = pop(); stack.push_back(a < b ? 1 : 0); break; }
      case Op::Lte: { double b = pop(), a = pop(); stack.push_back(a <= b ? 1 : 0); break; }
      case Op::Gt: { double b = pop(), a = pop(); stack.push_back(a > b ? 1 : 0); break; }
      case Op::Gte: { double b = pop(), a = pop(); stack.push_back(a >= b ? 1 : 0); break; }
      case Op::Eq: { double b = pop(), a = pop(); stack.push_back(a == b ? 1 : 0); break; }
      case Op::Neq: { double b = pop(), a = pop(); stack.push_back(a != b ? 1 : 0); break; }
      case Op::And: {
        double b = pop(), a = pop();
        stack.push_back(a != 0 && b != 0 ? 1 : 0);
        break;
      }
      case Op::Or: {
        double b = pop(), a = pop();
        stack.push_back(a != 0 || b != 0 ? 1 : 0);
        break;
      }
      case Op::Call: {
        int count = std::max(0, instruction.argc);
        std::vector<double> args(static_cast<size_t>(count));
        // Swift pops argc values then reverses, i.e. args end up in source
        // order. Fill back to front to match.
        for (int i = count - 1; i >= 0; --i) args[static_cast<size_t>(i)] = pop();
        stack.push_back(call(instruction.fn, args));
        break;
      }
      case Op::Nop: break;
    }
    if (!stack.empty() && !std::isfinite(stack.back())) stack.back() = 0;
  }
  return stack.empty() ? 0 : stack.back();
}

double Expression::call(const std::string& name, const std::vector<double>& values) {
  auto at = [&values](size_t index) -> double {
    return index < values.size() ? values[index] : 0;
  };
  const double a = at(0);
  const double b = at(1);
  const double c = at(2);

  if (name == "sin") return std::sin(a);
  if (name == "cos") return std::cos(a);
  if (name == "tan") return std::tan(a);
  if (name == "abs") return std::fabs(a);
  if (name == "sqrt") return a >= 0 ? std::sqrt(a) : 0;
  if (name == "floor") return std::floor(a);
  if (name == "ceil") return std::ceil(a);
  if (name == "fract") return a - std::floor(a);
  if (name == "exp") return std::exp(a);
  if (name == "log") return a > 0 ? std::log(a) : 0;
  if (name == "min") {
    if (values.empty()) return 0;
    return *std::min_element(values.begin(), values.end());
  }
  if (name == "max") {
    if (values.empty()) return 0;
    return *std::max_element(values.begin(), values.end());
  }
  if (name == "pow") return std::pow(a, b);
  if (name == "clamp") return std::min(std::max(a, b), c);
  if (name == "mix") return a + (b - a) * c;
  if (name == "step") return b < a ? 0 : 1;
  if (name == "smoothstep") {
    if (b == a) return 0;
    double t = std::min(std::max((c - a) / (b - a), 0.0), 1.0);
    return t * t * (3 - 2 * t);
  }
  if (name == "select") return a != 0 ? b : c;
  if (name == "noise") {
    double value = std::sin(a * 12.9898 + b * 78.233 + c * 37.719) * 43758.5453;
    return value - std::floor(value);
  }
  return a;
}

double Expression::evaluate(const json::Value* value, const ExpressionInputs& inputs,
                            double fallback) {
  if (value == nullptr) return fallback;
  if (auto number = value->number()) return *number;
  Expression expression;
  if (!compile(*value, expression)) return fallback;
  return expression.evaluate(inputs);
}

std::vector<std::string> paletteSlots(const std::string& expression) {
  // Swift splits on the inverse of [alphanumerics + '_'], which yields empty
  // components between adjacent separators. Those empty components matter:
  // the lookup requires the component right after "palette" to be non-empty,
  // so "palette . primary" must not match while "palette.primary" does.
  std::vector<std::string> parts;
  std::string current;
  for (char c : expression) {
    if (isIdentifierChar(c)) {
      current.push_back(c);
    } else {
      parts.push_back(current);
      current.clear();
    }
  }
  parts.push_back(current);

  std::vector<std::string> slots;
  for (size_t index = 0; index < parts.size(); ++index) {
    if (parts[index] != "palette") continue;
    if (index + 1 >= parts.size() || parts[index + 1].empty()) continue;
    slots.push_back(parts[index + 1]);
  }
  return slots;
}

}  // namespace nova
