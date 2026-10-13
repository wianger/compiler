#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sysy {

struct Location { int line = 1, column = 1; };

struct Error : std::runtime_error {
  Location location;
  Error(Location loc, const std::string &message)
      : std::runtime_error(message), location(loc) {}
};

struct Source { std::string path, text; };
enum class TokenKind { Identifier, Number, String, Punctuation, End };
struct Token { TokenKind kind; std::string text; Location location; };
using TokenIR = std::vector<Token>;

enum class Base { Int, Float, Void, String };
struct Type {
  Base base = Base::Int;
  // Array parameters have -1 as their first extent. Arrays retain their shape
  // until lowering, so partial indexing and argument checks stay target-independent.
  std::vector<int64_t> shape;
  bool tensor = false;
  bool numeric() const { return shape.empty() && (base == Base::Int || base == Base::Float); }
  bool array() const { return !shape.empty(); }
  int64_t elements() const;
  std::string str() const;
};

struct Number {
  Base base = Base::Int;
  int32_t integer = 0;
  float floating = 0;
  static Number fromInt(int32_t v) { return {Base::Int, v, 0}; }
  static Number fromFloat(float v) { return {Base::Float, 0, v}; }
  bool truth() const { return base == Base::Float ? floating != 0 : integer != 0; }
};
Number convert(Number value, Base type, Location loc);

struct Symbol;
struct Function;
struct Expr {
  enum class Kind { Number, String, Variable, Call, Unary, Binary, Element };
  Kind kind;
  Location location;
  std::string text;
  std::vector<std::unique_ptr<Expr>> operands;
  // The semantic pass fills these fields; subsequent passes consume them.
  Type type;
  std::optional<Number> constant;
  Symbol *symbol = nullptr;
  Function *callee = nullptr;
  bool uniformProduct = false;
  Expr(Kind kind, Location loc, std::string text = {})
      : kind(kind), location(loc), text(std::move(text)) {}
};

struct Initializer {
  Location location;
  bool list = false;
  std::unique_ptr<Expr> value;
  std::vector<Initializer> children;
};

struct Variable {
  Location location;
  std::string name;
  Base base = Base::Int;
  bool isConst = false, arrayParameter = false, tensor = false;
  std::vector<std::unique_ptr<Expr>> dimensions;
  std::optional<Initializer> initializer;
  Symbol *symbol = nullptr;
  // Sparse row-major initialization: missing entries are zero. Expressions
  // remain in source order, including local initializers with side effects.
  std::vector<std::pair<int64_t, Expr *>> initializers;
};

struct Stmt {
  enum class Kind { Block, Declaration, Expression, Assignment, If, While, Break, Continue, Return };
  Kind kind;
  Location location;
  std::vector<std::unique_ptr<Stmt>> children;
  std::vector<Variable> variables;
  std::unique_ptr<Expr> expression, target;
  Stmt(Kind kind, Location loc) : kind(kind), location(loc) {}
};

struct Symbol {
  std::string name;
  Type type;
  bool isConst = false, global = false, parameter = false, constantReady = false;
  // Only nonzero values need storage. This also supports constant array indexing.
  std::unordered_map<int64_t, Number> values;
};

struct Function {
  Location location;
  std::string name, linkageName;
  Base result = Base::Void;
  std::vector<Variable> parameters;
  std::unique_ptr<Stmt> body;
  bool runtime = false, variadic = false, timer = false;
  bool tensorResult = false;
  std::vector<int64_t> resultShape;
};

struct Item {
  std::vector<Variable> variables;
  std::unique_ptr<Function> function;
};

struct ProgramIR {
  std::vector<Item> items;
  std::vector<std::unique_ptr<Symbol>> symbols;
  std::vector<std::unique_ptr<Function>> runtime;
};

TokenIR tokenize(const Source &source);
ProgramIR parse(const TokenIR &tokens);
void resolve(ProgramIR &program);
void expandTensors(ProgramIR &program);
void analyzeTensors(ProgramIR &program);
void optimizeHighIR(ProgramIR &program, int level);
void printIR(const ProgramIR &program, std::ostream &out);

} // namespace sysy
