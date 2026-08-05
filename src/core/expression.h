// Phonoscope expression bytecode VM.
//
// A direct port of `PhonoscopeExpression.swift`. The dashboard compiles module
// YAML expressions to this bytecode, so both engines only ever interpret --
// neither parses expression source, and neither can execute module-supplied
// code. Divergence here would show up immediately in the conformance corpus,
// so the operator semantics (including the deliberate divide-by-near-zero and
// non-finite guards) are reproduced exactly.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "core/json.h"

namespace nova {

using ExpressionInputs = std::unordered_map<std::string, double>;

class Expression {
 public:
  // Compiles the `{ "$expr": ..., "code": [...] }` form. Returns false when the
  // value is not an expression object.
  static bool compile(const json::Value& value, Expression& out);

  double evaluate(const ExpressionInputs& inputs) const;

  const std::string& source() const { return source_; }
  bool empty() const { return code_.empty(); }

  // Evaluates a value that may be a plain number, an expression object, or
  // absent. Mirrors `PhonoscopeExpression.evaluate(_:inputs:fallback:)`.
  static double evaluate(const json::Value* value, const ExpressionInputs& inputs,
                         double fallback);

 private:
  struct Instruction {
    enum class Op {
      Const, Load, Neg, Not, Add, Sub, Mul, Div, Mod, Pow,
      Lt, Lte, Gt, Gte, Eq, Neq, And, Or, Call, Nop
    };
    Op op = Op::Nop;
    double value = 0;
    std::string key;
    std::string fn;
    int argc = 0;
  };

  static Instruction::Op opFor(const std::string& name);
  static double call(const std::string& name, const std::vector<double>& values);

  std::string source_;
  std::vector<Instruction> code_;
};

// Extracts the `palette.<slot>` identifiers referenced by an expression source
// string, in order. Port of `phonoscopePaletteSlots(in:)`.
std::vector<std::string> paletteSlots(const std::string& expression);

}  // namespace nova
