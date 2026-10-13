#include "ir.hpp"

#include <functional>
#include <unordered_map>

namespace sysy {
namespace {
using E = std::unique_ptr<Expr>;
using S = std::unique_ptr<Stmt>;

E clone(const Expr &source) {
  auto e = std::make_unique<Expr>(source.kind, source.location, source.text);
  e->type = source.type; e->constant = source.constant; e->symbol = source.symbol; e->callee = source.callee;
  for (const auto &operand : source.operands) e->operands.push_back(clone(*operand));
  return e;
}
E reference(Symbol *symbol) {
  auto e = std::make_unique<Expr>(Expr::Kind::Variable, Location{}, symbol->name);
  e->symbol = symbol; e->type = symbol->type; return e;
}
E integer(int64_t n) {
  if (n < INT32_MIN || n > INT32_MAX) throw Error({}, "tensor lowering requires an int-sized iteration space");
  auto e = std::make_unique<Expr>(Expr::Kind::Number, Location{}, std::to_string(n));
  e->constant = Number::fromInt(n); return e;
}
E zero(Base base) {
  auto e = integer(0);
  if (base == Base::Float) { e->type.base = base; e->constant = Number::fromFloat(0); }
  return e;
}
E boolean(E source) {
  auto inner = std::make_unique<Expr>(Expr::Kind::Unary, source->location, "!");
  inner->operands.push_back(std::move(source));
  auto outer = std::make_unique<Expr>(Expr::Kind::Unary, inner->location, "!");
  outer->operands.push_back(std::move(inner)); return outer;
}
E binary(std::string op, E a, E b) {
  auto e = std::make_unique<Expr>(Expr::Kind::Binary, a->location, op);
  e->type.base = op == "<" ? Base::Int : (a->type.base == Base::Float || b->type.base == Base::Float ? Base::Float : Base::Int);
  e->operands.push_back(std::move(a)); e->operands.push_back(std::move(b)); return e;
}
E element(E array, E index) {
  auto e = std::make_unique<Expr>(Expr::Kind::Element, array->location, "element");
  e->type.base = array->type.base;
  e->operands.push_back(std::move(array)); e->operands.push_back(std::move(index)); return e;
}
S block() { return std::make_unique<Stmt>(Stmt::Kind::Block, Location{}); }
S assign(E target, E expression) {
  auto s = std::make_unique<Stmt>(Stmt::Kind::Assignment, target->location);
  s->target = std::move(target); s->expression = std::move(expression); return s;
}
S eval(E expression) {
  auto s = std::make_unique<Stmt>(Stmt::Kind::Expression, expression->location);
  s->expression = std::move(expression); return s;
}
S declare(Symbol *symbol, E initial = {}) {
  Variable v; v.name = symbol->name; v.symbol = symbol; v.base = symbol->type.base; v.tensor = symbol->type.tensor;
  if (initial) {
    v.initializer.emplace(); v.initializer->value = std::move(initial);
    v.initializers.emplace_back(0, v.initializer->value.get());
  }
  auto s = std::make_unique<Stmt>(Stmt::Kind::Declaration, Location{});
  s->variables.push_back(std::move(v)); return s;
}

class TensorExpansion {
  ProgramIR &program;
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
    auto w = std::make_unique<Stmt>(Stmt::Kind::While, Location{});
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
      auto result = std::make_unique<Expr>(Expr::Kind::Unary, e.location, e.text);
      result->type.base = e.type.base; result->operands.push_back(scalarize(*e.operands[0], index)); return result;
    }
    throw std::logic_error("unexpanded tensor operand");
  }
  S copy(E target, E expression, int64_t count) {
    return loop(count, [&](Symbol *i) {
      return assign(element(clone(*target), reference(i)), scalarize(*expression, *reference(i)));
    });
  }
  S matmul(Symbol *target, const Expr &a, const Expr &b) {
    int64_t rows = a.type.shape[0], inner = a.type.shape[1], columns = b.type.shape[1];
    return loop(rows, [&](Symbol *i) {
      return loop(columns, [&](Symbol *j) {
        auto sum = temporary({target->type.base, {}});
        auto body = block(); body->children.push_back(declare(sum, zero(sum->type.base)));
        body->children.push_back(loop(inner, [&](Symbol *k) {
          auto left = element(clone(a), binary("+", binary("*", reference(i), integer(inner)), reference(k)));
          auto right = element(clone(b), binary("+", binary("*", reference(k), integer(columns)), reference(j)));
          return assign(reference(sum), binary("+", reference(sum), binary("*", std::move(left), std::move(right))));
        }));
        auto offset = binary("+", binary("*", reference(i), integer(columns)), reference(j));
        body->children.push_back(assign(element(reference(target), std::move(offset)), reference(sum)));
        return body;
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
      prefix.push_back(matmul(result, *e->operands[0], *e->operands[1])); e = reference(result);
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
      auto branch = std::make_unique<Stmt>(Stmt::Kind::If, e->location);
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
    for (auto &operand : e->operands) rewrite(operand, prefix);
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
        auto d = std::make_unique<Stmt>(Stmt::Kind::Declaration, v.location);
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
      if (s->kind == Stmt::Kind::Return) expanded->children.push_back(std::make_unique<Stmt>(Stmt::Kind::Return, s->location));
      s = std::move(expanded); return;
    }
    rewrite(s->target, prefix); rewrite(s->expression, prefix);
    if (prefix.empty()) return;
    if (s->kind == Stmt::Kind::While) {
      auto guard = std::make_unique<Stmt>(Stmt::Kind::If, s->location);
      guard->expression = std::make_unique<Expr>(Expr::Kind::Unary, s->location, "!");
      guard->expression->operands.push_back(std::move(s->expression));
      guard->children.push_back(std::make_unique<Stmt>(Stmt::Kind::Break, s->location));
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
  explicit TensorExpansion(ProgramIR &program) : program(program) {}
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
} // namespace

void expandTensors(ProgramIR &program) { TensorExpansion(program).run(); }
} // namespace sysy
