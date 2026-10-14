#pragma once
#include "arena.hpp"

#include <cstdint>
#include <array>
#include <map>
#include <deque>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
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
struct Token { TokenKind kind; std::string_view text; Location location; };
struct TokenIR {
  std::vector<Token> tokens;
  std::deque<std::string> strings;
  TokenIR() = default;
  TokenIR(TokenIR &&) = default;
  TokenIR &operator=(TokenIR &&) = default;
  TokenIR(const TokenIR &) = delete;
  auto begin() const { return tokens.begin(); }
  auto end() const { return tokens.end(); }
  const Token &at(size_t n) const { return tokens.at(n); }
  void push_back(Token t) { tokens.push_back(t); }
};

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
struct Expr;
struct Stmt;
using ExprRef = NodeRef<Expr>;
using StmtRef = NodeRef<Stmt>;
struct Expr {
  enum class Kind { Number, String, Variable, Call, Unary, Binary, Element };
  Kind kind;
  Location location;
  std::string text;
  std::vector<ExprRef> operands;
  // The semantic pass fills these fields; subsequent passes consume them.
  Type type;
  std::optional<Number> constant;
  Symbol *symbol = nullptr;
  Function *callee = nullptr;
  bool uniformProduct = false;
  int schedule = -1;
  Expr(Kind kind, Location loc, std::string_view text = {})
      : kind(kind), location(loc), text(text) {}
};

// Visit children in source order, then the parent, without using the host stack.
template<class F> void visitExpressions(Expr &root, F body) {
  std::vector<std::pair<Expr *,size_t>> stack{{&root,0}};
  while (!stack.empty()) {
    auto &frame = stack.back();
    if (frame.second < frame.first->operands.size()) {
      auto child = frame.first->operands[frame.second++].get();
      stack.push_back({child,0});
    } else { auto node = frame.first; stack.pop_back(); body(*node); }
  }
}

struct Initializer {
  Location location;
  bool list = false;
  ExprRef value;
  std::vector<Initializer> children;
};

struct Variable {
  Location location;
  std::string name;
  Base base = Base::Int;
  bool isConst = false, arrayParameter = false, tensor = false;
  std::vector<ExprRef> dimensions;
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
  std::vector<StmtRef> children;
  std::vector<Variable> variables;
  ExprRef expression, target;
  bool scheduled = false;
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
  StmtRef body;
  bool runtime = false, variadic = false, timer = false;
  bool tensorResult = false;
  std::vector<int64_t> resultShape;
};

struct Item {
  std::vector<Variable> variables;
  std::unique_ptr<Function> function;
};

struct TensorSchedule {
  uint32_t node;
  int64_t rows, reduction, columns;
  int columnTile;
  bool uniform;
  Base base;
};
struct PolyAffine {
  std::array<int64_t,2> dimensions{};
  std::map<const Symbol *,int64_t> parameters;
  int64_t constant = 0;
  bool known = true;
};
struct PolyAccess {
  const Symbol *object = nullptr;
  bool write = false, logical = false;
  std::vector<PolyAffine> subscripts;
  PolyAffine offset;
};
struct PolyDependence {
  int source, target;
  bool difference = false;
  std::vector<std::array<int64_t,2>> bounds;
  std::vector<std::vector<int64_t>> equalities;
  // 0 = disproved conflict, 1 = witness/possible conflict, 2 = unknown.
  int parallel = 2, interchange = 2;
};
struct PolyRegion {
  uint32_t statement;
  std::string function, decision = "unchanged";
  std::array<const Symbol *,2> induction{};
  std::array<int64_t,2> lower{}, upper{};
  std::vector<const Symbol *> parameters;
  std::vector<PolyAccess> accesses;
  std::vector<PolyDependence> dependences;
  int tile = 1;
  int predicated = 0;
  size_t queries = 0;
};
struct ProgramIR {
  std::unique_ptr<NodeArena<Expr>> expressions = std::make_unique<NodeArena<Expr>>();
  std::unique_ptr<NodeArena<Stmt>> statements = std::make_unique<NodeArena<Stmt>>();
  std::vector<TensorSchedule> schedules;
  std::vector<PolyRegion> polyRegions;
  std::vector<Item> items;
  std::vector<std::unique_ptr<Symbol>> symbols;
  std::vector<std::unique_ptr<Function>> runtime;
  // After CFG lowering, keep stable semantic identities and global data while
  // ending the lifetime of syntax arenas and all references into them.
  void releaseSyntax();
};

TokenIR tokenize(const Source &source);
ProgramIR parse(const TokenIR &tokens);
void resolve(ProgramIR &program);
void planTensors(ProgramIR &program, int tile = 0);
void printTensorPlan(const ProgramIR &program, std::ostream &out);
void expandTensors(ProgramIR &program);
void analyzeTensors(ProgramIR &program);
void optimizeHighIR(ProgramIR &program, int level);
void optimizePolyhedral(ProgramIR &program, int level, int tile = 0);
void printPolyhedralIR(const ProgramIR &program, std::ostream &out);
void printIR(const ProgramIR &program, std::ostream &out);

} // namespace sysy
