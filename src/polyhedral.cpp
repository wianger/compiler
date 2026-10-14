#include "ir.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <numeric>
#include <set>

namespace sysy {
namespace {
using Bounds = std::array<int64_t,2>;
using Environment = std::map<const Symbol *,Bounds>;
using E = ExprRef;
using S = StmtRef;
bool add(int64_t a, int64_t b, int64_t &r) {
  if ((b > 0 && a > INT64_MAX-b) || (b < 0 && a < INT64_MIN-b)) return false;
  r = a+b; return true;
}
bool multiply(int64_t a, int64_t b, int64_t &r) {
  if (!a || !b) { r = 0; return true; }
  if (a > 0 ? (b > 0 ? a > INT64_MAX/b : b < INT64_MIN/a)
            : (b > 0 ? a < INT64_MIN/b : a < INT64_MAX/b)) return false;
  r = a*b; return true;
}
int64_t floorDivide(int64_t a, int64_t b) { return a/b - (a%b < 0); }
int64_t ceilDivide(int64_t a, int64_t b) { return a/b + (a%b > 0); }
struct Constraint { std::vector<int64_t> coefficients; int64_t constant = 0; bool equality = false; };
// Bounded Presburger feasibility: exact unit-equality projection, integer
// interval propagation, congruence checks, and budgeted branch-and-bound.
// Failure to prove emptiness always blocks the transformation.
struct IntegerSet {
  std::vector<Bounds> bounds;
  std::vector<Constraint> rows;
  bool valid = true;
  void equation(const std::vector<int64_t> &row) {
    rows.push_back({std::vector<int64_t>(row.begin(),row.end()-1),row.back(),true});
  }
  void direction(int a, int b, int sign) {
    Constraint c; c.coefficients.resize(bounds.size()); c.constant = -1;
    c.coefficients[a] = -sign; c.coefficients[b] = sign; rows.push_back(std::move(c));
  }
  bool project() {
    for (size_t iteration = 0; iteration < bounds.size(); ++iteration) {
      int pivot = -1, variable = -1;
      for (size_t n = 0; n < rows.size() && pivot < 0; ++n) if (rows[n].equality)
        for (size_t v = 0; v < bounds.size(); ++v) if (rows[n].coefficients[v] == 1 || rows[n].coefficients[v] == -1) {
          pivot = n; variable = v; break;
        }
      if (pivot < 0) return true;
      auto equation = rows[pivot]; int64_t sign = equation.coefficients[variable];
      rows.erase(rows.begin()+pivot);
      for (auto &row : rows) {
        int64_t scale;
        if (!multiply(row.coefficients[variable],sign,scale)) return false;
        if (scale == INT64_MIN) return false;
        for (size_t v = 0; v < bounds.size(); ++v) {
          int64_t term;
          if (!multiply(-scale,equation.coefficients[v],term) || !add(row.coefficients[v],term,row.coefficients[v])) return false;
        }
        int64_t term;
        if (!multiply(-scale,equation.constant,term) || !add(row.constant,term,row.constant)) return false;
      }
      Constraint low, high; low.coefficients.resize(bounds.size()); high.coefficients.resize(bounds.size());
      for (size_t v = 0; v < bounds.size(); ++v) if (static_cast<int>(v) != variable) {
        if (!multiply(-sign,equation.coefficients[v],low.coefficients[v]) ||
            !multiply(sign,equation.coefficients[v],high.coefficients[v])) return false;
      }
      int64_t c;
      if (!multiply(-sign,equation.constant,c) || !add(c,-bounds[variable][0],low.constant) ||
          !multiply(sign,equation.constant,c) || !add(c,bounds[variable][1],high.constant)) return false;
      rows.push_back(std::move(low)); rows.push_back(std::move(high)); bounds[variable] = {0,0};
    }
    return true;
  }
  // 0 = empty, 1 = nonempty, 2 = unknown.
  int search(std::vector<Bounds> box, int &budget, int depth) const {
    if (--budget < 0 || depth > 24) return 2;
    for (auto b : box) if (b[0] > b[1]) return 0;
    for (int round = 0; round < 32; ++round) {
      bool changed = false;
      for (const auto &row : rows) {
        int64_t maximum = row.constant, minimum = row.constant, gcd = 0, fixed = row.constant;
        for (size_t v = 0; v < box.size(); ++v) {
          int64_t a = row.coefficients[v], hi, lo, term;
          if (a == INT64_MIN || !multiply(a,box[v][a >= 0 ? 1 : 0],hi) ||
              !multiply(a,box[v][a >= 0 ? 0 : 1],lo) ||
              !add(maximum,hi,maximum) || !add(minimum,lo,minimum)) return 2;
          if (box[v][0] == box[v][1]) {
            if (!multiply(a,box[v][0],term) || !add(fixed,term,fixed)) return 2;
          } else gcd = std::gcd(gcd,a < 0 ? -a : a);
        }
        if (maximum < 0 || (row.equality && (minimum > 0 || (gcd && fixed%gcd)))) return 0;
        for (int polarity : {1,-1}) {
          if (polarity == -1 && !row.equality) break;
          if (minimum == INT64_MIN) return 2;
          int64_t max = polarity == 1 ? maximum : -minimum;
          for (size_t v = 0; v < box.size(); ++v) {
            int64_t a = row.coefficients[v]*polarity, term, rest;
            if (!a) continue;
            if (!multiply(a,box[v][a > 0 ? 1 : 0],term) || term == INT64_MIN || !add(max,-term,rest) || rest == INT64_MIN) return 2;
            if (a > 0) {
              int64_t next = std::max(box[v][0],ceilDivide(-rest,a));
              changed |= next != box[v][0]; box[v][0] = next;
            } else {
              int64_t next = std::min(box[v][1],floorDivide(rest,-a));
              changed |= next != box[v][1]; box[v][1] = next;
            }
            if (box[v][0] > box[v][1]) return 0;
          }
        }
      }
      if (!changed) break;
    }
    int variable = -1; int64_t width = 0;
    for (size_t v = 0; v < box.size(); ++v) if (box[v][1]-box[v][0] > width) {
      bool used = std::any_of(rows.begin(),rows.end(),[&](const Constraint &r) { return r.coefficients[v] != 0; });
      if (used) { width = box[v][1]-box[v][0]; variable = v; }
    }
    if (variable < 0) return 1;
    int64_t middle = box[variable][0]+width/2; auto other = box;
    box[variable][1] = middle; other[variable][0] = middle+1;
    int left = search(std::move(box),budget,depth+1); if (left == 1) return 1;
    int right = search(std::move(other),budget,depth+1);
    return right == 1 ? 1 : left == 0 && right == 0 ? 0 : 2;
  }
  int empty() {
    if (!valid || !project()) return 2;
    int budget = 96; return search(bounds,budget,0);
  }
};

bool scalar(const Expr &e, const Symbol *s) { return e.kind == Expr::Kind::Variable && e.operands.empty() && e.symbol == s; }
std::optional<int64_t> constant(const Expr &e, const Environment &env) {
  if (e.type.base != Base::Int || !e.type.numeric()) return {};
  if (e.constant) return e.constant->integer;
  if (e.kind == Expr::Kind::Variable && e.operands.empty()) {
    auto found = env.find(e.symbol);
    if (found != env.end() && found->second[0] == found->second[1]) return found->second[0];
  }
  return {};
}
struct Loop {
  S statement, body, init;
  const Symbol *counter = nullptr;
  int64_t lower = 0, upper = 0;
};
template<class F> void expressions(const Stmt &s, F visit) {
  if (s.expression) visitExpressions(*s.expression,visit);
  if (s.target) visitExpressions(*s.target,visit);
  for (const auto &v : s.variables) for (auto [offset,e] : v.initializers) visitExpressions(*e,visit);
}
void invalidateCalls(const Stmt &s, Environment &env) {
  expressions(s,[&](Expr &e) {
    if (e.kind != Expr::Kind::Call) return;
    for (auto it = env.begin(); it != env.end();)
      if (it->first->global && !it->first->isConst) it = env.erase(it); else ++it;
  });
}
bool parseLoop(S s, const Environment &env, Loop &loop) {
  if (s->kind != Stmt::Kind::While || s->scheduled || !s->expression || s->children.size() != 1 ||
      s->children[0]->kind != Stmt::Kind::Block || s->children[0]->children.empty()) return false;
  auto &c = *s->expression;
  if (c.kind != Expr::Kind::Binary || c.text != "<" || c.operands.size() != 2) return false;
  auto &v = *c.operands[0];
  if (v.kind != Expr::Kind::Variable || !v.operands.empty() || v.symbol->global || v.symbol->type.array() || v.type.base != Base::Int) return false;
  auto first = env.find(v.symbol); auto last = constant(*c.operands[1],env);
  if (first == env.end() || first->second[0] != first->second[1] || !last || *last <= first->second[0]) return false;
  auto body = s->children[0]; auto tail = body->children.back();
  if (tail->kind != Stmt::Kind::Assignment || !scalar(*tail->target,v.symbol) ||
      tail->expression->kind != Expr::Kind::Binary || tail->expression->text != "+") return false;
  auto &step = *tail->expression;
  auto one = [](const Expr &e) { return e.constant && e.constant->base == Base::Int && e.constant->integer == 1; };
  if (step.type.base != Base::Int || (!(scalar(*step.operands[0],v.symbol) && one(*step.operands[1])) &&
      !(scalar(*step.operands[1],v.symbol) && one(*step.operands[0])))) return false;
  int writes = 0; bool bad = false; std::vector<S> work{body};
  while (!work.empty()) {
    auto node = work.back(); work.pop_back();
    if (node->kind == Stmt::Kind::Assignment && scalar(*node->target,v.symbol)) ++writes;
    if (c.operands[1]->symbol && c.operands[1]->symbol != v.symbol && node->kind == Stmt::Kind::Assignment &&
        scalar(*node->target,c.operands[1]->symbol)) bad = true;
    if (c.operands[1]->symbol && c.operands[1]->symbol->global && !c.operands[1]->symbol->isConst)
      expressions(*node,[&](Expr &e) { bad |= e.kind == Expr::Kind::Call; });
    for (auto child : node->children) work.push_back(child);
  }
  if (writes != 1 || bad) return false;
  loop = {s,body,{},v.symbol,first->second[0],*last}; return true;
}
void facts(const Stmt &s, Environment &env, bool invalidate = true) {
  if (invalidate) invalidateCalls(s,env);
  if (s.kind == Stmt::Kind::Block) {
    for (auto child : s.children) facts(*child,env,invalidate);
    return;
  }
  for (const auto &v : s.variables) if (v.symbol->type.numeric()) {
    env.erase(v.symbol);
    if (v.initializer && v.initializers.empty()) env[v.symbol] = {0,0};
    else if (v.initializers.size() == 1) {
      auto n = constant(*v.initializers[0].second,env);
      if (n && v.base == Base::Int) env[v.symbol] = {*n,*n};
    }
  }
  if (s.kind == Stmt::Kind::Assignment && s.target->kind == Expr::Kind::Variable && s.target->operands.empty()) {
    auto n = constant(*s.expression,env); env.erase(s.target->symbol);
    if (n && s.target->type.base == Base::Int) env[s.target->symbol] = {*n,*n};
  }
}

class Polyhedral {
  ProgramIR &program;
  int requestedTile, serial = 0;
  std::string function;
  struct Model {
    PolyRegion ir;
    Loop outer, inner;
    Environment environment;
    std::set<const Symbol *> privateSymbols;
    std::set<const Symbol *> defined;
    std::vector<S> prefix, suffix;
    bool valid = true;
    int phase = 0, expressions = 0, integerLocals = 0, floatLocals = 0;
  };
  Bounds bound(const Model &m, const Symbol *s) const {
    auto found = m.environment.find(s); return found == m.environment.end() ? Bounds{INT32_MIN,INT32_MAX} : found->second;
  }
  std::optional<Bounds> range(const PolyAffine &a, const Model &m) const {
    if (!a.known) return {};
    Bounds r{a.constant,a.constant};
    auto term = [&](int64_t coefficient, Bounds b) {
      int64_t lo,hi;
      return multiply(coefficient,b[coefficient >= 0 ? 0 : 1],lo) && multiply(coefficient,b[coefficient >= 0 ? 1 : 0],hi) &&
          add(r[0],lo,r[0]) && add(r[1],hi,r[1]);
    };
    for (int d = 0; d < 2; ++d) if (!term(a.dimensions[d],{m.ir.lower[d],m.ir.upper[d]-1})) return {};
    for (auto [s,c] : a.parameters) if (!term(c,bound(m,s))) return {};
    return r;
  }
  PolyAffine affine(const Expr &e, const Model &m, int depth = 0) const {
    PolyAffine a;
    if (depth > 64 || e.type.base != Base::Int) { a.known = false; return a; }
    if (e.constant) { a.constant = e.constant->integer; return a; }
    if (e.kind == Expr::Kind::Variable && e.operands.empty() && !e.symbol->type.array()) {
      if (e.symbol == m.outer.counter && m.outer.upper-m.outer.lower == 1) a.constant = m.outer.lower;
      else if (e.symbol == m.outer.counter) a.dimensions[0] = 1;
      else if (e.symbol == m.inner.counter) {
        if (m.phase == 0) a.known = false;
        else if (m.phase == 2) a.constant = m.inner.upper;
        else if (m.inner.upper-m.inner.lower == 1) a.constant = m.inner.lower;
        else a.dimensions[1] = 1;
      } else if (m.privateSymbols.count(e.symbol)) a.known = false;
      else {
        auto b = bound(m,e.symbol);
        if (b[0] == b[1]) a.constant = b[0]; else a.parameters[e.symbol] = 1;
      }
      return a;
    }
    int64_t scale = 1;
    if (e.kind == Expr::Kind::Unary && e.text == "-") { a = affine(*e.operands[0],m,depth+1); scale = -1; }
    else if (e.kind == Expr::Kind::Binary && (e.text == "+" || e.text == "-")) {
      a = affine(*e.operands[0],m,depth+1); auto b = affine(*e.operands[1],m,depth+1);
      scale = e.text == "-" ? -1 : 1; a.known &= b.known;
      auto merge = [&](int64_t &x, int64_t y) { int64_t z; a.known &= multiply(y,scale,z) && add(x,z,x); };
      for (int d = 0; d < 2; ++d) merge(a.dimensions[d],b.dimensions[d]);
      for (auto [s,c] : b.parameters) merge(a.parameters[s],c);
      merge(a.constant,b.constant); scale = 1;
    } else if (e.kind == Expr::Kind::Binary && e.text == "*") {
      int c = e.operands[0]->constant ? 0 : 1;
      if (!e.operands[c]->constant) { a.known = false; return a; }
      scale = e.operands[c]->constant->integer; a = affine(*e.operands[1-c],m,depth+1);
    } else { a.known = false; return a; }
    for (auto &c : a.dimensions) a.known &= multiply(c,scale,c);
    for (auto &p : a.parameters) a.known &= multiply(p.second,scale,p.second);
    a.known &= multiply(a.constant,scale,a.constant);
    auto r = range(a,m);
    if (!r || (*r)[0] < INT32_MIN || (*r)[1] > INT32_MAX) a.known = false;
    return a;
  }
  void access(const Expr &e, bool write, Model &m) {
    PolyAccess a; a.write = write;
    const Expr *base = &e;
    if (e.kind == Expr::Kind::Element) {
      base = e.operands[0].get();
      if (base->kind != Expr::Kind::Variable || !base->operands.empty()) { m.valid = false; return; }
      a.subscripts.push_back(affine(*e.operands[1],m));
      a.offset = a.subscripts[0];
    } else {
      if (e.operands.size() != e.symbol->type.shape.size()) { m.valid = false; return; }
      for (auto index : e.operands) a.subscripts.push_back(affine(*index,m));
      a.logical = true; int64_t stride = 1;
      for (size_t d = a.subscripts.size(); d-- > 0;) {
        const auto &index = a.subscripts[d];
        auto r = range(index,m);
        if (!r || (d > 0 && ((*r)[0] < 0 || (*r)[1] >= e.symbol->type.shape[d]))) a.logical = false;
        a.offset.known &= index.known;
        auto merge = [&](int64_t &x, int64_t y) { int64_t z; a.offset.known &= multiply(y,stride,z) && add(x,z,x); };
        for (int k = 0; k < 2; ++k) merge(a.offset.dimensions[k],index.dimensions[k]);
        for (auto [s,c] : index.parameters) merge(a.offset.parameters[s],c);
        merge(a.offset.constant,index.constant);
        if (d > 0 && !multiply(stride,e.symbol->type.shape[d],stride)) a.offset.known = false;
      }
    }
    a.object = base->symbol;
    if (!a.object || !a.object->type.array() || m.ir.accesses.size() >= 64) { m.valid = false; return; }
    if (write && (!a.offset.known || !range(a.offset,m))) m.valid = false;
    m.ir.accesses.push_back(std::move(a));
    for (size_t n = e.kind == Expr::Kind::Element ? 1 : 0; n < e.operands.size(); ++n) expression(*e.operands[n],m);
  }
  void expression(const Expr &e, Model &m) {
    if (++m.expressions > 512) { m.valid = false; return; }
    if (e.kind == Expr::Kind::Call || e.kind == Expr::Kind::String || e.type.array()) { m.valid = false; return; }
    if ((e.kind == Expr::Kind::Variable && e.symbol->type.array()) || e.kind == Expr::Kind::Element) { access(e,false,m); return; }
    if (e.symbol && m.privateSymbols.count(e.symbol) && !m.defined.count(e.symbol)) { m.valid = false; return; }
    if (m.phase == 0 && scalar(e,m.inner.counter)) { m.valid = false; return; }
    for (auto operand : e.operands) expression(*operand,m);
  }
  void collect(const Stmt &s, Model &m) {
    if (!m.valid) return;
    if (s.kind == Stmt::Kind::While || s.kind == Stmt::Kind::Break || s.kind == Stmt::Kind::Continue || s.kind == Stmt::Kind::Return) { m.valid = false; return; }
    for (const auto &v : s.variables) {
      if (!v.symbol->type.numeric() || !v.initializer) { m.valid = false; return; }
      for (auto [offset,value] : v.initializers) expression(*value,m);
      m.defined.insert(v.symbol);
    }
    if (s.kind == Stmt::Kind::Assignment) {
      if (s.target->kind == Expr::Kind::Variable && !s.target->symbol->type.array() && s.target->operands.empty()) {
        if (!m.privateSymbols.count(s.target->symbol)) { m.valid = false; return; }
      } else access(*s.target,true,m);
    }
    if (s.expression) expression(*s.expression,m);
    for (auto child : s.children) collect(*child,m);
  }
  bool aliases(const Symbol *a, const Symbol *b) const {
    if (a == b) return true;
    bool localA = !a->global && !a->parameter, localB = !b->global && !b->parameter;
    return !localA && !localB && (a->parameter || b->parameter);
  }
  PolyDependence relation(int p, int q, Model &m) {
    const auto &a = m.ir.accesses[p], &b = m.ir.accesses[q];
    PolyDependence result{p,q,false,{},{},2,2};
    if (a.object != b.object) return result; // unknown relative base for aliases
    std::vector<PolyAffine> x{a.offset}, y{b.offset};
    if (a.logical && b.logical && a.subscripts.size() == b.subscripts.size()) { x = a.subscripts; y = b.subscripts; }
    if (std::any_of(x.begin(),x.end(),[](const PolyAffine &v) { return !v.known; }) ||
        std::any_of(y.begin(),y.end(),[](const PolyAffine &v) { return !v.known; })) return result;
    auto safe = [](const PolyAffine &f) {
      return f.constant != INT64_MIN && f.dimensions[0] != INT64_MIN && f.dimensions[1] != INT64_MIN &&
          std::none_of(f.parameters.begin(),f.parameters.end(),[](auto p) { return p.second == INT64_MIN; });
    };
    if (!std::all_of(x.begin(),x.end(),safe) || !std::all_of(y.begin(),y.end(),safe)) return result;
    result.difference = true;
    for (size_t n = 0; n < x.size(); ++n)
      result.difference &= x[n].dimensions == y[n].dimensions && x[n].parameters == y[n].parameters;
    if (result.difference) {
      for (int d = 0; d < 2; ++d) {
        int64_t span = m.ir.upper[d]-m.ir.lower[d]-1; result.bounds.push_back({-span,span});
      }
      for (size_t n = 0; n < x.size(); ++n) {
        int64_t c; if (!add(y[n].constant,-x[n].constant,c)) { result.equalities.clear(); return result; }
        result.equalities.push_back({x[n].dimensions[0],x[n].dimensions[1],c});
      }
    } else {
      for (int repeat = 0; repeat < 2; ++repeat) for (int d = 0; d < 2; ++d) result.bounds.push_back({m.ir.lower[d],m.ir.upper[d]-1});
      for (auto s : m.ir.parameters) result.bounds.push_back(bound(m,s));
      for (size_t n = 0; n < x.size(); ++n) {
        std::vector<int64_t> row(result.bounds.size()+1);
        for (int d = 0; d < 2; ++d) { row[d] = x[n].dimensions[d]; row[d+2] = -y[n].dimensions[d]; }
        for (size_t v = 0; v < m.ir.parameters.size(); ++v) {
          auto s = m.ir.parameters[v]; int64_t left = x[n].parameters.count(s) ? x[n].parameters.at(s) : 0;
          int64_t right = y[n].parameters.count(s) ? y[n].parameters.at(s) : 0;
          if (!add(left,-right,row[4+v])) { result.equalities.clear(); return result; }
        }
        if (!add(x[n].constant,-y[n].constant,row.back())) { result.equalities.clear(); return result; }
        result.equalities.push_back(std::move(row));
      }
    }
    return result;
  }
  int query(const PolyDependence &r, int outerSign, int innerSign, Model &m) {
    if (r.equalities.empty()) return 2;
    IntegerSet set; set.bounds = r.bounds;
    for (auto &row : r.equalities) set.equation(row);
    if (r.difference) {
      auto sign = [&](int dim, int direction) {
        Constraint c; c.coefficients.resize(set.bounds.size()); c.coefficients[dim] = direction; c.constant = -1; set.rows.push_back(std::move(c));
      };
      if (outerSign) sign(0,outerSign);
      if (innerSign) sign(1,innerSign);
    } else {
      if (outerSign) set.direction(0,2,outerSign);
      if (innerSign) set.direction(1,3,innerSign);
    }
    ++m.ir.queries; return set.empty();
  }
  std::pair<bool,bool> legality(Model &m) {
    std::set<const Symbol *> parameters;
    for (const auto &a : m.ir.accesses) {
      for (auto [s,c] : a.offset.parameters) if (c) parameters.insert(s);
      for (const auto &f : a.subscripts) for (auto [s,c] : f.parameters) if (c) parameters.insert(s);
    }
    for (auto s : parameters) m.ir.parameters.push_back(s);
    if (parameters.size() > 8) return {false,false};
    bool parallel = true, interchange = true;
    for (size_t p = 0; p < m.ir.accesses.size(); ++p) for (size_t q = p; q < m.ir.accesses.size(); ++q) {
      const auto &a = m.ir.accesses[p], &b = m.ir.accesses[q];
      if ((!a.write && !b.write) || !aliases(a.object,b.object)) continue;
      auto r = relation(p,q,m);
      int positive = query(r,1,0,m), negative = query(r,-1,0,m);
      r.parallel = positive == 0 && negative == 0 ? 0 : positive == 1 || negative == 1 ? 1 : 2;
      int first = query(r,1,-1,m), second = query(r,-1,1,m);
      r.interchange = first == 0 && second == 0 ? 0 : first == 1 || second == 1 ? 1 : 2;
      parallel &= r.parallel == 0; interchange &= r.interchange == 0;
      m.ir.dependences.push_back(std::move(r));
    }
    return {parallel,interchange};
  }
  E integer(int64_t n) {
    auto e = program.expressions->make(Expr::Kind::Number,Location{},std::to_string(n)); e->constant = Number::fromInt(n); return e;
  }
  E variable(const Symbol *s) {
    auto e = program.expressions->make(Expr::Kind::Variable,Location{},s->name); e->symbol = const_cast<Symbol *>(s); e->type = s->type; return e;
  }
  E binary(std::string op, E a, E b) {
    auto e = program.expressions->make(Expr::Kind::Binary,Location{},op);
    e->type.base = Base::Int; e->operands = {a,b}; return e;
  }
  S block() { return program.statements->make(Stmt::Kind::Block,Location{}); }
  S assign(const Symbol *s, E e) {
    auto node = program.statements->make(Stmt::Kind::Assignment,Location{}); node->target = variable(s); node->expression = e; return node;
  }
  Symbol *privateSymbol(const Symbol *s) {
    auto copy = std::make_unique<Symbol>(*s); copy->name = ".poly"+std::to_string(serial++)+"."+s->name;
    copy->parameter = copy->global = false; auto result = copy.get(); program.symbols.push_back(std::move(copy)); return result;
  }
  using Substitution = std::map<const Symbol *,E>;
  E clone(const Expr &e, const Substitution &substitution) {
    if (e.kind == Expr::Kind::Variable && e.operands.empty()) {
      auto found = substitution.find(e.symbol); if (found != substitution.end()) return found->second;
    }
    auto result = program.expressions->make(e.kind,e.location,e.text); *result = e; result->operands.clear();
    for (auto operand : e.operands) result->operands.push_back(clone(*operand,substitution));
    return result;
  }
  S clone(const Stmt &s, const Substitution &substitution, bool recursive = true) {
    auto result = program.statements->make(s.kind,s.location); result->scheduled = s.scheduled;
    if (s.target) result->target = clone(*s.target,substitution);
    if (s.expression) result->expression = clone(*s.expression,substitution);
    for (const auto &old : s.variables) {
      auto v = old; v.dimensions.clear(); v.initializers.clear(); v.initializer.reset();
      auto found = substitution.find(v.symbol); if (found != substitution.end()) { v.symbol = found->second->symbol; v.name = v.symbol->name; }
      if (old.initializer) {
        v.initializer.emplace();
        if (!old.initializers.empty()) {
          auto e = clone(*old.initializers.front().second,substitution); v.initializer->value = e; v.initializers.push_back({0,e.get()});
        }
      }
      result->variables.push_back(std::move(v));
    }
    if (recursive) for (auto child : s.children) result->children.push_back(clone(*child,substitution));
    return result;
  }
  S loop(const Symbol *counter, int64_t upper, S body, int step = 1) {
    body->children.push_back(assign(counter,binary("+",variable(counter),integer(step))));
    auto node = program.statements->make(Stmt::Kind::While,Location{}); node->scheduled = true;
    node->expression = binary("<",variable(counter),integer(upper)); node->children = {body}; return node;
  }
  bool speculativeInteger(const Expr &e, const Model &m) const {
    if (e.type.base != Base::Int || !e.type.numeric() || e.kind == Expr::Kind::Call || e.kind == Expr::Kind::String) return false;
    if (e.kind == Expr::Kind::Binary && (e.text == "/" || e.text == "%") &&
        (!e.operands[1]->constant || !e.operands[1]->constant->integer)) return false;
    if (e.kind == Expr::Kind::Variable && e.symbol->type.array()) {
      if (e.operands.size() != e.symbol->type.shape.size()) return false;
      for (size_t d = 0; d < e.operands.size(); ++d) {
        auto r = range(affine(*e.operands[d],m),m);
        if (!r || (*r)[0] < 0 || (*r)[1] >= e.symbol->type.shape[d]) return false;
      }
    } else if (e.kind == Expr::Kind::Element) return false;
    for (auto child : e.operands) if (!speculativeInteger(*child,m)) return false;
    return true;
  }
  S cloneCore(const Stmt &s, const Substitution &substitution, Model &m) {
    if (s.kind == Stmt::Kind::If && s.children.size() == 1 && s.expression->type.base == Base::Int) {
      auto update = s.children[0];
      while (update->kind == Stmt::Kind::Block && update->children.size() == 1) update = update->children[0];
      if (update->kind == Stmt::Kind::Assignment && update->target->type.base == Base::Int &&
          update->target->kind == Expr::Kind::Variable && update->target->operands.empty() &&
          m.privateSymbols.count(update->target->symbol) && update->expression->type.base == Base::Int &&
          update->expression->kind == Expr::Kind::Binary && update->expression->text == "+") {
        const auto &rhs = *update->expression; int accumulator = scalar(*rhs.operands[0],update->target->symbol) ? 0 : 1;
        if (scalar(*rhs.operands[accumulator],update->target->symbol) && speculativeInteger(*rhs.operands[1-accumulator],m)) {
          auto condition = clone(*s.expression,substitution);
          bool boolean = condition->kind == Expr::Kind::Unary && condition->text == "!";
          if (condition->kind == Expr::Kind::Binary) {
            const auto &op = condition->text;
            boolean |= op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=" || op == "&&" || op == "||";
          }
          if (!boolean) condition = binary("!=",condition,integer(0));
          auto contribution = binary("*",condition,clone(*rhs.operands[1-accumulator],substitution));
          auto result = program.statements->make(Stmt::Kind::Assignment,s.location);
          result->target = clone(*update->target,substitution);
          result->expression = binary("+",clone(*rhs.operands[accumulator],substitution),contribution);
          ++m.ir.predicated; return result;
        }
      }
    }
    auto result = clone(s,substitution,false);
    for (auto child : s.children) result->children.push_back(cloneCore(*child,substitution,m));
    return result;
  }
  S jam(Model &m, int width) {
    m.phase = 1;
    auto result = block(), body = block(), fused = block();
    std::vector<Substitution> substitutions(width);
    const Symbol *shared = m.inner.counter;
    if (m.inner.init->kind == Stmt::Kind::Declaration) shared = privateSymbol(shared);
    for (int lane = 0; lane < width; ++lane) {
      auto &s = substitutions[lane]; s[m.outer.counter] = lane ? binary("+",variable(m.outer.counter),integer(lane)) : variable(m.outer.counter);
      s[m.inner.counter] = variable(shared);
      for (auto symbol : m.privateSymbols) s[symbol] = variable(privateSymbol(symbol));
      for (auto prefix : m.prefix) body->children.push_back(clone(*prefix,s));
    }
    body->children.push_back(clone(*m.inner.init,substitutions.front()));
    for (int lane = 0; lane < width; ++lane) for (size_t n = 0; n+1 < m.inner.body->children.size(); ++n)
      fused->children.push_back(cloneCore(*m.inner.body->children[n],substitutions[lane],m));
    body->children.push_back(loop(shared,m.inner.upper,fused));
    for (int lane = 0; lane < width; ++lane) for (auto suffix : m.suffix) body->children.push_back(clone(*suffix,substitutions[lane]));
    auto main = loop(m.outer.counter,m.outer.upper-width+1,body,width);
    result->children.push_back(main);
    auto remainder = clone(*m.outer.statement,{}); remainder->scheduled = true; result->children.push_back(remainder);
    return result;
  }
  S interchange(Model &m) {
    auto result = block(), outside = block(), inside = block();
    result->children.push_back(clone(*m.inner.init,{}));
    outside->children.push_back(assign(m.outer.counter,integer(m.outer.lower)));
    for (size_t n = 0; n+1 < m.inner.body->children.size(); ++n) inside->children.push_back(clone(*m.inner.body->children[n],{}));
    outside->children.push_back(loop(m.outer.counter,m.outer.upper,inside));
    result->children.push_back(loop(m.inner.counter,m.inner.upper,outside));
    result->children.push_back(assign(m.outer.counter,integer(m.outer.upper)));
    return result;
  }
  bool candidate(S &s, const Environment &env, const Loop &outer) {
    Model m; m.outer = outer; m.environment = env; m.ir.statement = s.index; m.ir.function = function;
    m.environment[outer.counter] = {outer.lower,outer.upper-1};
    S inner; Environment factsInside = m.environment;
    for (size_t n = 0; n+1 < outer.body->children.size(); ++n) {
      auto child = outer.body->children[n];
      if (child->kind == Stmt::Kind::While) {
        if (inner || !parseLoop(child,factsInside,m.inner)) return false;
        inner = child;
      } else if (!inner) {
        facts(*child,factsInside);
        m.prefix.push_back(child);
      } else m.suffix.push_back(child);
    }
    if (!inner || outer.counter == m.inner.counter) return false;
    for (auto it = m.prefix.begin(); it != m.prefix.end(); ++it) {
      auto n = *it;
      while (n->kind == Stmt::Kind::Block && n->children.size() == 1) n = n->children[0];
      bool init = n->kind == Stmt::Kind::Assignment && scalar(*n->target,m.inner.counter);
      init |= n->kind == Stmt::Kind::Declaration && n->variables.size() == 1 && n->variables[0].symbol == m.inner.counter;
      if (init) { m.inner.init = n; m.prefix.erase(it); break; }
    }
    if (!m.inner.init) return false;
    m.ir.induction = {outer.counter,m.inner.counter}; m.ir.lower = {outer.lower,m.inner.lower}; m.ir.upper = {outer.upper,m.inner.upper};
    // Declare-private symbols are copied per lane, including body temporaries.
    std::vector<S> work = m.prefix; work.insert(work.end(),m.suffix.begin(),m.suffix.end());
    for (size_t n = 0; n+1 < m.inner.body->children.size(); ++n) work.push_back(m.inner.body->children[n]);
    int nodes = 0;
    while (!work.empty()) {
      auto node = work.back(); work.pop_back();
      if (++nodes > 128) return false;
      for (const auto &v : node->variables) {
        if (v.symbol == outer.counter || v.symbol == m.inner.counter) return false;
        m.privateSymbols.insert(v.symbol);
        (v.base == Base::Float ? m.floatLocals : m.integerLocals)++;
      }
      for (auto child : node->children) work.push_back(child);
    }
    m.phase = 0; for (auto n : m.prefix) collect(*n,m);
    m.phase = 1; for (size_t n = 0; n+1 < m.inner.body->children.size(); ++n) collect(*m.inner.body->children[n],m);
    m.phase = 2; for (auto n : m.suffix) collect(*n,m);
    if (!m.valid || std::none_of(m.ir.accesses.begin(),m.ir.accesses.end(),[](const PolyAccess &a) { return a.write; })) return false;
    auto legal = legality(m);
    bool perfect = m.prefix.empty() && m.suffix.empty();
    auto cost = [&](int dimension) {
      double result = 0;
      for (auto &a : m.ir.accesses) if (a.offset.known) {
        auto coefficient = a.offset.dimensions[dimension];
        auto stride = coefficient == INT64_MIN ? INT64_MAX : std::abs(coefficient);
        result += (a.write ? 3 : 1)*(stride == 0 ? .2 : 1+std::min<int64_t>(stride,1024)/16.0);
      }
      return result;
    };
    if (perfect && legal.second && cost(0)+1 < cost(1)) {
      m.ir.decision = "interchange"; s = interchange(m);
    } else {
      int width = requestedTile ? requestedTile : 4;
      while (width > 1 && (m.integerLocals*width > 8 || m.floatLocals*width > 8 || m.expressions*width > 384 ||
          outer.upper-outer.lower < width)) width /= 2;
      bool reuse = false;
      for (const auto &a : m.ir.accesses) if (!a.write && a.offset.known)
        reuse |= a.offset.dimensions[0] == 0 && a.offset.dimensions[1] != 0;
      if (legal.first && reuse && width > 1) {
        m.ir.tile = width; m.ir.decision = "unroll-and-jam"; s = jam(m,width);
      } else m.ir.decision = !legal.first && !legal.second ? "blocked-dependence-or-unknown" : "unchanged-cost";
    }
    bool changed = m.ir.decision == "interchange" || m.ir.decision == "unroll-and-jam";
    program.polyRegions.push_back(std::move(m.ir)); return changed;
  }
  void forgetWrites(const Stmt &s, Environment &env) {
    if (s.kind == Stmt::Kind::Assignment && s.target->kind == Expr::Kind::Variable && s.target->operands.empty()) env.erase(s.target->symbol);
    for (const auto &v : s.variables) env.erase(v.symbol);
    invalidateCalls(s,env);
    for (auto child : s.children) forgetWrites(*child,env);
  }
  void walk(S &s, Environment &env) {
    invalidateCalls(*s,env);
    if (s->scheduled) { forgetWrites(*s,env); return; }
    if (s->kind == Stmt::Kind::Block) {
      for (auto &child : s->children) walk(child,env);
      return;
    }
    Loop loop;
    if (parseLoop(s,env,loop)) {
      if (!candidate(s,env,loop)) { auto nested = env; nested[loop.counter] = {loop.lower,loop.upper-1}; walk(s->children[0],nested); }
      forgetWrites(*s,env); env[loop.counter] = {loop.upper,loop.upper}; return;
    }
    if (s->kind == Stmt::Kind::If || s->kind == Stmt::Kind::While) {
      for (auto &child : s->children) { auto nested = env; walk(child,nested); }
      forgetWrites(*s,env); return;
    }
    facts(*s,env,false);
  }
public:
  Polyhedral(ProgramIR &p, int tile) : program(p), requestedTile(tile) {}
  void run() {
    program.polyRegions.clear();
    for (auto &item : program.items) if (item.function) {
      bool nested = false; std::vector<std::pair<S,int>> work{{item.function->body,0}};
      while (!work.empty() && !nested) {
        auto [node,depth] = work.back(); work.pop_back();
        if (node->scheduled) continue;
        if (node->kind == Stmt::Kind::While && ++depth >= 2) { nested = true; break; }
        for (auto child : node->children) work.push_back({child,depth});
      }
      if (nested) { function = item.function->name; Environment env; walk(item.function->body,env); }
    }
  }
};
} // namespace
void optimizePolyhedral(ProgramIR &program, int level, int tile) {
  if (level >= 2) Polyhedral(program,tile).run();
}
void printPolyhedralIR(const ProgramIR &program, std::ostream &out) {
  const char *verdict[] = {"empty","nonempty","unknown"};
  for (const auto &r : program.polyRegions) {
    out << "region " << r.function << ".s" << r.statement << " domain={";
    for (int d = 0; d < 2; ++d) out << (d ? ", " : "") << r.lower[d] << " <= d" << d << '(' << r.induction[d]->name << ") < " << r.upper[d];
    out << "} schedule=" << r.decision << " tile=" << r.tile << " predicated=" << r.predicated << " queries=" << r.queries << '\n';
    out << "  shared parameters=[";
    for (size_t n = 0; n < r.parameters.size(); ++n) out << (n ? "," : "") << r.parameters[n]->name;
    out << "]\n";
    std::map<const Symbol *,int> objects;
    for (size_t n = 0; n < r.accesses.size(); ++n) {
      const auto &a = r.accesses[n]; auto object = objects.emplace(a.object,objects.size()).first->second;
      out << "  a" << n << (a.write ? " write " : " read ")
          << (a.object->global ? "global:" : a.object->parameter ? "parameter:" : "local:")
          << a.object->name << '@' << object << " map=";
      if (!a.offset.known) out << "opaque";
      else {
        out << a.offset.constant;
        for (int d = 0; d < 2; ++d) out << " + " << a.offset.dimensions[d] << "*d" << d;
        for (auto [s,c] : a.offset.parameters) out << " + " << c << '*' << s->name;
      }
      out << (a.logical ? " [logical indices]" : " [linear elements]") << '\n';
    }
    for (const auto &d : r.dependences) {
      out << "  R(a" << d.source << ",a" << d.target << ") " << (d.difference ? "delta" : "source/destination")
          << " cross-outer=" << verdict[d.parallel] << " reversed-order=" << verdict[d.interchange] << " bounds={";
      for (auto b : d.bounds) out << '[' << b[0] << ',' << b[1] << ']';
      out << "} equations={";
      for (const auto &row : d.equalities) {
        out << '['; for (size_t n = 0; n < row.size(); ++n) out << (n ? "," : "") << row[n]; out << "]";
      }
      out << "}\n";
    }
  }
}
} // namespace sysy
