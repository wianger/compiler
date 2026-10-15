#include "ir.hpp"

#include <algorithm>
#include <functional>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>

namespace sysy {
namespace {
using E = ExprRef;
using S = StmtRef;

struct NodeBuilder {
  ProgramIR &program;
  explicit NodeBuilder(ProgramIR &p) : program(p) {}
  E clone(const Expr &source) {
    auto e = program.expressions->make(source.kind, source.location, source.text);
    e->type = source.type; e->constant = source.constant; e->symbol = source.symbol; e->callee = source.callee;
    e->uniformProduct = source.uniformProduct; e->schedule = source.schedule;
    for (const auto &operand : source.operands) e->operands.push_back(clone(*operand));
    return e;
  }
  E reference(Symbol *symbol) {
    auto e = program.expressions->make(Expr::Kind::Variable, Location{}, symbol->name);
    e->symbol = symbol; e->type = symbol->type; return e;
  }
  E integer(int64_t n) {
    if (n < INT32_MIN || n > INT32_MAX) throw Error({}, "tensor lowering requires an int-sized iteration space");
    auto e = program.expressions->make(Expr::Kind::Number, Location{}, std::to_string(n));
    e->constant = Number::fromInt(n); return e;
  }
  E zero(Base base) {
    auto e = integer(0);
    if (base == Base::Float) { e->type.base = base; e->constant = Number::fromFloat(0); }
    return e;
  }
  E boolean(E source) {
    auto inner = program.expressions->make(Expr::Kind::Unary, source->location, "!");
    inner->operands.push_back(std::move(source));
    auto outer = program.expressions->make(Expr::Kind::Unary, inner->location, "!");
    outer->operands.push_back(std::move(inner)); return outer;
  }
  E binary(std::string op, E a, E b) {
    auto e = program.expressions->make(Expr::Kind::Binary, a->location, op);
    e->type.base = op == "<" ? Base::Int : (a->type.base == Base::Float || b->type.base == Base::Float ? Base::Float : Base::Int);
    e->operands.push_back(std::move(a)); e->operands.push_back(std::move(b)); return e;
  }
  E element(E array, E index) {
    auto e = program.expressions->make(Expr::Kind::Element, array->location, "element");
    e->type.base = array->type.base;
    e->operands.push_back(std::move(array)); e->operands.push_back(std::move(index)); return e;
  }
  S block() { return program.statements->make(Stmt::Kind::Block, Location{}); }
  S assign(E target, E expression) {
    auto s = program.statements->make(Stmt::Kind::Assignment, target->location);
    s->target = std::move(target); s->expression = std::move(expression); return s;
  }
  S eval(E expression) {
    auto s = program.statements->make(Stmt::Kind::Expression, expression->location);
    s->expression = std::move(expression); return s;
  }
  S declare(Symbol *symbol, E initial = {}) {
    Variable v; v.name = symbol->name; v.symbol = symbol; v.base = symbol->type.base; v.tensor = symbol->type.tensor;
    if (initial) {
      v.initializer.emplace(); v.initializer->value = std::move(initial);
      v.initializers.emplace_back(0, v.initializer->value.get());
    }
    auto s = program.statements->make(Stmt::Kind::Declaration, Location{});
    s->variables.push_back(std::move(v)); return s;
  }

};

class TensorExpansion : NodeBuilder {
  std::unordered_map<const Function *, Symbol *> results;
  Function *current = nullptr;
  int serial = 0;

  Symbol *temporary(Type type) {
    auto s = std::make_unique<Symbol>(); s->name = ".tensor" + std::to_string(serial++); s->type = std::move(type);
    auto pointer = s.get(); program.symbols.push_back(std::move(s)); return pointer;
  }
  S loop(int64_t count, const std::function<S(Symbol *)> &body) {
    auto index = temporary({Base::Int, {}});
    auto outer = block(); outer->children.push_back(declare(index, integer(0)));
    if (count == 0) return outer;
    if (count == 1) { outer->children.push_back(body(index)); return outer; }
    auto w = program.statements->make(Stmt::Kind::While, Location{});
    w->scheduled = true;
    w->expression = binary("<", reference(index), integer(count));
    auto inner = block(); inner->children.push_back(body(index));
    inner->children.push_back(assign(reference(index), binary("+", reference(index), integer(1))));
    w->children.push_back(std::move(inner)); outer->children.push_back(std::move(w)); return outer;
  }
  E scalarize(const Expr &e, const Expr &index) {
    if (!e.type.array()) return clone(e);
    if (e.kind == Expr::Kind::Variable) return element(clone(e), clone(index));
    if (e.kind == Expr::Kind::Binary) return binary(e.text, scalarize(*e.operands[0], index), scalarize(*e.operands[1], index));
    if (e.kind == Expr::Kind::Unary) {
      auto result = program.expressions->make(Expr::Kind::Unary, e.location, e.text);
      result->type.base = e.type.base; result->operands.push_back(scalarize(*e.operands[0], index)); return result;
    }
    throw std::logic_error("unexpanded tensor operand");
  }
  S copy(E target, E expression, int64_t count) {
    return loop(count, [&](Symbol *i) {
      return assign(element(clone(*target), reference(i)), scalarize(*expression, *reference(i)));
    });
  }
  S matmul(Symbol *target, const Expr &a, const Expr &b, int width) {
    int64_t rows = a.type.shape[0], inner = a.type.shape[1], columns = b.type.shape[1];
    return loop(rows, [&](Symbol *i) {
      auto row = block();
      auto tile = [&](Symbol *j, int width, int64_t first, int64_t step) {
        std::vector<Symbol *> sums; auto body = block();
        for (int lane = 0; lane < width; ++lane) {
          auto sum = temporary({target->type.base, {}}); sums.push_back(sum);
          body->children.push_back(declare(sum, zero(sum->type.base)));
        }
        auto column = [&](int lane) { return binary("+", binary("*", reference(j), integer(step)), integer(first+lane)); };
        body->children.push_back(loop(inner, [&](Symbol *k) {
          auto microkernel = block();
          for (int lane = 0; lane < width; ++lane) {
            auto left = element(clone(a), binary("+", binary("*", reference(i), integer(inner)), reference(k)));
            auto right = element(clone(b), binary("+", binary("*", reference(k), integer(columns)), column(lane)));
            microkernel->children.push_back(assign(reference(sums[lane]), binary("+", reference(sums[lane]), binary("*", std::move(left), std::move(right)))));
          }
          return microkernel;
        }));
        for (int lane = 0; lane < width; ++lane) {
          auto offset = binary("+", binary("*", reference(i), integer(columns)), column(lane));
          body->children.push_back(assign(element(reference(target), std::move(offset)), reference(sums[lane])));
        }
        return body;
      };
      if (columns >= width) row->children.push_back(loop(columns/width, [&](Symbol *j) { return tile(j, width, 0, width); }));
      if (columns % width) row->children.push_back(loop(columns%width, [&](Symbol *j) { return tile(j, 1, columns/width*width, 1); }));
      return row;
    });
  }
  S uniformMatmul(Symbol *target, const Expr &a, const Expr &b) {
    int64_t inner = a.type.shape[1];
    return loop(a.type.shape[0], [&](Symbol *i) {
      return loop(b.type.shape[1], [&](Symbol *j) {
        auto left = clone(a); left->operands.push_back(reference(i)); left->operands.push_back(integer(0)); left->type = {a.type.base, {}};
        auto right = clone(b); right->operands.push_back(integer(0)); right->operands.push_back(reference(j)); right->type = {b.type.base, {}};
        auto product = binary("*", std::move(left), std::move(right));
        auto scaled = binary("*", integer(inner), std::move(product));
        // The original dot product starts with +0, including when every term
        // is -0. Preserve that sign rather than introducing a reassociation.
        if (target->type.base == Base::Float) scaled = binary("+", zero(Base::Float), std::move(scaled));
        auto offset = binary("+", binary("*", reference(i), integer(b.type.shape[1])), reference(j));
        return assign(element(reference(target), std::move(offset)), std::move(scaled));
      });
    });
  }
  E materialize(E expression, std::vector<S> &prefix) {
    auto symbol = temporary(expression->type); prefix.push_back(declare(symbol));
    prefix.push_back(copy(reference(symbol), std::move(expression), symbol->type.elements()));
    return reference(symbol);
  }
  // Prepare scalar broadcasts once, fuse elementwise trees, and materialize
  // calls/matrix products before copying. Matrix assignment is safe under aliasing.
  void prepare(E &e, std::vector<S> &prefix) {
    if (!e->type.array()) {
      rewrite(e, prefix);
      if (!e->constant) {
        auto symbol = temporary(e->type); prefix.push_back(declare(symbol, std::move(e))); e = reference(symbol);
      }
      return;
    }
    if (e->kind == Expr::Kind::Variable) {
      for (auto &index : e->operands) prepare(index, prefix);
      return;
    }
    if (e->kind == Expr::Kind::Call) { rewrite(e, prefix); return; }
    for (auto &operand : e->operands) prepare(operand, prefix);
    if (e->kind == Expr::Kind::Binary && e->text == "@") {
      for (auto &operand : e->operands)
        if (operand->kind != Expr::Kind::Variable) operand = materialize(std::move(operand), prefix);
      auto result = temporary(e->type); prefix.push_back(declare(result));
      if (e->schedule < 0) throw std::logic_error("tensor product has no schedule");
      const auto &schedule = program.schedules.at(e->schedule);
      prefix.push_back(schedule.uniform ? uniformMatmul(result, *e->operands[0], *e->operands[1]) : matmul(result, *e->operands[0], *e->operands[1],schedule.columnTile)); e = reference(result);
    }
  }
  void rewrite(E &e, std::vector<S> &prefix) {
    if (!e) return;
    if (e->kind == Expr::Kind::Binary && (e->text == "&&" || e->text == "||")) {
      // Materialized calls on the right remain behind the short-circuit branch.
      if (e->constant) return;
      std::vector<S> right;
      rewrite(e->operands[0], prefix); rewrite(e->operands[1], right);
      if (right.empty()) return;
      auto result = temporary({Base::Int, {}}); prefix.push_back(declare(result));
      auto rhs = block();
      for (auto &before : right) rhs->children.push_back(std::move(before));
      rhs->children.push_back(assign(reference(result), boolean(std::move(e->operands[1]))));
      auto branch = program.statements->make(Stmt::Kind::If, e->location);
      branch->expression = std::move(e->operands[0]);
      if (e->text == "&&") {
        branch->children.push_back(std::move(rhs)); branch->children.push_back(assign(reference(result), integer(0)));
      } else {
        branch->children.push_back(assign(reference(result), integer(1))); branch->children.push_back(std::move(rhs));
      }
      prefix.push_back(std::move(branch)); e = reference(result); return;
    }
    if (e->kind == Expr::Kind::Call) {
      for (auto &arg : e->operands) {
        if (arg->type.array() && arg->type.tensor) {
          prepare(arg, prefix);
          if (arg->kind != Expr::Kind::Variable) arg = materialize(std::move(arg), prefix);
        } else rewrite(arg, prefix);
      }
      if (e->callee->tensorResult) {
        auto result = temporary(e->type); prefix.push_back(declare(result));
        e->operands.push_back(reference(result)); e->type = {Base::Void, {}};
        prefix.push_back(eval(std::move(e))); e = reference(result);
      }
      return;
    }
    std::vector<E *> work;
    for (auto it = e->operands.rbegin(); it != e->operands.rend(); ++it) work.push_back(&*it);
    while (!work.empty()) {
      E &child = *work.back(); work.pop_back();
      if (child->kind == Expr::Kind::Call || (child->kind == Expr::Kind::Binary && (child->text == "&&" || child->text == "||"))) rewrite(child,prefix);
      else for (auto it = child->operands.rbegin(); it != child->operands.rend(); ++it) work.push_back(&*it);
    }
  }
  void initializer(Initializer &init, std::vector<S> &prefix, std::unordered_map<Expr *, Expr *> &replacements) {
    if (init.value) {
      auto old = init.value.get(); rewrite(init.value, prefix); replacements[old] = init.value.get();
    }
    for (auto &child : init.children) initializer(child, prefix, replacements);
  }
  void statement(S &s) {
    if (s->kind == Stmt::Kind::Block) { for (auto &child : s->children) statement(child); return; }
    if (s->kind == Stmt::Kind::Declaration) {
      auto replacement = block();
      for (auto &v : s->variables) {
        std::vector<S> prefix;
        std::unordered_map<Expr *, Expr *> replacements;
        if (v.initializer) initializer(*v.initializer, prefix, replacements);
        for (auto &entry : v.initializers) entry.second = replacements.at(entry.second);
        for (auto &before : prefix) replacement->children.push_back(std::move(before));
        auto d = program.statements->make(Stmt::Kind::Declaration, v.location);
        d->variables.push_back(std::move(v)); replacement->children.push_back(std::move(d));
      }
      s = std::move(replacement); return;
    }
    for (auto &child : s->children) statement(child);
    std::vector<S> prefix;
    if ((s->kind == Stmt::Kind::Assignment && s->target->type.tensor && s->target->type.array()) ||
        (s->kind == Stmt::Kind::Return && current->tensorResult)) {
      E target = s->kind == Stmt::Kind::Return ? reference(results.at(current)) : std::move(s->target);
      // Freeze destination indices before evaluating the right-hand side.
      for (auto &index : target->operands) prepare(index, prefix);
      prepare(s->expression, prefix);
      int64_t count = s->kind == Stmt::Kind::Return ? Type{current->result, current->resultShape}.elements() : target->type.elements();
      auto expanded = block();
      for (auto &before : prefix) expanded->children.push_back(std::move(before));
      expanded->children.push_back(copy(std::move(target), std::move(s->expression), count));
      if (s->kind == Stmt::Kind::Return) expanded->children.push_back(program.statements->make(Stmt::Kind::Return, s->location));
      s = std::move(expanded); return;
    }
    rewrite(s->target, prefix); rewrite(s->expression, prefix);
    if (prefix.empty()) return;
    if (s->kind == Stmt::Kind::While) {
      auto guard = program.statements->make(Stmt::Kind::If, s->location);
      guard->expression = program.expressions->make(Expr::Kind::Unary, s->location, "!");
      guard->expression->operands.push_back(std::move(s->expression));
      guard->children.push_back(program.statements->make(Stmt::Kind::Break, s->location));
      auto body = block();
      for (auto &before : prefix) body->children.push_back(std::move(before));
      body->children.push_back(std::move(guard)); body->children.push_back(std::move(s->children[0]));
      s->children[0] = std::move(body); s->expression = integer(1);
    } else {
      auto expanded = block();
      for (auto &before : prefix) expanded->children.push_back(std::move(before));
      expanded->children.push_back(std::move(s)); s = std::move(expanded);
    }
  }
public:
  explicit TensorExpansion(ProgramIR &program) : NodeBuilder(program) {}
  void run() {
    for (auto &item : program.items) if (item.function && item.function->tensorResult) {
      auto &f = *item.function;
      Type type{f.result, f.resultShape, true}; type.shape[0] = -1;
      auto result = temporary(type); result->parameter = true;
      Variable parameter; parameter.name = result->name; parameter.base = f.result; parameter.symbol = result;
      parameter.arrayParameter = parameter.tensor = true;
      f.parameters.push_back(std::move(parameter)); results[&f] = result;
    }
    for (auto &item : program.items) if (item.function) { current = item.function.get(); statement(current->body); }
    for (auto &item : program.items) if (item.function && item.function->tensorResult) item.function->result = Base::Void;
  }
};

// Track uniform rows/columns while tensors still carry shape and producer
// information. Floating-point specialization requires a proof of exact dyadic
// products and sums; arbitrary float matrices retain the sequential reduction.
class TensorStructure {
  struct Dyadic { uint64_t bound; int exponent; };
  struct Fact { bool rows = false, columns = false; std::optional<Dyadic> number; };
  using Known = std::map<const Symbol *, Number>;
  struct State { Known known; std::map<const Symbol *, Fact> arrays; };
  struct Changes { std::set<const Symbol *> written; bool control = false, call = false; };
  struct Counter { const Symbol *symbol; int32_t extent; };
  static constexpr uint64_t precision = uint64_t{1} << 24;

  static void collect(const Stmt &s, Changes &c) {
    if (s.kind == Stmt::Kind::Break || s.kind == Stmt::Kind::Continue || s.kind == Stmt::Kind::Return || s.kind == Stmt::Kind::If) c.control = true;
    if (s.target && s.target->symbol) c.written.insert(s.target->symbol);
    for (const auto &v : s.variables) c.written.insert(v.symbol);
    std::function<void(const Expr &)> expr = [&](const Expr &e) { c.call |= e.kind == Expr::Kind::Call; for (const auto &a : e.operands) expr(*a); };
    if (s.expression) expr(*s.expression);
    if (s.target) expr(*s.target);
    for (const auto &v : s.variables) for (auto init : v.initializers) expr(*init.second);
    for (const auto &child : s.children) collect(*child, c);
  }
  static std::optional<Counter> counter(const Stmt &s, const Known &known) {
    if (s.kind != Stmt::Kind::While || s.expression->kind != Expr::Kind::Binary || s.expression->text != "<" ||
        s.children[0]->kind != Stmt::Kind::Block || s.children[0]->children.empty()) return {};
    const auto &a = *s.expression->operands[0], &b = *s.expression->operands[1];
    if (a.kind != Expr::Kind::Variable || !a.operands.empty() || a.type.base != Base::Int || !b.constant || b.constant->base != Base::Int || b.constant->integer < 0) return {};
    auto init = known.find(a.symbol);
    if (init == known.end() || init->second.base != Base::Int || init->second.integer != 0) return {};
    const auto &last = *s.children[0]->children.back();
    if (last.kind != Stmt::Kind::Assignment || last.target->symbol != a.symbol || last.expression->kind != Expr::Kind::Binary || last.expression->text != "+") return {};
    const auto &x = *last.expression->operands[0], &y = *last.expression->operands[1];
    if (x.kind != Expr::Kind::Variable || x.symbol != a.symbol || !y.constant || y.constant->base != Base::Int || y.constant->integer != 1) return {};
    int count = 0;
    std::function<void(const Stmt &)> writes = [&](const Stmt &n) { if (n.target && n.target->symbol == a.symbol) ++count; for (const auto &c : n.children) writes(*c); };
    writes(*s.children[0]); if (count != 1) return {};
    return Counter{a.symbol, b.constant->integer};
  }
  static std::optional<Dyadic> literal(Number n) {
    if (n.base == Base::Int) return Dyadic{static_cast<uint64_t>(n.integer < 0 ? -static_cast<int64_t>(n.integer) : n.integer), 0};
    if (!std::isfinite(n.floating)) return {};
    uint32_t raw; std::memcpy(&raw, &n.floating, 4); raw &= 0x7fffffff;
    int exponent = (raw >> 23) ? static_cast<int>(raw >> 23) - 150 : -149;
    uint64_t mantissa = (raw & 0x7fffff) | ((raw >> 23) ? 0x800000 : 0);
    if (!mantissa) return Dyadic{0, 0};
    while (!(mantissa & 1)) { mantissa >>= 1; ++exponent; }
    return Dyadic{mantissa, exponent};
  }
  static bool representable(Dyadic n) {
    if (!n.bound) return true;
    int high = 0; for (auto v = n.bound; v > 1; v >>= 1) ++high;
    return n.bound <= precision && n.exponent >= -149 && high + n.exponent <= 127;
  }
  static std::optional<Dyadic> number(const Expr &e, const Known &known, const std::map<const Symbol *, int32_t> &bounds) {
    if (e.constant) return literal(*e.constant);
    if (e.kind == Expr::Kind::Variable && !e.type.array()) {
      if (bounds.count(e.symbol)) return Dyadic{static_cast<uint64_t>(std::max(0, bounds.at(e.symbol)-1)), 0};
      if (known.count(e.symbol)) return literal(known.at(e.symbol));
      if (e.type.base == Base::Int) return Dyadic{uint64_t{1} << 31, 0};
      return {};
    }
    if (e.kind == Expr::Kind::Unary && e.text != "!") return number(*e.operands[0], known, bounds);
    if (e.kind != Expr::Kind::Binary) return {};
    auto a = number(*e.operands[0], known, bounds), b = number(*e.operands[1], known, bounds);
    if (e.type.base == Base::Int) {
      if (e.text == "%" && e.operands[1]->constant && e.operands[1]->constant->integer != 0) {
        int64_t d = e.operands[1]->constant->integer; return Dyadic{static_cast<uint64_t>(d < 0 ? -d : d)-1, 0};
      }
      uint64_t full = uint64_t{1} << 31;
      if (a && b && (e.text == "+" || e.text == "-")) return Dyadic{std::min(full, a->bound+b->bound), 0};
      if (a && b && e.text == "*") return Dyadic{std::min(full, a->bound*b->bound), 0};
      return Dyadic{full, 0};
    }
    if (!a || !b || a->bound > precision || b->bound > precision) return {};
    Dyadic result{0, 0};
    if (e.text == "*") result = {a->bound*b->bound, a->exponent+b->exponent};
    else if (e.text == "+" || e.text == "-") {
      result.exponent = std::min(a->exponent, b->exponent);
      int x = a->exponent-result.exponent, y = b->exponent-result.exponent;
      if (x > 24 || y > 24) return {};
      result.bound = (a->bound << x) + (b->bound << y);
    } else return {};
    return representable(result) ? std::optional<Dyadic>(result) : std::nullopt;
  }
  static bool independent(const Expr &e, const Changes &c, const Symbol *allowed) {
    if (e.kind == Expr::Kind::Call || (e.symbol && c.written.count(e.symbol) && e.symbol != allowed)) return false;
    for (const auto &a : e.operands) if (!independent(*a, c, allowed)) return false;
    return true;
  }
  static std::map<const Symbol *, Fact> producer(const Stmt &s, const State &state) {
    std::map<const Symbol *, Fact> result; auto outer = counter(s, state.known);
    Changes changes; collect(*s.children[0], changes);
    if (!outer || changes.call || changes.control) return result;
    Known known = state.known; const Stmt *inner = nullptr; std::optional<Counter> column;
    for (const auto &child : s.children[0]->children) {
      if (child->kind == Stmt::Kind::Declaration) for (const auto &v : child->variables)
        if (v.initializers.size() == 1 && v.initializers[0].second->constant) known[v.symbol] = *v.initializers[0].second->constant;
      if (child->kind == Stmt::Kind::While) { if (inner) return {}; inner = child.get(); column = counter(*inner, known); }
    }
    if (!inner || !column) return result;
    std::set<const Symbol *> invalid;
    for (auto symbol : changes.written) known.erase(symbol);
    std::map<const Symbol *, int32_t> bounds{{outer->symbol,outer->extent},{column->symbol,column->extent}};
    std::function<void(const Stmt &, bool)> scan = [&](const Stmt &n, bool inside) {
      if (n.kind == Stmt::Kind::While && &n != inner) return;
      if (&n == inner) inside = true;
      if (n.target && n.target->symbol && n.target->symbol->type.array()) {
        auto symbol = n.target->symbol; const auto &target = *n.target;
        bool full = inside && symbol->type.shape == std::vector<int64_t>{outer->extent, column->extent} && target.operands.size() == 2 &&
            target.operands[0]->kind == Expr::Kind::Variable && target.operands[0]->symbol == outer->symbol &&
            target.operands[1]->kind == Expr::Kind::Variable && target.operands[1]->symbol == column->symbol;
        if (!full) { invalid.insert(symbol); return; }
        Fact fact{independent(*n.expression, changes, outer->symbol), independent(*n.expression, changes, column->symbol), number(*n.expression, known, bounds)};
        if (symbol->type.base == Base::Float && fact.number && !representable(*fact.number)) fact.number.reset();
        auto old = result.find(symbol);
        if (old != result.end()) { fact.rows &= old->second.rows; fact.columns &= old->second.columns; fact.number.reset(); }
        result[symbol] = fact;
      }
      for (const auto &child : n.children) scan(*child, inside);
    };
    scan(*s.children[0], false);
    for (auto symbol : invalid) result.erase(symbol);
    return result;
  }
  static void expr(Expr &e, State &state) { visitExpressions(e,[&](Expr &node) { expressionNode(node,state); }); }
  static void expressionNode(Expr &e, State &state) {
    if (e.kind == Expr::Kind::Call) {
      if (!e.callee->runtime) {
        state.arrays.clear();
        for (auto it = state.known.begin(); it != state.known.end();) { if (it->first->global) it = state.known.erase(it); else ++it; }
      }
      else if (e.text == "getarray" || e.text == "getfarray") {
        if (e.operands[0]->symbol) state.arrays.erase(e.operands[0]->symbol); else state.arrays.clear();
      }
    }
    if (e.kind != Expr::Kind::Binary || e.text != "@") return;
    auto &a = *e.operands[0], &b = *e.operands[1];
    if (a.kind != Expr::Kind::Variable || b.kind != Expr::Kind::Variable || !a.operands.empty() || !b.operands.empty() || a.type.shape[1] <= 0) return;
    auto x = state.arrays.find(a.symbol), y = state.arrays.find(b.symbol);
    if (x == state.arrays.end() || y == state.arrays.end() || !x->second.rows || !y->second.columns) return;
    if (e.type.base == Base::Int) { e.uniformProduct = true; return; }
    auto p = x->second.number, q = y->second.number;
    if (!p || !q || p->bound > precision || q->bound > precision) return;
    uint64_t product = p->bound*q->bound, length = a.type.shape[1];
    if (product > precision/length) return;
    e.uniformProduct = representable({product*length, p->exponent+q->exponent});
  }
  static void walk(Stmt &s, State &state) {
    if (s.kind == Stmt::Kind::Block) { for (auto &child : s.children) walk(*child, state); return; }
    if (s.target) expr(*s.target, state);
    if (s.expression) expr(*s.expression, state);
    if (s.kind == Stmt::Kind::Declaration) for (auto &v : s.variables) {
      for (auto init : v.initializers) expr(*init.second, state);
      if (v.symbol->type.numeric() && v.initializers.size() == 1 && v.initializers[0].second->constant) state.known[v.symbol] = convert(*v.initializers[0].second->constant, v.base, v.location);
    }
    if (s.kind == Stmt::Kind::Assignment) {
      state.arrays.erase(s.target->symbol); state.known.erase(s.target->symbol);
      if (s.target->type.numeric() && !s.target->symbol->type.array() && s.expression->constant) state.known[s.target->symbol] = convert(*s.expression->constant, s.target->type.base, s.location);
    }
    if (s.kind == Stmt::Kind::If) {
      State yes = state, no = state; walk(*s.children[0], yes); if (s.children.size() == 2) walk(*s.children[1], no);
      state.arrays.clear(); state.known.clear();
      for (auto entry : yes.arrays) if (no.arrays.count(entry.first)) {
        auto other = no.arrays.at(entry.first); entry.second.rows &= other.rows; entry.second.columns &= other.columns;
        entry.second.number.reset(); state.arrays.insert(entry);
      }
      for (auto entry : yes.known) if (no.known.count(entry.first) && entry.second.base == Base::Int && no.known.at(entry.first).base == Base::Int && entry.second.integer == no.known.at(entry.first).integer) state.known.insert(entry);
    }
    if (s.kind == Stmt::Kind::While) {
      auto inferred = producer(s, state); Changes changes; collect(*s.children[0], changes);
      for (auto symbol : changes.written) { state.arrays.erase(symbol); state.known.erase(symbol); }
      if (changes.call) {
        state.arrays.clear();
        for (auto it = state.known.begin(); it != state.known.end();) { if (it->first->global) it = state.known.erase(it); else ++it; }
      }
      auto body = state; walk(*s.children[0], body);
      for (auto it = state.arrays.begin(); it != state.arrays.end();) { if (!body.arrays.count(it->first)) it = state.arrays.erase(it); else ++it; }
      for (auto entry : inferred) state.arrays[entry.first] = entry.second;
    }
  }
public:
  static void run(ProgramIR &program) { for (auto &item : program.items) if (item.function) { State state; walk(*item.function->body, state); } }
};

// Bottom-up abstract interpretation of small, effect-free scalar functions.
// Compose callees' transfer functions over Z/(2^32), including unused operands
// in the purity check. Lowering preserves argument evaluation before replacing
// a call, so a constant result never removes its caller's side effects.
class ScalarFunctions {
  using Form = Function::AffineReturn;
  std::map<Function *,int> state;
  int depth = 0;
  std::optional<Form> expression(const Expr &e, Function &f, size_t &budget) {
    if (!budget || e.type.base != Base::Int || e.type.array()) return {};
    --budget;
    Form a; a.coefficients.resize(f.parameters.size());
    if (e.constant && e.constant->base == Base::Int) { a.offset = e.constant->integer; return a; }
    if (e.kind == Expr::Kind::Variable && e.operands.empty()) {
      for (size_t n = 0; n < f.parameters.size(); ++n) if (e.symbol == f.parameters[n].symbol) {
        a.coefficients[n] = 1; return a;
      }
      return {};
    }
    auto scale = [](Form &x,uint32_t k) { x.offset *= k; for (auto &c : x.coefficients) c *= k; };
    auto add = [](Form &x,const Form &y) { x.offset += y.offset; for (size_t n = 0; n < x.coefficients.size(); ++n) x.coefficients[n] += y.coefficients[n]; };
    if (e.kind == Expr::Kind::Call) {
      analyze(*e.callee);
      if (!e.callee->affineReturn) return {};
      const auto &callee = *e.callee->affineReturn; a.offset = callee.offset;
      for (size_t n = 0; n < e.operands.size(); ++n) {
        auto argument = expression(*e.operands[n],f,budget); if (!argument) return {};
        scale(*argument,callee.coefficients[n]); add(a,*argument);
      }
      return a;
    }
    if (e.kind != Expr::Kind::Unary && e.kind != Expr::Kind::Binary) return {};
    auto left = expression(*e.operands[0],f,budget); if (!left) return {};
    if (e.kind == Expr::Kind::Unary) {
      if (e.text == "-") scale(*left,UINT32_MAX);
      else if (e.text != "+") return {};
      return left;
    }
    auto right = expression(*e.operands[1],f,budget); if (!right) return {};
    if (e.text == "+" || e.text == "-") {
      if (e.text == "-") scale(*right,UINT32_MAX);
      add(*left,*right); return left;
    }
    if (e.text == "*") {
      auto constant = [](const Form &x) { return std::all_of(x.coefficients.begin(),x.coefficients.end(),[](uint32_t c) { return !c; }); };
      if (constant(*left)) { scale(*right,left->offset); return right; }
      if (constant(*right)) { scale(*left,right->offset); return left; }
    }
    return {};
  }
  void analyze(Function &f) {
    if (state[&f]) return;
    state[&f] = 1;
    if (depth >= 32 || f.runtime || f.tensorResult || f.result != Base::Int || f.parameters.size() > 8 ||
        !f.body || f.body->kind != Stmt::Kind::Block || f.body->children.size() != 1) return;
    const auto &s = *f.body->children[0];
    if (s.kind != Stmt::Kind::Return || !s.expression) return;
    size_t budget = 64; ++depth; f.affineReturn = expression(*s.expression,f,budget); --depth;
  }
public:
  void run(ProgramIR &program) { for (auto &item : program.items) if (item.function) analyze(*item.function); }
};

// Close a positive additive modular recurrence only when a runtime guard proves
// that all additions and the combined product stay in the signed int32 range.
// Negative, wrapping and resumed executions retain the original loop.
class IntegerLoops : NodeBuilder {
  std::set<const Symbol *> zero;
  std::map<const Symbol *,int> writes;
  const HighIROptions &options;
  static bool ref(const Expr &e,const Symbol *s) { return e.kind == Expr::Kind::Variable && e.symbol == s && e.operands.empty(); }
  static std::optional<int32_t> value(const Expr &e) {
    if (e.constant && e.constant->base == Base::Int) return e.constant->integer;
    return {};
  }
  void facts(const Stmt &s) {
    for (const auto &v : s.variables)
      if (v.base == Base::Int && !v.symbol->type.array() && v.initializers.size() == 1 &&
          value(*v.initializers[0].second) == std::optional<int32_t>(0)) zero.insert(v.symbol);
    if (s.kind == Stmt::Kind::Assignment) ++writes[s.target->symbol];
    for (const auto &c : s.children) facts(*c);
  }
  void walk(S &s) {
    for (auto &c : s->children) walk(c);
    if (s->kind != Stmt::Kind::While || s->expression->kind != Expr::Kind::Binary || s->expression->text != "<" ||
        s->children[0]->kind != Stmt::Kind::Block || s->children[0]->children.size() < 2 || s->children[0]->children.size() > 3) return;
    const auto &left = *s->expression->operands[0], &bound = *s->expression->operands[1]; auto counter = left.symbol;
    if (!counter || !ref(left,counter) || left.type.base != Base::Int || !zero.count(counter) || writes[counter] != 1 ||
        bound.type.base != Base::Int || !bound.type.numeric() ||
        !(bound.kind == Expr::Kind::Number || (bound.kind == Expr::Kind::Variable && bound.operands.empty()))) return;
    const auto &body = *s->children[0];
    if (body.children.size() == 2) {
      const auto &a = *body.children[0], &step = *body.children[1]; auto state = a.target ? a.target->symbol : nullptr;
      if (!options.quotientLoops || a.kind != Stmt::Kind::Assignment || step.kind != Stmt::Kind::Assignment ||
          !state || state == counter || state == bound.symbol || !state->type.numeric() || state->type.base != Base::Int ||
          !ref(*a.target,state) || !ref(*step.target,counter) || a.expression->kind != Expr::Kind::Binary || a.expression->text != "/" ||
          !ref(*a.expression->operands[0],state) || step.expression->kind != Expr::Kind::Binary || step.expression->text != "+" ||
          !ref(*step.expression->operands[0],counter) || value(*step.expression->operands[1]) != std::optional<int32_t>(1)) return;
      auto divisor = value(*a.expression->operands[1]);
      if (!divisor || *divisor < 2 || (*divisor & (*divisor-1))) return;
      int shift = 0; for (auto d = *divisor; d > 1; d /= 2) ++shift;
      auto factor = binaryNode("*",clone(bound),integer(shift));
      auto negative = program.expressions->make(Expr::Kind::Unary,Location{},"-");
      negative->type.base = Base::Int; negative->operands.push_back(reference(state));
      auto quotient = binaryNode("shr",std::move(negative),clone(*factor));
      auto restore = program.expressions->make(Expr::Kind::Unary,Location{},"-");
      restore->type.base = Base::Int; restore->operands.push_back(std::move(quotient));
      auto sign = program.statements->make(Stmt::Kind::If,Location{});
      sign->expression = binaryNode("<",reference(state),integer(0));
      sign->children = {assign(reference(state),std::move(restore)),assign(reference(state),binaryNode("shr",reference(state),std::move(factor)))};
      auto saturated = program.statements->make(Stmt::Kind::If,Location{});
      saturated->expression = binaryNode(">=",clone(bound),integer((32+shift-1)/shift));
      saturated->children = {assign(reference(state),integer(0)),std::move(sign)};
      auto once = block(); once->children = {std::move(saturated),assign(reference(counter),clone(bound))};
      auto version = program.statements->make(Stmt::Kind::If,s->location);
      version->expression = binaryNode("&&",clone(*s->expression),binaryNode("==",reference(counter),integer(0)));
      version->children = {std::move(once),s}; s = std::move(version); return;
    }
    if (!options.modularLoops) return;
    const auto &a = *body.children[0], &r = *body.children[1], &step = *body.children[2];
    if (a.kind != Stmt::Kind::Assignment || r.kind != Stmt::Kind::Assignment || step.kind != Stmt::Kind::Assignment) return;
    auto state = a.target->symbol;
    if (!state || state == counter || state == bound.symbol || !state->type.numeric() || state->type.base != Base::Int ||
        !ref(*a.target,state) || !ref(*r.target,state) || !ref(*step.target,counter)) return;
    auto binary = [](const Expr &e,std::string_view op) { return e.kind == Expr::Kind::Binary && e.text == op; };
    if (!binary(*a.expression,"+") || !binary(*r.expression,"%") || !binary(*step.expression,"+") ||
        !ref(*r.expression->operands[0],state) || !ref(*step.expression->operands[0],counter) ||
        value(*step.expression->operands[1]) != std::optional<int32_t>(1)) return;
    int side = ref(*a.expression->operands[0],state) ? 1 : ref(*a.expression->operands[1],state) ? 0 : -1;
    if (side < 0) return;
    auto delta = value(*a.expression->operands[side]), modulus = value(*r.expression->operands[1]);
    if (!delta || !modulus || *delta <= 0 || *modulus <= 0 || int64_t(*modulus)-1+*delta > INT32_MAX) return;
    auto amount = binaryNode("*",integer(*delta),clone(bound));
    auto guard = binaryNode("&&",clone(*s->expression),binaryNode("&&",binaryNode("==",reference(counter),integer(0)),
        binaryNode("&&",binaryNode("<=",clone(bound),integer(INT32_MAX / *delta)),
        binaryNode("&&",binaryNode(">=",reference(state),integer(0)),
            binaryNode("<=",reference(state),binaryNode("-",integer(INT32_MAX),clone(*amount)))))));
    auto closed = block();
    closed->children.push_back(assign(reference(state),binaryNode("%",binaryNode("+",reference(state),std::move(amount)),integer(*modulus))));
    closed->children.push_back(assign(reference(counter),clone(bound)));
    auto version = program.statements->make(Stmt::Kind::If,s->location);
    version->expression = std::move(guard); version->children = {std::move(closed),s}; s = std::move(version);
  }
  E binaryNode(std::string op,E a,E b) { return NodeBuilder::binary(std::move(op),std::move(a),std::move(b)); }
public:
  IntegerLoops(ProgramIR &p,const HighIROptions &o) : NodeBuilder(p), options(o) {}
  void run() { for (auto &item : program.items) if (item.function) { zero.clear();writes.clear();facts(*item.function->body);walk(item.function->body); } }
};

// Recognize the complete recursive integer bitonic network, not a function
// name or a checksum. Its comparator, merge and sorter must match every effect.
// On a bounded power-of-two interval the network establishes a total order;
// applying the same order again is the identity. Other sizes use the original.
class SortingNetworks {
  using Functions = std::map<const Function *,const Symbol *>;
  static const Stmt *single(const Stmt *s) {
    while (s && s->kind == Stmt::Kind::Block && s->children.size() == 1) s = s->children[0].get();
    return s;
  }
  static bool variable(const Expr &e,const Symbol *s) { return e.kind == Expr::Kind::Variable && e.symbol == s && e.operands.empty(); }
  static bool literal(const Expr &e,int n) { return e.constant && e.constant->base == Base::Int && e.constant->integer == n; }
  static bool binary(const Expr &e,std::string_view op) { return e.kind == Expr::Kind::Binary && e.text == op && e.operands.size() == 2; }
  static bool add(const Expr &e,const Symbol *a,const Symbol *b) {
    return binary(e,"+") && ((variable(*e.operands[0],a) && variable(*e.operands[1],b)) ||
                             (variable(*e.operands[1],a) && variable(*e.operands[0],b)));
  }
  static const Symbol *declaration(const Stmt &s,const Expr *&init) {
    const auto *d = single(&s);
    if (d->kind != Stmt::Kind::Declaration || d->variables.size() != 1 || d->variables[0].base != Base::Int ||
        d->variables[0].symbol->type.array() || d->variables[0].initializers.size() != 1) return nullptr;
    init = d->variables[0].initializers[0].second; return d->variables[0].symbol;
  }
  static const Expr *call(const Stmt &s,const Function *callee) {
    if (s.kind != Stmt::Kind::Expression || !s.expression || s.expression->kind != Expr::Kind::Call ||
        s.expression->operands.size() != 3 || (callee && s.expression->callee != callee)) return nullptr;
    return s.expression.get();
  }
  static const Stmt *body(const Function &f) {
    if (f.runtime || f.tensorResult || f.result != Base::Void || f.parameters.size() != 3) return nullptr;
    for (const auto &p : f.parameters) if (p.base != Base::Int || p.symbol->type.array()) return nullptr;
    return single(f.body.get());
  }
  static bool element(const Expr &e,const Symbol *array,const Symbol *index) {
    return e.kind == Expr::Kind::Variable && e.symbol == array && e.operands.size() == 1 && variable(*e.operands[0],index);
  }
  static const Symbol *swap(const Stmt *s,const Symbol *i,const Symbol *j,std::string_view op) {
    s = single(s);
    if (!s || s->kind != Stmt::Kind::If || s->children.size() != 1 || !binary(*s->expression,op)) return nullptr;
    const auto &left = *s->expression->operands[0], &right = *s->expression->operands[1];
    auto array = left.symbol;
    if (!array || !array->global || array->isConst || array->type.base != Base::Int || array->type.shape.size() != 1 ||
        !element(left,array,i) || !element(right,array,j)) return nullptr;
    const auto &block = *s->children[0];
    if (block.kind != Stmt::Kind::Block || block.children.size() != 3) return nullptr;
    const Expr *initial = nullptr; auto temp = declaration(*block.children[0],initial);
    if (!temp || !element(*initial,array,i)) return nullptr;
    const auto &a = *block.children[1], &b = *block.children[2];
    if (a.kind != Stmt::Kind::Assignment || b.kind != Stmt::Kind::Assignment || !element(*a.target,array,i) ||
        !element(*a.expression,array,j) || !element(*b.target,array,j) || !variable(*b.expression,temp)) return nullptr;
    return array;
  }
  static bool positive(const Stmt &s,const Symbol *count) {
    return s.kind == Stmt::Kind::If && s.children.size() == 1 && binary(*s.expression,">") &&
        variable(*s.expression->operands[0],count) && literal(*s.expression->operands[1],1);
  }
public:
  Functions run(const ProgramIR &program) {
    Functions comparators,mergers,sorters;
    for (const auto &item : program.items) if (item.function) {
      const auto &f = *item.function; auto s = body(f); if (!s) continue;
      auto i = f.parameters[0].symbol, j = f.parameters[1].symbol, d = f.parameters[2].symbol;
      if (s->kind != Stmt::Kind::If || s->children.size() != 2 || !binary(*s->expression,"==") ||
          !variable(*s->expression->operands[0],d) || !literal(*s->expression->operands[1],1)) continue;
      auto a = swap(s->children[0].get(),i,j,">");
      if (a && swap(s->children[1].get(),i,j,"<") == a) comparators[&f] = a;
    }
    for (const auto &item : program.items) if (item.function) {
      const auto &f = *item.function; auto s = body(f); if (!s) continue;
      auto low = f.parameters[0].symbol, count = f.parameters[1].symbol, dir = f.parameters[2].symbol;
      if (!positive(*s,count)) continue;
      const auto &b = *s->children[0]; if (b.kind != Stmt::Kind::Block || b.children.size() != 5) continue;
      const Expr *initial = nullptr; auto half = declaration(*b.children[0],initial);
      if (!half || !binary(*initial,"/") || !variable(*initial->operands[0],count) || !literal(*initial->operands[1],2)) continue;
      auto index = declaration(*b.children[1],initial); if (!index || !variable(*initial,low)) continue;
      const auto &loop = *b.children[2];
      if (loop.kind != Stmt::Kind::While || !binary(*loop.expression,"<") || !variable(*loop.expression->operands[0],index) ||
          !add(*loop.expression->operands[1],low,half)) continue;
      const auto &iteration = *loop.children[0];
      if (iteration.kind != Stmt::Kind::Block || iteration.children.size() != 2) continue;
      auto cmp = call(*iteration.children[0],nullptr);
      if (!cmp || !comparators.count(cmp->callee) || !variable(*cmp->operands[0],index) ||
          !add(*cmp->operands[1],index,half) || !variable(*cmp->operands[2],dir)) continue;
      const auto &step = *iteration.children[1];
      if (step.kind != Stmt::Kind::Assignment || !variable(*step.target,index) || !binary(*step.expression,"+") ||
          !variable(*step.expression->operands[0],index) || !literal(*step.expression->operands[1],1)) continue;
      auto first = call(*b.children[3],&f), second = call(*b.children[4],&f);
      if (!first || !second || !variable(*first->operands[0],low) || !add(*second->operands[0],low,half) ||
          !variable(*first->operands[1],half) || !variable(*second->operands[1],half) ||
          !variable(*first->operands[2],dir) || !variable(*second->operands[2],dir)) continue;
      mergers[&f] = comparators.at(cmp->callee);
    }
    for (const auto &item : program.items) if (item.function) {
      const auto &f = *item.function; auto s = body(f); if (!s) continue;
      auto low = f.parameters[0].symbol, count = f.parameters[1].symbol, dir = f.parameters[2].symbol;
      if (!positive(*s,count)) continue;
      const auto &b = *s->children[0]; if (b.kind != Stmt::Kind::Block || b.children.size() != 4) continue;
      const Expr *initial = nullptr; auto half = declaration(*b.children[0],initial);
      if (!half || !binary(*initial,"/") || !variable(*initial->operands[0],count) || !literal(*initial->operands[1],2)) continue;
      auto first = call(*b.children[1],&f), second = call(*b.children[2],&f), merge = call(*b.children[3],nullptr);
      if (!first || !second || !merge || !mergers.count(merge->callee) || !variable(*first->operands[0],low) ||
          !add(*second->operands[0],low,half) || !variable(*first->operands[1],half) || !variable(*second->operands[1],half) ||
          !literal(*first->operands[2],1) || !literal(*second->operands[2],0) || !variable(*merge->operands[0],low) ||
          !variable(*merge->operands[1],count) || !variable(*merge->operands[2],dir)) continue;
      sorters[&f] = mergers.at(merge->callee);
    }
    return sorters;
  }
};

// A rectangular reset is a region kill: later reads of that region observe
// only this iteration's reset and kernel writes. Check every kernel access
// against the reset domain, including triangular loops, before dropping replay.
// This deliberately accepts only two-dimensional, call-free kernels and exact
// zero-based copies. Unsupported control, indices and aliases retain the loop.
class ResetRegions {
  using Symbols = std::set<const Symbol *>;
  struct State { Symbols bounded, starts; };
  const Symbol *bound = nullptr, *output = nullptr;
  std::map<const Symbol *,const Symbol *> objects;
  size_t budget = 4096;
  static bool ref(const Expr &e,const Symbol *s) { return e.kind == Expr::Kind::Variable && e.symbol == s && e.operands.empty(); }
  static bool number(const Expr &e,int n) { return e.constant && e.constant->base == Base::Int && e.constant->integer == n; }
  static bool binary(const Expr &e,std::string_view op) { return e.kind == Expr::Kind::Binary && e.text == op; }
  static bool step(const Stmt &s,const Symbol *i) {
    return s.kind == Stmt::Kind::Assignment && ref(*s.target,i) && binary(*s.expression,"+") &&
        ref(*s.expression->operands[0],i) && number(*s.expression->operands[1],1);
  }
  static int writes(const Stmt &s,const Symbol *i) {
    int n = s.target && s.target->symbol == i ? 1 : 0;
    for (const auto &c : s.children) n += writes(*c,i);
    return n;
  }
  static const Symbol *loop(const Stmt &s,const Symbol *n) {
    if (s.kind != Stmt::Kind::While || !binary(*s.expression,"<") || !ref(*s.expression->operands[1],n) ||
        s.children[0]->kind != Stmt::Kind::Block || s.children[0]->children.empty()) return nullptr;
    auto i = s.expression->operands[0]->symbol;
    if (!i || i->global || i == n || i->type.base != Base::Int || !ref(*s.expression->operands[0],i) ||
        !step(*s.children[0]->children.back(),i) || writes(*s.children[0],i) != 1) return nullptr;
    return i;
  }
  static bool zero(const Stmt &s,const Symbol *i) {
    return s.kind == Stmt::Kind::Assignment && ref(*s.target,i) && number(*s.expression,0);
  }
  static bool cell(const Expr &e,const Symbol *i,const Symbol *j) {
    return e.kind == Expr::Kind::Variable && e.symbol && e.operands.size() == 2 &&
        ref(*e.operands[0],i) && ref(*e.operands[1],j);
  }
  static bool start(const Expr &e,const State &state) {
    if (number(e,0)) return true;
    return binary(e,"+") && number(*e.operands[1],1) &&
        e.operands[0]->kind == Expr::Kind::Variable && e.operands[0]->operands.empty() && state.bounded.count(e.operands[0]->symbol);
  }
  bool expression(const Expr &e,const State &state) {
    if (!budget) return false;
    --budget;
    if (e.kind == Expr::Kind::Call || e.kind == Expr::Kind::Element) return false;
    if (e.kind == Expr::Kind::Variable && e.symbol->type.array()) {
      if (!objects.count(e.symbol) || e.operands.size() != 2) return false;
      for (const auto &index : e.operands)
        if (index->kind != Expr::Kind::Variable || !index->operands.empty() || !state.bounded.count(index->symbol)) return false;
    }
    for (const auto &a : e.operands) if (!expression(*a,state)) return false;
    return true;
  }
  bool statement(const Stmt &s,State &state) {
    if (!budget) return false;
    --budget;
    if (s.kind == Stmt::Kind::Block) {
      for (const auto &c : s.children) if (!statement(*c,state)) return false;
      return true;
    }
    if (s.kind == Stmt::Kind::While) {
      auto i = loop(s,bound); if (!i || !state.starts.count(i)) return false;
      auto body = state; body.bounded.insert(i);
      if (!statement(*s.children[0],body)) return false;
      // Other induction variables may end at n and are no longer valid indices.
      for (auto it = state.starts.begin(); it != state.starts.end();)
        if (writes(*s.children[0],*it) && *it != i) it = state.starts.erase(it); else ++it;
      return true;
    }
    if (s.kind == Stmt::Kind::Declaration) {
      for (const auto &v : s.variables) {
        if (v.symbol->type.array()) return false;
        for (auto initial : v.initializers) {
          if (!expression(*initial.second,state)) return false;
          if (start(*initial.second,state)) state.starts.insert(v.symbol);
        }
      }
      return true;
    }
    if (s.kind != Stmt::Kind::Assignment) return false;
    const auto &target = *s.target;
    if (target.kind != Expr::Kind::Variable || !expression(target,state) || !expression(*s.expression,state)) return false;
    if (target.symbol->type.array()) return objects.count(target.symbol) && objects.at(target.symbol) == output;
    if (target.symbol->global || target.symbol == bound) return false;
    state.starts.erase(target.symbol);
    if (start(*s.expression,state)) state.starts.insert(target.symbol);
    return true;
  }
public:
  struct Domain { const Expr *extent; int64_t capacity; };
  std::optional<Domain> check(const Stmt &body,const Symbol *counter) {
    if (body.children.size() != 4) return {};
    const auto &reset = *body.children[1], &invoke = *body.children[2];
    if (reset.kind != Stmt::Kind::While || !binary(*reset.expression,"<")) return {};
    const auto &extent = *reset.expression->operands[1]; auto n = extent.symbol;
    if (!n || !ref(extent,n) || n == counter || n->type.base != Base::Int || !n->type.numeric()) return {};
    auto i = loop(reset,n); if (!i || i == counter || !zero(*body.children[0],i)) return {};
    const auto &rows = *reset.children[0]; if (rows.children.size() != 3) return {};
    auto j = loop(*rows.children[1],n);
    if (!j || j == i || j == counter || !zero(*rows.children[0],j)) return {};
    const auto &columns = *rows.children[1]->children[0]; if (columns.children.size() != 2) return {};
    const auto &copy = *columns.children[0];
    if (copy.kind != Stmt::Kind::Assignment || !cell(*copy.target,i,j) || !cell(*copy.expression,i,j)) return {};
    auto dst = copy.target->symbol, src = copy.expression->symbol;
    if (dst == src || !dst->global || !src->global || dst->isConst || dst->type.shape.size() != 2 ||
        dst->type.base != src->type.base || dst->type.shape != src->type.shape) return {};
    if (invoke.kind != Stmt::Kind::Expression || invoke.expression->kind != Expr::Kind::Call) return {};
    const auto &call = *invoke.expression; const auto &f = *call.callee;
    if (f.runtime || f.result != Base::Void || f.tensorResult || f.parameters.size() != call.operands.size()) return {};
    bound = nullptr; output = dst;
    for (size_t p = 0; p < f.parameters.size(); ++p) {
      auto symbol = f.parameters[p].symbol; const auto &arg = *call.operands[p];
      if (!symbol->type.array()) {
        if (bound || symbol->type.base != Base::Int || !ref(arg,n)) return {};
        bound = symbol;
      } else {
        if (arg.kind != Expr::Kind::Variable || !arg.operands.empty() || !arg.symbol->global ||
            arg.symbol->type.shape != dst->type.shape || arg.symbol->type.base != symbol->type.base ||
            symbol->type.shape.size() != 2 || symbol->type.shape[1] != dst->type.shape[1]) return {};
        objects[symbol] = arg.symbol;
      }
    }
    State state;
    if (!bound || !statement(*f.body,state)) return {};
    return Domain{&extent,std::min(dst->type.shape[0],dst->type.shape[1])};
  }
};

// A bounded symbolic-domain proof, separate from its replay consumer. The
// reset is a must-write Cartesian region; every subsequent output access must
// lie in it. Loop dimensions retain their parameter and affine displacement,
// rather than reducing everything to unrelated numeric intervals.
class RegionKills {
  using Certificate = Function::ReplayRegion;
  struct Range { int64_t lower; const Symbol *upper; int64_t offset; };
  struct State {
    std::map<const Symbol *,Range> ranges;
    std::set<const Symbol *> defined;
  };
  struct Loop { const Symbol *index, *bound; int offset; };
  Certificate certificate;
  std::map<const Symbol *,size_t> parameters;
  std::vector<Loop> domain;
  std::set<const Symbol *> reads;
  std::function<bool(const Function &)> scalarPure;
  size_t budget = 4096;
  static bool ref(const Expr &e,const Symbol *s) { return e.kind == Expr::Kind::Variable && e.symbol == s && e.operands.empty(); }
  static std::optional<int32_t> number(const Expr &e) {
    if (e.constant && e.constant->base == Base::Int) return e.constant->integer;
    return {};
  }
  static bool binary(const Expr &e,std::string_view op) { return e.kind == Expr::Kind::Binary && e.text == op; }
  static const Stmt &unwrap(const Stmt &s) {
    auto node = &s;
    while (node->kind == Stmt::Kind::Block && node->children.size() == 1) node = node->children[0].get();
    return *node;
  }
  static std::set<const Symbol *> writes(const Stmt &s) {
    std::set<const Symbol *> result;
    std::vector<const Stmt *> pending{&s};
    while (!pending.empty()) {
      auto node = pending.back(); pending.pop_back();
      if (node->target) result.insert(node->target->symbol);
      for (const auto &v : node->variables) result.insert(v.symbol);
      for (const auto &c : node->children) pending.push_back(c.get());
    }
    return result;
  }
  static int assignments(const Stmt &s,const Symbol *i) {
    int n = s.target && s.target->symbol == i ? 1 : 0;
    for (const auto &c : s.children) n += assignments(*c,i);
    return n;
  }
  std::optional<Loop> loop(const Stmt &s,const State &state) {
    if (s.kind != Stmt::Kind::While || (!binary(*s.expression,"<") && !binary(*s.expression,"<=")) ||
        s.children[0]->kind != Stmt::Kind::Block || s.children[0]->children.empty()) return {};
    auto i = s.expression->operands[0]->symbol, n = s.expression->operands[1]->symbol;
    if (!i || i->global || parameters.count(i) || !n || !parameters.count(n) ||
        !ref(*s.expression->operands[0],i) || !ref(*s.expression->operands[1],n) || !state.ranges.count(i)) return {};
    auto start = state.ranges.at(i);
    if (start.upper || start.lower != start.offset || start.lower < 0 || start.lower > INT32_MAX-1) return {};
    const auto &tail = *s.children[0]->children.back();
    if (tail.kind != Stmt::Kind::Assignment || !ref(*tail.target,i) || !binary(*tail.expression,"+") ||
        !ref(*tail.expression->operands[0],i) || number(*tail.expression->operands[1]) != 1 || assignments(*s.children[0],i) != 1) return {};
    return Loop{i,n,s.expression->text == "<" ? -1 : 0};
  }
  std::optional<Range> range(const Expr &e,const State &state) {
    if (auto n = number(e)) return Range{*n,nullptr,*n};
    if (e.kind == Expr::Kind::Variable && e.operands.empty()) {
      if (parameters.count(e.symbol)) return Range{certificate.parameters[parameters.at(e.symbol)].lower,e.symbol,0};
      if (state.ranges.count(e.symbol)) return state.ranges.at(e.symbol);
    }
    if ((binary(e,"+") || binary(e,"-")) && number(*e.operands[1])) {
      auto r = range(*e.operands[0],state); if (!r) return {};
      int64_t delta = *number(*e.operands[1]); if (e.text == "-") delta = -delta;
      r->lower += delta; r->offset += delta;
      int64_t upper = r->offset;
      if (r->upper) upper += certificate.parameters[parameters.at(r->upper)].upper;
      if (r->lower < INT32_MIN || upper > INT32_MAX) return {};
      return r;
    }
    return {};
  }
  bool restrict(const Symbol *p,int64_t lower,int64_t upper) {
    auto &b = certificate.parameters[parameters.at(p)];
    lower = std::max<int64_t>(b.lower,lower); upper = std::min<int64_t>(b.upper,upper);
    if (lower > upper) return false;
    b.lower = lower; b.upper = upper; return true;
  }
  bool access(const Expr &e,const State &state) {
    if (!e.symbol->global || e.operands.size() != e.symbol->type.shape.size()) return false;
    bool output = e.symbol == certificate.output;
    if (!output) reads.insert(e.symbol);
    for (size_t d = 0; d < e.operands.size(); ++d) {
      auto r = range(*e.operands[d],state); if (!r || r->lower < 0) return false;
      if (r->upper) {
        if (!restrict(r->upper,0,e.symbol->type.shape[d]-1-r->offset)) return false;
      } else if (r->offset >= e.symbol->type.shape[d]) return false;
      if (output) {
        if (r->upper) {
          if (r->upper != domain[d].bound || r->offset > domain[d].offset) return false;
        } else if (!restrict(domain[d].bound,r->offset-domain[d].offset,INT32_MAX-1)) return false;
      }
    }
    return true;
  }
  bool expression(const Expr &e,const State &state) {
    if (!budget) return false;
    --budget;
    if (e.kind == Expr::Kind::Element || e.kind == Expr::Kind::String) return false;
    if (e.kind == Expr::Kind::Variable) {
      if (e.symbol->type.array()) { if (!access(e,state)) return false; }
      else if (e.symbol->global) reads.insert(e.symbol);
      else if (!parameters.count(e.symbol) && !state.defined.count(e.symbol)) return false;
    }
    if (e.kind == Expr::Kind::Call && (!e.callee || !scalarPure(*e.callee))) return false;
    for (const auto &a : e.operands) if (!expression(*a,state)) return false;
    return true;
  }
  bool scalar(const Stmt &node,State &state,bool prefix = false) {
    const auto &s = unwrap(node);
    if (s.kind == Stmt::Kind::Declaration) {
      for (const auto &v : s.variables) {
        if (!v.symbol->type.numeric() || v.base != Base::Int || v.initializers.size() > 1) return false;
        state.ranges.erase(v.symbol); state.defined.erase(v.symbol);
        if (!v.initializers.empty()) {
          auto e = v.initializers[0].second;
          if (!expression(*e,state) || (prefix && !number(*e))) return false;
          state.defined.insert(v.symbol); if (auto r = range(*e,state)) state.ranges[v.symbol] = *r;
        }
      }
      return true;
    }
    if (s.kind != Stmt::Kind::Assignment || s.target->kind != Expr::Kind::Variable ||
        s.target->symbol->global || parameters.count(s.target->symbol) || !s.target->type.numeric() ||
        s.target->type.base != Base::Int || !expression(*s.expression,state) || (prefix && !number(*s.expression))) return false;
    auto r = range(*s.expression,state); state.ranges.erase(s.target->symbol); state.defined.insert(s.target->symbol);
    if (r) state.ranges[s.target->symbol] = *r;
    return true;
  }
  bool reset(const Stmt &s,State &state,size_t depth) {
    if (depth >= 3 || !budget) return false;
    --budget;
    auto l = loop(s,state); if (!l || state.ranges.at(l->index).lower != 0) return false;
    for (auto previous : domain) if (l->index == previous.index) return false;
    domain.push_back(*l);
    const auto &body = *s.children[0]; State inner = state;
    inner.ranges[l->index] = {0,l->bound,l->offset};
    bool found = false;
    for (size_t n = 0; n+1 < body.children.size(); ++n) {
      const auto &node = unwrap(*body.children[n]);
      if (found) return false;
      if (node.kind == Stmt::Kind::While) { if (!reset(node,inner,depth+1)) return false; found = true; }
      else if (node.kind == Stmt::Kind::Assignment && node.target->symbol && node.target->symbol->type.array()) {
        auto object = node.target->symbol;
        if (!object->global || object->isConst || object->type.shape.size() != domain.size() ||
            node.target->operands.size() != domain.size() || !node.expression->constant || node.expression->constant->truth()) return false;
        for (size_t d = 0; d < domain.size(); ++d) {
          if (!ref(*node.target->operands[d],domain[d].index) ||
              !restrict(domain[d].bound,-domain[d].offset,object->type.shape[d]-1-domain[d].offset)) return false;
        }
        certificate.output = object; found = true;
      } else if (!scalar(node,inner,true)) return false;
    }
    for (auto object : writes(body)) state.ranges.erase(object);
    return found;
  }
  bool statement(const Stmt &s,State &state) {
    if (!budget) return false;
    --budget;
    if (s.kind == Stmt::Kind::Block) {
      for (const auto &c : s.children) if (!statement(*c,state)) return false;
      return true;
    }
    if (s.kind == Stmt::Kind::While) {
      auto l = loop(s,state); if (!l) return false;
      auto inner = state;
      inner.ranges[l->index] = {state.ranges.at(l->index).lower,l->bound,l->offset};
      const auto &body = *s.children[0];
      for (size_t n = 0; n+1 < body.children.size(); ++n) if (!statement(*body.children[n],inner)) return false;
      for (auto object : writes(body)) state.ranges.erase(object);
      return true;
    }
    if (s.kind == Stmt::Kind::If) {
      if (!expression(*s.expression,state)) return false;
      auto yes = state, no = state;
      if (!statement(*s.children[0],yes) || (s.children.size() == 2 && !statement(*s.children[1],no))) return false;
      state.ranges.clear();
      for (auto entry : yes.ranges) if (no.ranges.count(entry.first)) {
        auto other = no.ranges.at(entry.first), r = entry.second;
        if (r.lower == other.lower && r.upper == other.upper && r.offset == other.offset) state.ranges.insert(entry);
      }
      state.defined.clear();
      for (auto symbol : yes.defined) if (no.defined.count(symbol)) state.defined.insert(symbol);
      return true;
    }
    if (s.kind == Stmt::Kind::Assignment && s.target->symbol && s.target->symbol->type.array())
      return s.target->symbol == certificate.output && expression(*s.target,state) && expression(*s.expression,state);
    return scalar(s,state);
  }
public:
  std::optional<Certificate> check(const Function &f,std::function<bool(const Function &)> pure) {
    if (f.runtime || f.result != Base::Int || f.tensorResult || f.parameters.empty() || f.parameters.size() > 8 ||
        f.body->kind != Stmt::Kind::Block || f.body->children.empty()) return {};
    size_t nodes = 0;
    std::vector<std::pair<const Stmt *,int>> pending{{f.body.get(),0}};
    while (!pending.empty()) {
      auto [s,depth] = pending.back(); pending.pop_back();
      if (++nodes > 4096 || depth > 64) return {};
      for (const auto &c : s->children) pending.push_back({c.get(),depth+1});
      std::vector<std::pair<const Expr *,int>> expressions;
      if (s->expression) expressions.push_back({s->expression.get(),0});
      if (s->target) expressions.push_back({s->target.get(),0});
      for (const auto &v : s->variables) for (auto init : v.initializers) expressions.push_back({init.second,0});
      while (!expressions.empty()) {
        auto [e,d] = expressions.back(); expressions.pop_back();
        if (++nodes > 4096 || d > 64) return {};
        for (const auto &a : e->operands) expressions.push_back({a.get(),d+1});
      }
    }
    scalarPure = std::move(pure); certificate.parameters.resize(f.parameters.size());
    auto written = writes(*f.body);
    for (size_t p = 0; p < f.parameters.size(); ++p) {
      auto symbol = f.parameters[p].symbol;
      if (!symbol->type.numeric() || symbol->type.base != Base::Int || written.count(symbol)) return {};
      parameters[symbol] = p;
    }
    State state; bool killed = false;
    const auto &body = *f.body;
    for (size_t n = 0; n+1 < body.children.size(); ++n) {
      const auto &node = unwrap(*body.children[n]);
      if (!killed && node.kind == Stmt::Kind::While) {
        if (!reset(node,state,0) || !certificate.output) return {};
        // Reset induction variables are defined even when used after the nest.
        for (auto l : domain) state.defined.insert(l.index);
        killed = true;
      } else if (!(killed ? statement(node,state) : scalar(node,state,true))) return {};
    }
    const auto &last = unwrap(*body.children.back());
    if (!killed || last.kind != Stmt::Kind::Return || !last.expression || !expression(*last.expression,state)) return {};
    certificate.reads.assign(reads.begin(),reads.end());
    return certificate;
  }
};

// Bounded integer execution produces an ordered store trace, not an assumed
// final global state. Unknown inputs, calls, array reads and exceptional
// arithmetic reject the trace. Local scalar state is replayed at its exit.
// Evaluate a bounded integer trace without reading mutable memory or executing
// calls. Preserve the ordered stores; export only private scalar exit values.
class ConstantLoops {
public:
  struct Store { const Expr *target; std::vector<int32_t> indices; int32_t value; };
  struct Model { std::vector<Store> stores; std::map<const Symbol *,int32_t> scalars; size_t cost = 1; };
private:
  std::map<const Symbol *,int32_t> values;
  std::set<const Symbol *> written, declared;
  Model model;
  size_t &budget;
  std::optional<int32_t> expression(const Expr &e,int depth = 0) {
    if (!budget || depth > 32 || !e.type.numeric() || e.type.base != Base::Int) return {};
    --budget;
    if (e.constant && e.constant->base == Base::Int) return e.constant->integer;
    if (e.kind == Expr::Kind::Variable && e.operands.empty() && values.count(e.symbol)) return values.at(e.symbol);
    if (e.kind != Expr::Kind::Unary && e.kind != Expr::Kind::Binary) return {};
    auto a = expression(*e.operands[0],depth+1); if (!a) return {};
    if (e.kind == Expr::Kind::Unary) {
      if (e.text == "+") return a;
      if (e.text == "-") return static_cast<int32_t>(0u-uint32_t(*a));
      if (e.text == "!") return !*a;
      return {};
    }
    if (e.text == "&&" && !*a) return 0;
    if (e.text == "||" && *a) return 1;
    auto b = expression(*e.operands[1],depth+1); if (!b) return {};
    uint32_t x = *a, y = *b;
    if (e.text == "+") return static_cast<int32_t>(x+y);
    if (e.text == "-") return static_cast<int32_t>(x-y);
    if (e.text == "*") return static_cast<int32_t>(x*y);
    if (e.text == "/" || e.text == "%") {
      if (!*b) return {};
      if (*a == INT32_MIN && *b == -1) return e.text == "/" ? INT32_MIN : 0;
      return e.text == "/" ? *a / *b : *a % *b;
    }
    if (e.text == "&") return static_cast<int32_t>(x&y);
    if (e.text == "shr") return static_cast<int32_t>(x>>(y&31));
    if (e.text == "==") return *a == *b;
    if (e.text == "!=") return *a != *b;
    if (e.text == "<") return *a < *b;
    if (e.text == "<=") return *a <= *b;
    if (e.text == ">") return *a > *b;
    if (e.text == ">=") return *a >= *b;
    if (e.text == "&&") return bool(*a) && bool(*b);
    if (e.text == "||") return bool(*a) || bool(*b);
    return {};
  }
  bool statement(const Stmt &s,int depth = 0) {
    if (!budget || depth > 32) return false;
    --budget;
    if (s.kind == Stmt::Kind::Block) {
      for (const auto &c : s.children) if (!statement(*c,depth+1)) return false;
      return true;
    }
    if (s.kind == Stmt::Kind::If) {
      auto condition = expression(*s.expression); if (!condition) return false;
      if (*condition) return statement(*s.children[0],depth+1);
      return s.children.size() == 1 || statement(*s.children[1],depth+1);
    }
    if (s.kind == Stmt::Kind::While) {
      for (;;) {
        auto condition = expression(*s.expression); if (!condition) return false;
        if (!*condition) return true;
        if (!statement(*s.children[0],depth+1)) return false;
      }
    }
    if (s.kind == Stmt::Kind::Declaration) {
      for (const auto &v : s.variables) {
        if (!v.symbol->type.numeric() || v.base != Base::Int || v.initializers.size() > 1) return false;
        declared.insert(v.symbol); values.erase(v.symbol);
        if (!v.initializers.empty()) {
          auto value = expression(*v.initializers[0].second); if (!value) return false;
          values[v.symbol] = *value;
        } else if (v.initializer) values[v.symbol] = 0;
      }
      return true;
    }
    if (s.kind != Stmt::Kind::Assignment || s.target->kind != Expr::Kind::Variable ||
        !s.target->type.numeric() || s.target->type.base != Base::Int) return false;
    auto value = expression(*s.expression); if (!value) return false;
    auto symbol = s.target->symbol;
    if (!symbol->type.array()) {
      if (symbol->global || !s.target->operands.empty()) return false;
      values[symbol] = *value; written.insert(symbol); return true;
    }
    if (model.stores.size() >= 256 || s.target->operands.size() != symbol->type.shape.size() || s.target->operands.size() > 3) return false;
    Store store{s.target.get(),{},*value};
    for (const auto &index : s.target->operands) {
      auto n = expression(*index); if (!n) return false;
      store.indices.push_back(*n);
    }
    model.cost += 3+store.indices.size(); model.stores.push_back(std::move(store)); return true;
  }
public:
  explicit ConstantLoops(size_t &work) : budget(work) {}
  std::optional<Model> run(const Stmt &loop,const Symbol *counter) {
    values[counter] = 0;
    if (!statement(loop) || model.stores.empty()) return {};
    for (auto symbol : written) if (!declared.count(symbol)) {
      model.scalars[symbol] = values.at(symbol); model.cost += 3;
    }
    return std::move(model);
  }
};

// Factor repeated, read-only loop bodies with integer additive reductions.
// Definite assignment proves that auxiliary state resets each iteration; reads
// of carried reductions, array writes, and impure calls prevent the transform.
class Repetition : NodeBuilder {
  struct Summary { int state = 0; bool pure = false; std::set<const Symbol *> reads; };
  std::map<const Function *, Summary> summaries;
  std::map<const Symbol *, int32_t> initial;
  std::map<const Symbol *, int> assignments;
  std::map<const Function *,const Symbol *> sorters;
  bool resetRegions = true;
  int serial = 0;

  std::optional<std::pair<std::string,int64_t>> comparison(const Expr &e,const Symbol *i) {
    if (e.kind != Expr::Kind::Binary || (e.text != "<" && e.text != "<=" && e.text != ">" && e.text != ">=")) return {};
    auto a = e.operands[0], b = e.operands[1];
    if (a->kind != Expr::Kind::Variable || a->symbol != i || !a->operands.empty() ||
        !b->constant || b->constant->base != Base::Int) return {};
    return std::make_pair(e.text,int64_t(b->constant->integer));
  }
  S partitionBody(const Stmt &source,const Symbol *i,int64_t low,int64_t high) {
    if (source.kind == Stmt::Kind::If) if (auto c = comparison(*source.expression,i)) {
      auto [op,k] = *c;
      bool yes = op == "<" ? high < k : op == "<=" ? high <= k : op == ">" ? low > k : low >= k;
      bool no = op == "<" ? low >= k : op == "<=" ? low > k : op == ">" ? high <= k : high < k;
      if (yes) return partitionBody(*source.children[0],i,low,high);
      if (no) return source.children.size() == 2 ? partitionBody(*source.children[1],i,low,high) : block();
    }
    auto s = program.statements->make(source.kind,source.location); *s = source; s->children.clear();
    if (source.expression) s->expression = clone(*source.expression);
    if (source.target) s->target = clone(*source.target);
    for (const auto &c : source.children) s->children.push_back(partitionBody(*c,i,low,high));
    return s;
  }
  static const Stmt &unwrap(const Stmt &s) {
    auto node = &s;
    while (node->kind == Stmt::Kind::Block && node->children.size() == 1) node = node->children[0].get();
    return *node;
  }
  void smallLoops(S &s,size_t &growth,size_t &evaluation,bool constants,bool unroll) {
    for (auto &child : s->children) smallLoops(child,growth,evaluation,constants,unroll);
    if (s->kind != Stmt::Kind::Block) return;
    for (size_t position = 0; position < s->children.size(); ++position) {
      auto &loop = s->children[position];
      if (loop->kind != Stmt::Kind::While || loop->scheduled || loop->expression->kind != Expr::Kind::Binary ||
          loop->expression->text != "<" || loop->children[0]->kind != Stmt::Kind::Block || loop->children[0]->children.empty()) continue;
      auto &a = *loop->expression->operands[0], &b = *loop->expression->operands[1];
      if (a.kind != Expr::Kind::Variable || !a.operands.empty() || a.symbol->global || !a.type.numeric() || a.type.base != Base::Int ||
          !b.constant || b.constant->base != Base::Int || b.constant->integer < 2 || b.constant->integer > 128) continue;
      auto counter = a.symbol; const auto &body = *loop->children[0];
      auto step = increment(*body.children.back(),counter);
      if (!step || !step->constant || step->constant->base != Base::Int || step->constant->integer != 1) continue;
      size_t nodes = 0; int writes = 0; bool valid = true, nested = false, branches = false, arrayWrites = false;
      std::vector<std::pair<const Stmt *,int>> pending{{&body,0}};
      while (!pending.empty() && valid) {
        auto [node,depth] = pending.back(); pending.pop_back();
        if (++nodes > 1024 || depth > 32 || node->kind == Stmt::Kind::Break || node->kind == Stmt::Kind::Continue || node->kind == Stmt::Kind::Return) { valid = false; break; }
        if (node->target && node->target->symbol == counter) ++writes;
        nested |= node->kind == Stmt::Kind::While;
        branches |= node->kind == Stmt::Kind::If;
        arrayWrites |= node->target && node->target->symbol->type.array();
        auto count = [&](Expr *e) { if (e) visitExpressions(*e,[&](Expr &) { if (++nodes > 1024) valid = false; }); };
        count(node->expression.get()); count(node->target.get());
        for (const auto &v : node->variables) for (auto entry : v.initializers) count(entry.second);
        for (const auto &c : node->children) pending.push_back({c.get(),depth+1});
      }
      if (!valid || writes != 1) continue;
      bool zero = false;
      for (size_t n = position; n && position-n < 8;) {
        const auto &prefix = unwrap(*s->children[--n]);
        if (prefix.kind == Stmt::Kind::Assignment && prefix.target->symbol == counter) {
          zero = prefix.expression->constant && prefix.expression->constant->base == Base::Int && prefix.expression->constant->integer == 0; break;
        }
        bool touched = false;
        if (prefix.kind == Stmt::Kind::Declaration) for (const auto &v : prefix.variables) if (v.symbol == counter) {
          touched = true; zero = v.initializers.size() == 1 && v.initializers[0].second->constant &&
              v.initializers[0].second->constant->base == Base::Int && v.initializers[0].second->constant->integer == 0;
        }
        if (touched) break;
        std::vector<const Stmt *> scan{&prefix};
        size_t visited = 0;
        while (!scan.empty()) {
          if (++visited > 1024) { touched = true; break; }
          auto node = scan.back(); scan.pop_back();
          touched |= node->target && node->target->symbol == counter;
          for (const auto &v : node->variables) touched |= v.symbol == counter;
          for (const auto &c : node->children) scan.push_back(c.get());
        }
        if (touched) break;
      }
      auto expanded = block();
      size_t work = std::min<size_t>(evaluation,4096); auto before = work;
      auto trace = constants ? ConstantLoops(work).run(*loop,counter) : std::optional<ConstantLoops::Model>{};
      evaluation -= before-work;
      if (trace && trace->cost+8 <= growth) {
        growth -= trace->cost+8;
        for (const auto &store : trace->stores) {
          auto target = program.expressions->make(Expr::Kind::Variable,store.target->location,store.target->text);
          target->symbol = store.target->symbol; target->type = store.target->type;
          for (int32_t index : store.indices) target->operands.push_back(integer(index));
          expanded->children.push_back(assign(std::move(target),integer(store.value)));
        }
        for (auto [symbol,value] : trace->scalars) expanded->children.push_back(assign(reference(const_cast<Symbol *>(symbol)),integer(value)));
      } else {
        size_t cost = nodes*(b.constant->integer-(zero ? 1 : 0))+8;
        // Residual loops and branch-heavy writes amplify CFG/inlining costs.
        if (!unroll || nested || (branches && arrayWrites) || b.constant->integer > 8 || cost > 4096 || cost > growth) continue;
        growth -= cost;
        if (!zero) expanded->children.push_back(assign(reference(counter),integer(0)));
        for (int n = 0; n < b.constant->integer; ++n) expanded->children.push_back(partitionBody(body,counter,n,n));
      }
      if (zero) loop = std::move(expanded);
      else {
        auto guard = program.statements->make(Stmt::Kind::If,loop->location);
        guard->expression = binary("==",reference(counter),integer(0)); guard->children = {std::move(expanded),loop}; loop = std::move(guard);
      }
    }
  }
  void partition(S &s,size_t &growth) {
    for (auto &child : s->children) partition(child,growth);
    if (s->kind != Stmt::Kind::While || s->expression->kind != Expr::Kind::Binary || s->expression->text != "<" ||
        s->children[0]->kind != Stmt::Kind::Block || s->children[0]->children.empty()) return;
    auto &a = *s->expression->operands[0], &b = *s->expression->operands[1];
    if (a.kind != Expr::Kind::Variable || !a.operands.empty() || a.symbol->global || a.type.base != Base::Int ||
        !b.constant || b.constant->base != Base::Int || b.constant->integer <= 0 || b.constant->integer > 1024) return;
    auto i = a.symbol; const auto &body = *s->children[0];
    auto step = increment(*body.children.back(),i);
    if (!step || !step->constant || step->constant->base != Base::Int || step->constant->integer != 1) return;
    size_t nodes = 0; int writes = 0; bool valid = true; std::set<int64_t> cuts;
    std::vector<std::pair<const Stmt *,int>> pending{{&body,0}};
    while (!pending.empty() && valid) {
      auto [node,depth] = pending.back(); pending.pop_back();
      if (++nodes > 512 || depth > 32 || node->kind == Stmt::Kind::Break || node->kind == Stmt::Kind::Continue || node->kind == Stmt::Kind::Return) { valid = false; break; }
      if (node->target && node->target->symbol == i) ++writes;
      if (node->kind == Stmt::Kind::If) if (auto c = comparison(*node->expression,i)) {
        auto [op,k] = *c; int64_t boundary = k+(op == "<=" || op == ">");
        if (boundary > 0 && boundary < b.constant->integer) cuts.insert(boundary);
      }
      auto safe = [&](Expr *e) {
        if (!e) return;
        visitExpressions(*e,[&](Expr &expression) {
          if (++nodes > 512 || (expression.kind == Expr::Kind::Call && !pure(*expression.callee))) valid = false;
        });
      };
      safe(node->expression.get()); safe(node->target.get());
      for (const auto &v : node->variables) for (auto entry : v.initializers) safe(entry.second);
      for (const auto &c : node->children) pending.push_back({c.get(),depth+1});
    }
    size_t cost = nodes*cuts.size();
    if (!valid || writes != 1 || cuts.empty() || cuts.size() > 7 || cost > 2048 || cost > growth) return;
    growth -= cost; cuts.insert(b.constant->integer);
    auto result = block(); int64_t low = INT32_MIN;
    for (int64_t end : cuts) {
      auto loop = program.statements->make(Stmt::Kind::While,s->location);
      loop->expression = binary("<",reference(i),integer(end));
      loop->children.push_back(partitionBody(body,i,low,end-1));
      result->children.push_back(std::move(loop)); low = end;
    }
    s = std::move(result);
  }

  bool expression(const Expr &e, Summary &summary) {
    if (e.symbol && e.symbol->global) summary.reads.insert(e.symbol);
    if (e.kind == Expr::Kind::Call) {
      if (e.callee->runtime || !pure(*e.callee)) return false;
      const auto &other = summaries.at(e.callee); summary.reads.insert(other.reads.begin(), other.reads.end());
    }
    for (const auto &child : e.operands) if (!expression(*child, summary)) return false;
    return true;
  }
  bool statementPure(const Stmt &s, Summary &summary) {
    if (s.kind == Stmt::Kind::Assignment && (s.target->kind != Expr::Kind::Variable || s.target->symbol->global || s.target->symbol->type.array())) return false;
    if (s.expression && !expression(*s.expression, summary)) return false;
    for (const auto &v : s.variables) for (auto entry : v.initializers) if (!expression(*entry.second, summary)) return false;
    for (const auto &child : s.children) if (!statementPure(*child, summary)) return false;
    return true;
  }
  bool pure(const Function &f) {
    auto &summary = summaries[&f];
    if (summary.state) return summary.state == 2 && summary.pure;
    summary.state = 1; summary.pure = !f.runtime && statementPure(*f.body, summary); summary.state = 2; return summary.pure;
  }
  void facts(const Stmt &s) {
    for (const auto &v : s.variables) if (v.symbol->type.numeric() && v.base == Base::Int && v.initializer) {
      if (v.initializers.empty()) initial[v.symbol] = 0;
      else if (v.initializers.size() == 1 && v.initializers[0].second->constant && v.initializers[0].second->type.base == Base::Int)
        initial[v.symbol] = v.initializers[0].second->constant->integer;
    }
    if (s.kind == Stmt::Kind::Assignment && s.target->kind == Expr::Kind::Variable) ++assignments[s.target->symbol];
    for (const auto &child : s.children) facts(*child);
  }
  const Expr *increment(const Stmt &s, const Symbol *symbol) {
    if (s.kind != Stmt::Kind::Assignment || s.target->kind != Expr::Kind::Variable || s.target->symbol != symbol ||
        s.expression->kind != Expr::Kind::Binary || s.expression->text != "+") return nullptr;
    for (int n = 0; n < 2; ++n) {
      const auto &e = *s.expression->operands[n];
      if (e.kind == Expr::Kind::Variable && e.operands.empty() && e.symbol == symbol) return s.expression->operands[1-n].get();
    }
    return nullptr;
  }
  void optimize(S &s) {
    for (auto &child : s->children) optimize(child);
    if (s->kind != Stmt::Kind::While || s->expression->kind != Expr::Kind::Binary || s->expression->text != "<" ||
        s->children[0]->kind != Stmt::Kind::Block || s->children[0]->children.empty()) return;
    auto &condition = *s->expression;
    auto &variable = *condition.operands[0], &limit = *condition.operands[1];
    if (variable.kind != Expr::Kind::Variable || !variable.type.numeric() || variable.type.base != Base::Int ||
        !limit.constant || limit.type.base != Base::Int) return;
    auto counter = variable.symbol;
    auto &body = *s->children[0]; auto tail = body.children.back().get();
    auto step = increment(*tail, counter);
    if (!step || !step->constant || step->constant->base != Base::Int || step->constant->integer != 1) return;
    if (!counter->global && limit.constant->integer > 0 && body.children.size() == 2 &&
        body.children[0]->kind == Stmt::Kind::Assignment) {
      auto &update = *body.children[0]; auto sum = update.target->symbol;
      auto call = sum ? increment(update,sum) : nullptr;
      if (sum != counter && sum && sum->type.numeric() && sum->type.base == Base::Int &&
          call && call->kind == Expr::Kind::Call && call->callee->replayRegion) {
        const auto &proof = *call->callee->replayRegion;
        bool stable = call->operands.size() == proof.parameters.size();
        for (auto object : proof.reads) if (object == sum || object == counter) stable = false;
        std::function<bool(const Expr &)> safe = [&](const Expr &e) {
          if (e.kind == Expr::Kind::Call || e.kind == Expr::Kind::Element || e.kind == Expr::Kind::String ||
              (e.symbol && (e.symbol == sum || e.symbol == counter || e.symbol->type.array()))) return false;
          for (const auto &a : e.operands) if (!safe(*a)) return false;
          return e.type.numeric() && e.type.base == Base::Int;
        };
        for (const auto &a : call->operands) stable &= safe(*a);
        if (stable) {
          auto guard = binary("&&",clone(condition),binary(">=",reference(counter),integer(0)));
          for (size_t p = 0; p < call->operands.size(); ++p) {
            const auto &b = proof.parameters[p];
            auto domain = binary("&&",binary(">=",clone(*call->operands[p]),integer(b.lower)),
                binary("<=",clone(*call->operands[p]),integer(b.upper)));
            guard = binary("&&",std::move(guard),std::move(domain));
          }
          auto once = block();
          auto count = binary("-",integer(limit.constant->integer),reference(counter));
          auto scaled = binary("+",reference(sum),binary("*",clone(*call),std::move(count)));
          once->children.push_back(assign(reference(sum),std::move(scaled)));
          once->children.push_back(assign(reference(counter),integer(limit.constant->integer)));
          auto version = program.statements->make(Stmt::Kind::If,s->location);
          version->expression = std::move(guard); version->children = {std::move(once),s}; s = std::move(version);
          return;
        }
      }
    }
    if (resetRegions && !counter->global) if (auto domain = ResetRegions().check(body,counter)) {
      auto guard = binary("&&",clone(condition),binary("&&",binary(">",clone(*domain->extent),integer(0)),
          binary("<=",clone(*domain->extent),integer(domain->capacity))));
      auto once = program.statements->make(body.kind,body.location);
      once->children = body.children;
      once->children.back() = assign(reference(counter),integer(limit.constant->integer));
      auto version = program.statements->make(Stmt::Kind::If,s->location);
      version->expression = std::move(guard); version->children = {std::move(once),s}; s = std::move(version);
      return;
    }
    if (!counter->global && body.children.size() == 2 && body.children[0]->kind == Stmt::Kind::Expression) {
      auto call = body.children[0]->expression;
      if (call && call->kind == Expr::Kind::Call && sorters.count(call->callee)) {
        auto array = sorters.at(call->callee); bool stable = call->operands.size() == 3;
        std::function<bool(const Expr &)> safe = [&](const Expr &e) {
          if (e.symbol && (e.symbol == counter || e.symbol->type.array())) return false;
          if (e.kind == Expr::Kind::Call) {
            if (e.callee->runtime || !pure(*e.callee) || summaries.at(e.callee).reads.count(array) || summaries.at(e.callee).reads.count(counter)) return false;
          }
          for (const auto &a : e.operands) if (!safe(*a)) return false;
          return true;
        };
        for (const auto &a : call->operands) stable &= a->type.numeric() && a->type.base == Base::Int && safe(*a);
        if (stable) {
          auto low = clone(*call->operands[0]), count = clone(*call->operands[1]);
          auto positive = binary("&&",binary(">",clone(*count),integer(0)),binary("<=",clone(*count),integer(array->type.shape[0])));
          auto interval = binary("&&",binary(">=",clone(*low),integer(0)),
              binary("<=",clone(*low),binary("-",integer(array->type.shape[0]),clone(*count))));
          auto power = binary("==",binary("&",clone(*count),binary("-",clone(*count),integer(1))),integer(0));
          auto domain = binary("||",binary("<=",clone(*count),integer(1)),
              binary("&&",std::move(positive),binary("&&",std::move(interval),std::move(power))));
          auto once = block(); once->children.push_back(eval(clone(*call)));
          once->children.push_back(assign(reference(counter),integer(limit.constant->integer)));
          auto version = program.statements->make(Stmt::Kind::If,s->location);
          version->expression = std::move(domain); version->children = {std::move(once),s};
          auto guard = program.statements->make(Stmt::Kind::If,s->location);
          guard->expression = clone(*s->expression); guard->children.push_back(std::move(version)); s = std::move(guard);
          return;
        }
      }
    }
    if (!initial.count(counter) || initial[counter] < 0 || assignments[counter] != 1) return;
    int64_t times = static_cast<int64_t>(limit.constant->integer) - initial[counter];
    if (times < 2) return;
    std::set<const Symbol *> locals, written;
    std::map<const Symbol *, std::vector<const Stmt *>> updates;
    bool valid = true;
    std::function<void(const Stmt &)> collect = [&](const Stmt &node) {
      if (&node == tail) return;
      if (node.kind == Stmt::Kind::Break || node.kind == Stmt::Kind::Continue || node.kind == Stmt::Kind::Return) valid = false;
      for (const auto &v : node.variables) { locals.insert(v.symbol); if (v.symbol->type.array()) valid = false; }
      if (node.kind == Stmt::Kind::Assignment) {
        if (node.target->kind != Expr::Kind::Variable || !node.target->type.numeric() || node.target->symbol->type.array()) valid = false;
        else { written.insert(node.target->symbol); updates[node.target->symbol].push_back(&node); }
      }
      for (const auto &child : node.children) collect(*child);
    };
    collect(body); if (!valid || written.count(counter)) return;
    std::set<const Symbol *> reductions, auxiliary;
    for (const auto *symbol : written) if (!locals.count(symbol)) {
      bool additive = symbol->type.base == Base::Int;
      for (auto update : updates[symbol]) additive &= increment(*update, symbol) != nullptr;
      (additive ? reductions : auxiliary).insert(symbol);
    }
    std::function<bool(const Expr &, const std::set<const Symbol *> &)> independent = [&](const Expr &e, const std::set<const Symbol *> &defined) {
      if (e.symbol && (e.symbol == counter || reductions.count(e.symbol) || (auxiliary.count(e.symbol) && !defined.count(e.symbol)))) return false;
      if (e.kind == Expr::Kind::Call) {
        if (e.callee->runtime || !pure(*e.callee)) return false;
        for (auto symbol : summaries.at(e.callee).reads) if (written.count(symbol) || symbol == counter) return false;
      }
      for (const auto &child : e.operands) if (!independent(*child, defined)) return false;
      return true;
    };
    std::function<bool(const Stmt &, std::set<const Symbol *> &)> check = [&](const Stmt &node, std::set<const Symbol *> &defined) {
      if (&node == tail) return true;
      if (node.kind == Stmt::Kind::Assignment) {
        auto symbol = node.target->symbol;
        const Expr *rhs = reductions.count(symbol) ? increment(node, symbol) : node.expression.get();
        if (!rhs || !independent(*rhs, defined)) return false;
        defined.insert(symbol); return true;
      }
      if (node.expression && !independent(*node.expression, defined)) return false;
      for (const auto &v : node.variables) for (auto entry : v.initializers) if (!independent(*entry.second, defined)) return false;
      if (node.kind == Stmt::Kind::If) {
        auto yes = defined, no = defined;
        if (!check(*node.children[0], yes)) return false;
        if (node.children.size() == 2 && !check(*node.children[1], no)) return false;
        for (auto symbol : yes) if (no.count(symbol)) defined.insert(symbol);
        return true;
      }
      if (node.kind == Stmt::Kind::While) { auto iteration = defined; return check(*node.children[0], iteration); }
      for (const auto &child : node.children) if (!check(*child, defined)) return false;
      return true;
    };
    std::set<const Symbol *> defined;
    if (!check(body, defined)) return;
    auto result = block(); std::map<const Symbol *, Symbol *> saved;
    for (auto symbol : reductions) {
      auto temp = std::make_unique<Symbol>(); temp->name = ".repeat" + std::to_string(serial++); temp->type = symbol->type;
      auto pointer = temp.get(); program.symbols.push_back(std::move(temp)); saved[symbol] = pointer;
      result->children.push_back(declare(pointer, reference(const_cast<Symbol *>(symbol))));
    }
    result->children.push_back(std::move(s->children[0]));
    for (auto symbol : reductions) {
      auto delta = binary("-", reference(const_cast<Symbol *>(symbol)), reference(saved.at(symbol)));
      auto scaled = binary("+", reference(saved.at(symbol)), binary("*", std::move(delta), integer(times)));
      result->children.push_back(assign(reference(const_cast<Symbol *>(symbol)), std::move(scaled)));
    }
    result->children.push_back(assign(reference(counter), integer(limit.constant->integer)));
    // A syntactic constant initializer need not execute again when an enclosing
    // loop revisits this statement. Preserve the actual zero-trip condition.
    auto guard = program.statements->make(Stmt::Kind::If,s->location);
    guard->expression = std::move(s->expression); guard->children.push_back(std::move(result));
    s = std::move(guard);
  }
public:
  explicit Repetition(ProgramIR &program) : NodeBuilder(program) {}
  void run(int level,const HighIROptions &options) {
    bool sortingContracts = level >= 3 && options.sortingContracts;
    bool regions = level >= 3 && options.regionSummaries;
    bool reset = options.resetRegions;
    resetRegions = reset;
    if (sortingContracts) sorters = SortingNetworks().run(program);
    for (auto &item : program.items) if (item.function) item.function->replayRegion.reset();
    if (regions) for (auto &item : program.items) if (item.function) {
      item.function->replayRegion = RegionKills().check(*item.function,[&](const Function &f) {
        if (!pure(f) || !summaries.at(&f).reads.empty() || f.tensorResult) return false;
        for (const auto &p : f.parameters) if (!p.symbol->type.numeric()) return false;
        return true;
      });
    }
    for (auto &item : program.items) if (item.function) {
      if (level >= 3 && (options.smallUnroll || options.constantLoops)) {
        size_t growth = 4096, evaluation = 16384;
        smallLoops(item.function->body,growth,evaluation,options.constantLoops,options.smallUnroll);
      }
      if (options.loopPartition) { size_t growth = 4096; partition(item.function->body,growth); }
      if (level >= 3) { initial.clear(); assignments.clear(); facts(*item.function->body); optimize(item.function->body); }
    }
  }
};
} // namespace

void planTensors(ProgramIR &program, int tile) {
  program.schedules.clear();
  for (uint32_t n = 0; n < program.expressions->size(); ++n) {
    auto &e = program.expressions->at(n);
    if (e.kind != Expr::Kind::Binary || e.text != "@") continue;
    const auto &a = e.operands[0]->type, &b = e.operands[1]->type;
    if (a.shape.size() != 2 || b.shape.size() != 2 || a.shape[1] != b.shape[0]) throw std::logic_error("invalid tensor iteration domain");
    // Independent columns preserve each dot product's reduction order. Eight
    // float accumulators fit the FP bank; integer kernels also need index and
    // pointer registers, so their conservative schedule uses four columns.
    int width = tile ? tile : e.type.base == Base::Float && b.shape[1] >= 8 && a.shape[1] >= 8 ? 8 : 4;
    if (width != 1 && width != 2 && width != 4 && width != 8) throw std::runtime_error("tensor tile must be 1, 2, 4 or 8");
    e.schedule = program.schedules.size();
    program.schedules.push_back({n,a.shape[0],a.shape[1],b.shape[1],width,e.uniformProduct,e.type.base});
  }
}
void printTensorPlan(const ProgramIR &program, std::ostream &out) {
  for (const auto &s : program.schedules) out << "product n" << s.node << " domain=[" << s.rows << ',' << s.columns << ',' << s.reduction
      << "] tile=[1," << s.columnTile << "] remainder=scalar reduction=ordered snapshot=result algorithm="
      << (s.uniform ? "proved-uniform" : "dot-product") << '\n';
}

void expandTensors(ProgramIR &program) { TensorExpansion(program).run(); }
void analyzeTensors(ProgramIR &program) { TensorStructure::run(program); }
void optimizeHighIR(ProgramIR &program, int level, const HighIROptions &options) {
  if (level >= 2) {
    if (options.scalarSummaries) ScalarFunctions().run(program);
    if (options.modularLoops || options.quotientLoops) IntegerLoops(program,options).run();
  }
  if (level >= 2) Repetition(program).run(level,options);
}
} // namespace sysy
