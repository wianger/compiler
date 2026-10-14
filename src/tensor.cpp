#include "ir.hpp"

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

// Factor repeated, read-only loop bodies with integer additive reductions.
// Definite assignment proves that auxiliary state resets each iteration; reads
// of carried reductions, array writes, and impure calls prevent the transform.
class Repetition : NodeBuilder {
  struct Summary { int state = 0; bool pure = false; std::set<const Symbol *> reads; };
  std::map<const Function *, Summary> summaries;
  std::map<const Symbol *, int32_t> initial;
  std::map<const Symbol *, int> assignments;
  int serial = 0;

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
    if (!initial.count(counter) || initial[counter] < 0 || assignments[counter] != 1) return;
    int64_t times = static_cast<int64_t>(limit.constant->integer) - initial[counter];
    if (times < 2) return;
    auto &body = *s->children[0]; auto tail = body.children.back().get();
    auto step = increment(*tail, counter);
    if (!step || !step->constant || step->constant->base != Base::Int || step->constant->integer != 1) return;
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
    s = std::move(result);
  }
public:
  explicit Repetition(ProgramIR &program) : NodeBuilder(program) {}
  void run() {
    for (auto &item : program.items) if (item.function) {
      initial.clear(); assignments.clear(); facts(*item.function->body); optimize(item.function->body);
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
void optimizeHighIR(ProgramIR &program, int level) { if (level >= 3) Repetition(program).run(); }
} // namespace sysy
