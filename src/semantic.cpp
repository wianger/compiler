#include "ir.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>

namespace sysy {
int64_t Type::elements() const {
  int64_t count = 1;
  for (auto extent : shape) {
    if (extent < 0 || (extent && count > std::numeric_limits<int64_t>::max() / 4 / extent))
      throw std::overflow_error("array size exceeds addressable storage");
    count *= extent;
  }
  return count;
}
std::string Type::str() const {
  std::string result = base == Base::Int ? "int" : base == Base::Float ? "float" : base == Base::Void ? "void" : "string";
  if (tensor && array()) result = "tensor " + result;
  for (auto extent : shape) result += "[" + (extent < 0 ? std::string() : std::to_string(extent)) + "]";
  return result;
}
Number convert(Number value, Base type, Location loc) {
  if (value.base == type) return value;
  if (type == Base::Float) return Number::fromFloat(static_cast<float>(value.integer));
  if (!std::isfinite(value.floating) || value.floating < -2147483648.0 || value.floating >= 2147483648.0)
    throw Error(loc, "floating constant is outside the int range");
  return Number::fromInt(static_cast<int32_t>(value.floating));
}

namespace {
Number literal(const Expr &e) {
  std::string text = e.text;
  bool hexadecimal = text.size() > 1 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
  bool floating = text.find('.') != std::string::npos ||
      text.find_first_of(hexadecimal ? "pP" : "eE") != std::string::npos;
  char *end;
  errno = 0;
  if (floating) {
    if (text.back() == 'f' || text.back() == 'F' || text.back() == 'l' || text.back() == 'L') text.pop_back();
    float value = std::strtof(text.c_str(), &end);
    if (*end || std::isinf(value)) throw Error(e.location, "invalid or overflowing floating literal");
    return Number::fromFloat(value);
  }
  while (text.back() == 'u' || text.back() == 'U' || text.back() == 'l' || text.back() == 'L') text.pop_back();
  auto value = std::strtoull(text.c_str(), &end, 0);
  if (*end || errno == ERANGE || value > UINT32_MAX) throw Error(e.location, "invalid or overflowing integer literal");
  return Number::fromInt(static_cast<int32_t>(static_cast<uint32_t>(value)));
}

std::optional<Number> binary(const std::string &op, Number a, Number b, Location loc) {
  if (op == "&&") return Number::fromInt(a.truth() && b.truth());
  if (op == "||") return Number::fromInt(a.truth() || b.truth());
  if (a.base == Base::Float || b.base == Base::Float) {
    float x = convert(a, Base::Float, loc).floating, y = convert(b, Base::Float, loc).floating;
    if (op == "+") return Number::fromFloat(x + y);
    if (op == "-") return Number::fromFloat(x - y);
    if (op == "*") return Number::fromFloat(x * y);
    if (op == "/") return Number::fromFloat(x / y);
    if (op == "==") return Number::fromInt(x == y);
    if (op == "!=") return Number::fromInt(x != y);
    if (op == "<") return Number::fromInt(x < y);
    if (op == ">") return Number::fromInt(x > y);
    if (op == "<=") return Number::fromInt(x <= y);
    if (op == ">=") return Number::fromInt(x >= y);
  }
  int32_t x = a.integer, y = b.integer;
  uint32_t ux = static_cast<uint32_t>(x), uy = static_cast<uint32_t>(y);
  if (op == "+") return Number::fromInt(static_cast<int32_t>(ux + uy));
  if (op == "-") return Number::fromInt(static_cast<int32_t>(ux - uy));
  if (op == "*") return Number::fromInt(static_cast<int32_t>(ux * uy));
  if (op == "/" || op == "%") {
    if (y == 0 || (x == INT32_MIN && y == -1)) return std::nullopt;
    return Number::fromInt(op == "/" ? x / y : x % y);
  }
  if (op == "==") return Number::fromInt(x == y);
  if (op == "!=") return Number::fromInt(x != y);
  if (op == "<") return Number::fromInt(x < y);
  if (op == ">") return Number::fromInt(x > y);
  if (op == "<=") return Number::fromInt(x <= y);
  if (op == ">=") return Number::fromInt(x >= y);
  throw std::logic_error("unknown binary operator");
}

class Resolver {
  ProgramIR &program;
  std::vector<std::unordered_map<std::string, Symbol *>> scopes;
  std::unordered_map<std::string, Function *> functions;
  std::unordered_map<std::string, Location> topNames;
  Function *current = nullptr;
  int loops = 0;

  Symbol *makeSymbol(std::string name, Type type, bool isConst, bool global, bool parameter) {
    auto s = std::make_unique<Symbol>();
    s->name = std::move(name); s->type = std::move(type); s->isConst = isConst;
    s->global = global; s->parameter = parameter;
    auto result = s.get(); program.symbols.push_back(std::move(s)); return result;
  }
  void topName(const std::string &name, Location loc) {
    if (!topNames.emplace(name, loc).second || functions.count(name))
      throw Error(loc, "redefinition of '" + name + "'");
  }
  Symbol *lookup(const Expr &e) {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      auto found = it->find(e.text);
      if (found != it->end()) return found->second;
    }
    throw Error(e.location, "undefined variable '" + e.text + "'");
  }
  void numeric(const Expr &e) {
    if (!e.type.numeric()) throw Error(e.location, "expected numeric value, found " + e.type.str());
  }
  void expr(Expr &e) {
    for (auto &operand : e.operands) expr(*operand);
    switch (e.kind) {
    case Expr::Kind::Number:
      e.constant = literal(e); e.type.base = e.constant->base; return;
    case Expr::Kind::String: e.type.base = Base::String; return;
    case Expr::Kind::Variable: {
      e.symbol = lookup(e);
      const auto &type = e.symbol->type;
      if (e.operands.size() > type.shape.size()) throw Error(e.location, "too many array indices");
      int64_t offset = 0;
      bool known = true;
      for (size_t i = 0; i < e.operands.size(); ++i) {
        auto &index = *e.operands[i];
        if (!index.type.numeric() || index.type.base != Base::Int) throw Error(index.location, "array index must be int");
        if (!index.constant) { known = false; continue; }
        auto n = index.constant->integer;
        if (n < 0 || (type.shape[i] >= 0 && n >= type.shape[i])) known = false;
        int64_t stride = 1;
        for (size_t j = i + 1; j < type.shape.size(); ++j) stride *= type.shape[j];
        // An out-of-bounds access stays an access, never a folded constant.
        if (known) offset += n * stride;
      }
      e.type = {type.base, std::vector<int64_t>(type.shape.begin() + e.operands.size(), type.shape.end()),
                type.tensor && e.operands.size() < type.shape.size()};
      if (e.type.numeric() && e.symbol->isConst && e.symbol->constantReady && known) {
        auto it = e.symbol->values.find(offset);
        e.constant = it == e.symbol->values.end() ?
            (type.base == Base::Int ? Number::fromInt(0) : Number::fromFloat(0)) : it->second;
      }
      return;
    }
    case Expr::Kind::Call: {
      auto it = functions.find(e.text);
      if (it == functions.end()) throw Error(e.location, "undefined function '" + e.text + "'");
      auto &f = *it->second; e.callee = &f; e.type.base = f.result;
      if (f.tensorResult) {
        if (f.resultShape.empty()) throw Error(e.location, "tensor return shape is not yet known");
        e.type = {f.result, f.resultShape, true};
      }
      if (e.operands.size() < f.parameters.size() || (!f.variadic && e.operands.size() != f.parameters.size()))
        throw Error(e.location, "wrong argument count for '" + e.text + "'");
      for (size_t i = 0; i < e.operands.size(); ++i) {
        auto &arg = *e.operands[i];
        if (i >= f.parameters.size()) { numeric(arg); continue; }
        const auto &expected = f.parameters[i].symbol->type;
        if (expected.numeric()) numeric(arg);
        else if (expected.base == Base::String) {
          if (arg.kind != Expr::Kind::String) throw Error(arg.location, "format argument must be a string literal");
        } else {
          bool matches = expected.base == arg.type.base && expected.shape.size() == arg.type.shape.size();
          for (size_t j = 1; matches && j < expected.shape.size(); ++j)
            matches = expected.shape[j] == arg.type.shape[j];
          if (!matches) throw Error(arg.location, "array argument has incompatible shape or element type");
        }
      }
      return;
    }
    case Expr::Kind::Unary: {
      auto &a = *e.operands[0];
      if (a.type.tensor && a.type.array() && e.text != "!") { e.type = a.type; return; }
      numeric(a);
      e.type.base = e.text == "!" ? Base::Int : a.type.base;
      if (!a.constant) return;
      auto n = *a.constant;
      if (e.text == "!") n = Number::fromInt(!n.truth());
      else if (e.text == "-") n = n.base == Base::Float ? Number::fromFloat(-n.floating) :
          Number::fromInt(static_cast<int32_t>(0u - static_cast<uint32_t>(n.integer)));
      e.constant = n; return;
    }
    case Expr::Kind::Binary: {
      auto &a = *e.operands[0], &b = *e.operands[1];
      if (a.type.array() || b.type.array()) {
        if ((!a.type.numeric() && !a.type.tensor) || (!b.type.numeric() && !b.type.tensor))
          throw Error(e.location, "array arithmetic requires tensor operands");
        Base common = a.type.base == Base::Float || b.type.base == Base::Float ? Base::Float : Base::Int;
        if (e.text == "@") {
          if (a.type.shape.size() != 2 || b.type.shape.size() != 2 || a.type.shape[1] != b.type.shape[0])
            throw Error(e.location, "matrix multiplication requires compatible rank-2 tensors");
          e.type = {common, {a.type.shape[0], b.type.shape[1]}, true};
        } else {
          if (e.text != "+" && e.text != "-" && e.text != "*" && e.text != "/" && e.text != "%")
            throw Error(e.location, "unsupported tensor operator");
          if (a.type.array() && b.type.array() && a.type.shape != b.type.shape)
            throw Error(e.location, "tensor operands must have the same shape");
          if (e.text == "%" && common != Base::Int) throw Error(e.location, "remainder requires int operands");
          e.type = {common, a.type.array() ? a.type.shape : b.type.shape, true};
        }
        for (auto size : e.type.shape) if (size < 0) throw Error(e.location, "tensor operation requires known extents");
        return;
      }
      if (e.text == "@") throw Error(e.location, "matrix multiplication requires tensor operands");
      numeric(a); numeric(b);
      bool logical = e.text == "&&" || e.text == "||";
      bool comparison = e.text == "==" || e.text == "!=" || e.text == "<" || e.text == ">" || e.text == "<=" || e.text == ">=";
      e.type.base = (logical || comparison) ? Base::Int :
          (a.type.base == Base::Float || b.type.base == Base::Float ? Base::Float : Base::Int);
      if (e.text == "%" && (a.type.base != Base::Int || b.type.base != Base::Int))
        throw Error(e.location, "remainder requires int operands");
      if (logical && a.constant && ((e.text == "&&" && !a.constant->truth()) || (e.text == "||" && a.constant->truth())))
        e.constant = Number::fromInt(a.constant->truth());
      else if (a.constant && b.constant) e.constant = binary(e.text, *a.constant, *b.constant, e.location);
      return;
    }
    case Expr::Kind::Element: throw std::logic_error("tensor elements must be introduced after resolution");
    }
  }
  int64_t dimension(Expr &e) {
    expr(e);
    if (!e.type.numeric() || e.type.base != Base::Int || !e.constant || e.constant->integer < 0)
      throw Error(e.location, "array dimension must be a nonnegative constant int");
    return e.constant->integer;
  }
  void flatten(Variable &v, Initializer &init, size_t depth, int64_t start, int64_t capacity) {
    if (!init.list) {
      if (capacity < 1) throw Error(init.location, "too many initializers");
      v.initializers.emplace_back(start, init.value.get()); return;
    }
    const auto &shape = v.symbol->type.shape;
    if (depth >= shape.size()) {
      if (init.children.size() > 1) throw Error(init.location, "too many scalar initializers");
      if (!init.children.empty()) flatten(v, init.children[0], depth, start, capacity);
      return;
    }
    int64_t cursor = 0;
    for (auto &child : init.children) {
      if (cursor >= capacity) throw Error(child.location, "too many array initializers");
      if (!child.list) { flatten(v, child, shape.size(), start + cursor, 1); ++cursor; continue; }
      size_t subDepth = depth + 1;
      int64_t span = 1;
      for (size_t j = subDepth; j < shape.size(); ++j) span *= shape[j];
      if (span == 0) throw Error(child.location, "initializer for a zero-sized array");
      while (cursor % span != 0) { span /= shape[subDepth++]; }
      flatten(v, child, subDepth, start + cursor, span);
      cursor += span;
    }
  }
  void variable(Variable &v, bool global, bool parameter = false) {
    Type type{v.base, {}, v.tensor};
    if (v.arrayParameter) type.shape.push_back(-1);
    for (auto &d : v.dimensions) type.shape.push_back(dimension(*d));
    if (v.tensor && type.shape.empty()) throw Error(v.location, "tensor variable must have an array shape");
    int64_t count = 1;
    try {
      if (v.arrayParameter) { Type tail = type; tail.shape.erase(tail.shape.begin()); tail.elements(); }
      else count = type.elements();
    } catch (const std::overflow_error &e) { throw Error(v.location, e.what()); }
    if (scopes.back().count(v.name)) throw Error(v.location, "redefinition of '" + v.name + "'");
    if (global) topName(v.name, v.location);
    v.symbol = makeSymbol(v.name, std::move(type), v.isConst, global, parameter);
    scopes.back()[v.name] = v.symbol;
    if (!v.initializer) return;
    if (v.symbol->type.array() && !v.initializer->list) throw Error(v.location, "array requires a brace initializer");
    flatten(v, *v.initializer, 0, 0, count);
    for (auto [index, e] : v.initializers) {
      expr(*e); numeric(*e);
      if (v.symbol->type.array() && v.base == Base::Int && e->type.base == Base::Float)
        throw Error(e->location, "int array initializer cannot contain float elements");
      if (global || v.isConst) {
        if (!e->constant) throw Error(e->location, "initializer must be a constant expression");
        Number value = convert(*e->constant, v.base, e->location);
        if (value.truth() || (value.base == Base::Float && std::signbit(value.floating))) v.symbol->values[index] = value;
      }
    }
    v.symbol->constantReady = v.isConst;
  }
  void statement(Stmt &s, bool body = false) {
    switch (s.kind) {
    case Stmt::Kind::Block:
      if (!body) scopes.emplace_back();
      for (auto &child : s.children) statement(*child);
      if (!body) scopes.pop_back();
      return;
    case Stmt::Kind::Declaration:
      for (auto &v : s.variables) variable(v, false);
      return;
    case Stmt::Kind::Expression:
      if (s.expression) {
        expr(*s.expression);
        if ((s.expression->type.array() && s.expression->kind != Expr::Kind::Call) || s.expression->type.base == Base::String)
          throw Error(s.location, "expression statement requires a scalar or void call");
      }
      return;
    case Stmt::Kind::Assignment:
      expr(*s.target); expr(*s.expression);
      if (s.target->kind != Expr::Kind::Variable || s.target->symbol->isConst)
        throw Error(s.target->location, "assignment requires a mutable lvalue");
      if (s.target->type.tensor && s.target->type.array()) {
        if (!s.expression->type.numeric() && (!s.expression->type.tensor || s.target->type.shape != s.expression->type.shape))
          throw Error(s.location, "tensor assignment requires a scalar or a matching tensor shape");
        return;
      }
      numeric(*s.target); numeric(*s.expression); return;
    case Stmt::Kind::If:
      expr(*s.expression); numeric(*s.expression);
      for (auto &child : s.children) statement(*child);
      return;
    case Stmt::Kind::While:
      expr(*s.expression); numeric(*s.expression);
      ++loops; statement(*s.children[0]); --loops; return;
    case Stmt::Kind::Break: case Stmt::Kind::Continue:
      if (!loops) throw Error(s.location, "loop control statement outside a loop");
      return;
    case Stmt::Kind::Return:
      if (s.expression) {
        expr(*s.expression);
        if (current->tensorResult) {
          if (!s.expression->type.tensor || !s.expression->type.array()) throw Error(s.location, "tensor function must return a tensor");
          for (auto extent : s.expression->type.shape) if (extent < 0) throw Error(s.location, "tensor return shape must be known");
          if (current->resultShape.empty()) current->resultShape = s.expression->type.shape;
          else if (current->resultShape != s.expression->type.shape) throw Error(s.location, "tensor returns must have a consistent shape");
          return;
        }
        numeric(*s.expression);
        if (current->result == Base::Void) throw Error(s.location, "void function cannot return a value");
      } else if (current->result != Base::Void) throw Error(s.location, "non-void function must return a value");
      return;
    }
  }
  void runtime(std::string name, Base result, std::vector<Type> params, bool variadic = false, bool timer = false) {
    auto f = std::make_unique<Function>();
    f->name = name; f->linkageName = timer ? "_sysy_" + name : name; f->result = result;
    f->runtime = true; f->variadic = variadic; f->timer = timer;
    for (auto type : params) {
      Variable v; v.symbol = makeSymbol("", std::move(type), false, false, true);
      f->parameters.push_back(std::move(v));
    }
    functions[name] = f.get(); program.runtime.push_back(std::move(f));
  }
public:
  explicit Resolver(ProgramIR &program) : program(program) {}
  void run() {
    Type i{Base::Int, {}}, f{Base::Float, {}}, ia{Base::Int, {-1}}, fa{Base::Float, {-1}};
    runtime("getint", Base::Int, {}); runtime("getch", Base::Int, {}); runtime("getfloat", Base::Float, {});
    runtime("getarray", Base::Int, {ia}); runtime("getfarray", Base::Int, {fa});
    runtime("putint", Base::Void, {i}); runtime("putch", Base::Void, {i}); runtime("putfloat", Base::Void, {f});
    runtime("putarray", Base::Void, {i, ia}); runtime("putfarray", Base::Void, {i, fa});
    runtime("putf", Base::Void, {{Base::String, {}}}, true);
    runtime("starttime", Base::Void, {}, false, true); runtime("stoptime", Base::Void, {}, false, true);
    scopes.emplace_back();
    bool main = false;
    for (auto &item : program.items) {
      if (!item.function) { for (auto &v : item.variables) variable(v, true); continue; }
      auto &function = *item.function;
      topName(function.name, function.location);
      // Signatures see declarations already encountered; the function itself is
      // registered before its body to support recursion without forward declarations.
      scopes.emplace_back();
      for (auto &p : function.parameters) variable(p, false, true);
      functions[function.name] = &function;
      current = &function;
      if (function.name == "main") {
        if (function.tensorResult || function.result != Base::Int || !function.parameters.empty())
          throw Error(function.location, "main must have signature int main()");
        main = true;
      }
      statement(*function.body, true);
      if (function.tensorResult && function.resultShape.empty()) throw Error(function.location, "tensor function needs a return with a known shape");
      scopes.pop_back();
    }
    if (!main) throw Error({}, "program must define int main()");
  }
};

void printExpr(const Expr &e, std::ostream &out) {
  out << '(' << e.type.str() << ' ';
  if (e.kind == Expr::Kind::String) out << std::quoted(e.text);
  else out << e.text;
  for (const auto &child : e.operands) { out << ' '; printExpr(*child, out); }
  out << ')';
}
void printVariable(const Variable &v, int indent, std::ostream &out) {
  out << std::string(indent, ' ') << (v.isConst ? "const " : "var ") << v.name << ": " << v.symbol->type.str() << '\n';
  for (auto [offset, expr] : v.initializers) {
    out << std::string(indent + 2, ' ') << '[' << offset << "] = "; printExpr(*expr, out); out << '\n';
  }
}
void printStmt(const Stmt &s, int indent, std::ostream &out) {
  static const char *names[] = {"block", "declare", "eval", "assign", "if", "while", "break", "continue", "return"};
  out << std::string(indent, ' ') << names[static_cast<int>(s.kind)];
  if (s.target) { out << ' '; printExpr(*s.target, out); }
  if (s.expression) { out << ' '; printExpr(*s.expression, out); }
  out << '\n';
  for (const auto &v : s.variables) printVariable(v, indent + 2, out);
  for (const auto &child : s.children) printStmt(*child, indent + 2, out);
}
} // namespace

void resolve(ProgramIR &program) { Resolver(program).run(); }
void printIR(const ProgramIR &program, std::ostream &out) {
  for (const auto &item : program.items) {
    for (const auto &v : item.variables) printVariable(v, 0, out);
    if (!item.function) continue;
    const auto &f = *item.function;
    out << "func " << f.name << " -> " << Type{f.result, {}}.str() << '\n';
    for (const auto &p : f.parameters) printVariable(p, 2, out);
    printStmt(*f.body, 2, out);
  }
}
} // namespace sysy
