#include "machine_ir.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <unordered_set>

namespace sysy {
CFGAnalysis::CFGAnalysis(const FunctionIR &f) {
  int count = f.blocks.size(); pred.resize(count); succ.resize(count); children.resize(count);
  rank.assign(count, -1); idom.assign(count, -1);
  for (int b = 0; b < count; ++b) {
    const auto &last = f.blocks[b].instructions.back();
    for (int next : {last.target, last.otherwise}) if (next >= 0 && std::find(succ[b].begin(), succ[b].end(), next) == succ[b].end()) {
      succ[b].push_back(next); pred[next].push_back(b);
    }
  }
  std::vector<bool> seen(count, false);
  std::function<void(int)> visit = [&](int b) {
    if (seen[b]) return;
    seen[b] = true; for (auto it = succ[b].rbegin(); it != succ[b].rend(); ++it) visit(*it); order.push_back(b);
  };
  visit(0); std::reverse(order.begin(), order.end());
  for (size_t n = 0; n < order.size(); ++n) rank[order[n]] = n;
  idom[0] = 0;
  auto intersect = [&](int a, int b) {
    while (a != b) {
      while (rank[a] > rank[b]) a = idom[a];
      while (rank[b] > rank[a]) b = idom[b];
    }
    return a;
  };
  bool changed;
  do {
    changed = false;
    for (int b : order) if (b != 0) {
      int parent = -1;
      for (int p : pred[b]) if (idom[p] >= 0) parent = parent < 0 ? p : intersect(parent, p);
      if (parent != idom[b]) { idom[b] = parent; changed = true; }
    }
  } while (changed);
  for (int b : order) if (b != 0) children[idom[b]].push_back(b);
  for (const auto &block : f.blocks) {
    const auto &last = block.instructions.back();
    topology.push_back({last.target, last.otherwise});
  }
}
bool CFGAnalysis::dominates(int a, int b) const {
  while (b >= 0 && b != a && b != idom[b]) b = idom[b];
  return a == b;
}
const CFGAnalysis &controlFlow(const FunctionIR &f) {
  bool valid = f.cfg && f.cfg->topology.size() == f.blocks.size();
  for (size_t b = 0; valid && b < f.blocks.size(); ++b) {
    const auto &i = f.blocks[b].instructions.back();
    valid = f.cfg->topology[b] == std::array<int, 2>{i.target, i.otherwise};
  }
  if (!valid) { f.cfg = std::make_shared<CFGAnalysis>(f); ++f.cfgBuilds; }
  else ++f.cfgHits;
  return *f.cfg;
}
namespace {
using Graph = CFGAnalysis;

bool effect(Op op) { return op == Op::Store || op == Op::Zero || op == Op::Call || op == Op::Jump || op == Op::Branch || op == Op::Return || op == Op::BranchCompare; }
int newValue(FunctionIR &f, ValueType type) { int id = f.values.size(); f.values.push_back(type); return id; }
int root(std::vector<int> &aliases, int v) {
  int r = v; while (aliases[r] != r) r = aliases[r];
  while (aliases[v] != v) { int next = aliases[v]; aliases[v] = r; v = next; }
  return r;
}
std::vector<const Instruction *> definitions(const FunctionIR &f) {
  std::vector<const Instruction *> defs(f.values.size(), nullptr);
  for (const auto &b : f.blocks) for (const auto &i : b.instructions) if (i.result >= 0) defs[i.result] = &i;
  return defs;
}

using Regions = std::set<const Symbol *>;
struct Effects { Regions reads, writes; bool observable = false; };
using Summaries = std::map<const Function *, Effects>;

// Pointer provenance is a union lattice. Distinct objects cannot overlap;
// parameters may alias each other or globals, but not this frame's locals.
std::vector<Regions> provenance(const FunctionIR &f) {
  std::vector<Regions> roots(f.values.size());
  bool changed;
  do {
    changed = false;
    for (const auto &b : f.blocks) for (const auto &i : b.instructions) {
      if (i.result < 0 || f.values[i.result] != ValueType::Pointer) continue;
      auto next = roots[i.result];
      if (i.op == Op::Address) next.insert(i.symbol);
      else if (i.op == Op::Parameter) next.insert(f.source->parameters.at(i.immediate).symbol);
      else if (i.op == Op::Offset || i.op == Op::Move) next.insert(roots[i.args[0]].begin(), roots[i.args[0]].end());
      else if (i.op == Op::Phi) for (int a : i.args) next.insert(roots[a].begin(), roots[a].end());
      else next.insert(nullptr);
      if (next != roots[i.result]) { roots[i.result] = std::move(next); changed = true; }
    }
  } while (changed);
  for (size_t v = 0; v < roots.size(); ++v) if (f.values[v] == ValueType::Pointer && roots[v].empty()) roots[v].insert(nullptr);
  return roots;
}
bool overlaps(const Regions &a, const Regions &b) {
  for (auto x : a) for (auto y : b) {
    if (!x || !y || x == y) return true;
    bool localX = !x->global && !x->parameter, localY = !y->global && !y->parameter;
    if (!localX && !localY && (x->parameter || y->parameter)) return true;
  }
  return false;
}

struct PointerLocation {
  const Symbol *object = nullptr;
  int base = -1;
  uint64_t offset = 0;
  bool sameBase(const PointerLocation &other) const {
    return object == other.object && base == other.base;
  }
  bool operator<(const PointerLocation &other) const {
    if (object != other.object) return std::less<const Symbol *>{}(object,other.object);
    return std::tie(base,offset) < std::tie(other.base,other.offset);
  }
};
uint64_t accessSize(ValueType type) {
  return type == ValueType::Pointer || type == ValueType::Double ? 8 : 4;
}
// Unknown arithmetic and phis retain their own SSA identity. Constant offsets
// may still distinguish fields relative to that identity, without assuming
// that different parameters or different dynamic indices are independent.
std::vector<PointerLocation> pointerLocations(const FunctionIR &f) {
  auto defs = definitions(f); std::vector<int> count(f.values.size(), 0);
  for (const auto &b : f.blocks) for (const auto &i : b.instructions)
    if (i.result >= 0) ++count[i.result];
  std::vector<PointerLocation> locations(f.values.size());
  for (size_t v = 0; v < locations.size(); ++v) locations[v].base = v;
  for (int b : controlFlow(f).order) for (const auto &i : f.blocks[b].instructions) {
    if (i.result < 0 || f.values[i.result] != ValueType::Pointer || count[i.result] != 1) continue;
    if (i.op == Op::Address) locations[i.result] = {i.symbol, -1, static_cast<uint64_t>(i.immediate)};
    else if (i.op == Op::Move) locations[i.result] = locations[i.args[0]];
    else if (i.op == Op::Offset) {
      const auto *index = defs[i.args[1]];
      if (!index || count[i.args[1]] != 1 || index->op != Op::Constant || f.values[i.args[1]] != ValueType::Int) continue;
      auto location = locations[i.args[0]];
      location.offset += static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(index->immediate))) *
          static_cast<uint64_t>(i.immediate);
      locations[i.result] = location;
    }
  }
  return locations;
}
struct AliasAnalysis {
  std::vector<Regions> roots;
  std::vector<PointerLocation> locations;
  explicit AliasAnalysis(const FunctionIR &f) : roots(provenance(f)), locations(pointerLocations(f)) {}
  bool mayAlias(int a, uint64_t aSize, int b, uint64_t bSize) const {
    if (!aSize || !bSize || !overlaps(roots[a], roots[b])) return false;
    const auto &x = locations[a], &y = locations[b];
    if (!x.sameBase(y)) return true;
    // Unsigned differences also handle RV64 pointer arithmetic wrapping, and
    // avoid signed overflow when a negative offset is multiplied by a stride.
    return x.offset - y.offset < bSize || y.offset - x.offset < aSize;
  }
  bool covers(int writer, uint64_t size, int reader, uint64_t readSize) const {
    const auto &x = locations[writer], &y = locations[reader];
    uint64_t distance = y.offset - x.offset;
    return x.sameBase(y) && distance < size && readSize <= size - distance;
  }
};

// Sparse SROA creates homes only for cells that are actually read. Dynamic
// accesses and escaping addresses reject the entire object; zero fills reset
// just the observed cells, rather than expanding a large array into registers.
void scalarReplace(FunctionIR &f) {
  if (f.ssa) throw std::runtime_error("sroa must precede SSA construction");
  if (std::none_of(f.objects.begin(), f.objects.end(), [](const Symbol *s) { return s->type.array(); })) return;
  AliasAnalysis alias(f); std::set<const Symbol *> rejected;
  std::map<const Symbol *, std::set<uint64_t>> cells;
  for (const auto *s : f.objects) if (s->type.array()) cells[s];
  auto reject = [&](int pointer) {
    // Escape is a def-use property, rather than a may-alias query: an opaque
    // external pointer (including a string literal) does not expose a local
    // allocation. Move/Offset/Phi provenance retains every local origin.
    for (auto s : alias.roots[pointer]) if (s) rejected.insert(s);
  };
  for (const auto &b : f.blocks) for (const auto &i : b.instructions) for (size_t n = 0; n < i.args.size(); ++n) {
    int a = i.args[n]; if (f.values[a] != ValueType::Pointer) continue;
    if ((i.op == Op::Offset || i.op == Op::Move) && n == 0 && i.result >= 0 && f.values[i.result] == ValueType::Pointer) continue;
    if (i.op == Op::Phi && i.result >= 0 && f.values[i.result] == ValueType::Pointer) continue;
    if (n != 0 || (i.op != Op::Load && i.op != Op::Store && i.op != Op::Zero)) { reject(a); continue; }
    const auto &p = alias.locations[a];
    if (!p.object || !cells.count(p.object)) { reject(a); continue; }
    uint64_t extent = static_cast<uint64_t>(p.object->type.elements()) * 4;
    uint64_t size = i.op == Op::Zero ? static_cast<uint64_t>(i.immediate) :
        accessSize(f.values[i.op == Op::Load ? i.result : i.args[1]]);
    if (p.offset % 4 || size % 4 || p.offset > extent || size > extent - p.offset || (i.op != Op::Zero && size != 4)) {
      reject(a); continue;
    }
    if (i.op == Op::Load) cells[p.object].insert(p.offset);
  }
  std::map<const Symbol *, std::map<uint64_t, int>> homes; size_t budget = 1024;
  for (const auto &[s, offsets] : cells) if (!rejected.count(s) && offsets.size() <= 256 && offsets.size() <= budget) {
    budget -= offsets.size(); auto &object = homes[s];
    for (auto offset : offsets) {
      int v = newValue(f, s->type.base == Base::Float ? ValueType::Float : ValueType::Int);
      object[offset] = v; f.variables.push_back(v);
    }
  }
  if (homes.empty()) return;
  for (auto &b : f.blocks) {
    std::vector<Instruction> code;
    for (auto i : b.instructions) {
      if (i.op == Op::Load || i.op == Op::Store || i.op == Op::Zero) {
        const auto &p = alias.locations[i.args[0]]; auto object = homes.find(p.object);
        if (object != homes.end()) {
          if (i.op == Op::Zero) {
            for (auto [offset, home] : object->second) if (offset >= p.offset && offset - p.offset < static_cast<uint64_t>(i.immediate)) {
              Instruction zero{Op::Constant}; zero.result = home; code.push_back(std::move(zero));
            }
            continue;
          }
          auto home = object->second.find(p.offset);
          if (i.op == Op::Load) { i.op = Op::Move; i.args = {home->second}; }
          else {
            if (home == object->second.end()) continue;
            i.op = Op::Move; i.result = home->second; i.args = {i.args[1]};
          }
        }
      }
      code.push_back(std::move(i));
    }
    b.instructions = std::move(code);
  }
  f.objects.erase(std::remove_if(f.objects.begin(), f.objects.end(), [&](const Symbol *s) { return homes.count(s); }), f.objects.end());
}
Effects callEffects(const Instruction &i, const std::vector<Regions> &roots, const Summaries &summaries) {
  auto found = summaries.find(i.callee);
  if (found == summaries.end()) return {{nullptr}, {nullptr}, true};
  Effects result; result.observable = found->second.observable;
  auto map = [&](const Regions &source, Regions &dest) {
    for (auto s : source) {
      if (s && s->parameter) {
        auto p = std::find_if(i.callee->parameters.begin(), i.callee->parameters.end(), [&](const Variable &v) { return v.symbol == s; });
        if (p == i.callee->parameters.end()) { dest.insert(nullptr); continue; }
        const auto &actual = roots.at(i.args.at(p - i.callee->parameters.begin()));
        dest.insert(actual.begin(), actual.end());
      } else dest.insert(s);
    }
  };
  map(found->second.reads, result.reads); map(found->second.writes, result.writes);
  return result;
}
Summaries summarize(const ModuleIR &module) {
  Summaries summaries;
  std::map<const Function *, std::vector<Regions>> roots;
  for (const auto &f : module.functions) { summaries[f.source] = {}; roots[f.source] = provenance(f); }
  bool changed;
  do {
    changed = false;
    for (const auto &f : module.functions) {
      Effects next; const auto &pointers = roots.at(f.source);
      auto merge = [](const Regions &from, Regions &to) {
        for (auto s : from) if (!s || s->global || s->parameter) to.insert(s);
      };
      for (const auto &b : f.blocks) for (const auto &i : b.instructions) {
        if (i.op == Op::Load) merge(pointers[i.args[0]], next.reads);
        if (i.op == Op::Store || i.op == Op::Zero) merge(pointers[i.args[0]], next.writes);
        if (i.op == Op::Call) {
          auto e = callEffects(i, pointers, summaries);
          merge(e.reads, next.reads); merge(e.writes, next.writes); next.observable |= e.observable;
        }
      }
      auto &old = summaries[f.source];
      if (next.reads != old.reads || next.writes != old.writes || next.observable != old.observable) {
        old = std::move(next); changed = true;
      }
    }
  } while (changed);
  return summaries;
}
bool constantCall(const Instruction &i, const Summaries &summaries) {
  auto found = summaries.find(i.callee);
  return i.op == Op::Call && found != summaries.end() && !found->second.observable && found->second.reads.empty() && found->second.writes.empty();
}

void ssa(FunctionIR &f) {
  if (f.ssa) return;
  const auto &graph = controlFlow(f);
  std::set<int> variables(f.variables.begin(), f.variables.end());
  std::vector<std::set<int>> use(f.blocks.size()), def(f.blocks.size()), live(f.blocks.size()), frontier(f.blocks.size());
  std::map<int, std::set<int>> sites;
  for (size_t b = 0; b < f.blocks.size(); ++b) for (const auto &i : f.blocks[b].instructions) {
    for (int a : i.args) if (variables.count(a) && !def[b].count(a)) use[b].insert(a);
    if (i.result >= 0 && variables.count(i.result)) { def[b].insert(i.result); sites[i.result].insert(b); }
  }
  bool changed;
  do {
    changed = false;
    for (auto it = graph.order.rbegin(); it != graph.order.rend(); ++it) {
      int b = *it; auto next = use[b];
      for (int s : graph.succ[b]) for (int v : live[s]) if (!def[b].count(v)) next.insert(v);
      if (next != live[b]) { live[b] = std::move(next); changed = true; }
    }
  } while (changed);
  for (int b : graph.order) if (graph.pred[b].size() > 1) for (int p : graph.pred[b])
    for (int runner = p; runner != graph.idom[b]; runner = graph.idom[runner]) frontier[runner].insert(b);
  for (int v : variables) {
    std::queue<int> work; auto visited = sites[v]; std::set<int> placed;
    for (int b : sites[v]) work.push(b);
    while (!work.empty()) {
      int b = work.front(); work.pop();
      for (int y : frontier[b]) if (live[y].count(v) && placed.insert(y).second) {
        Instruction phi{Op::Phi}; phi.result = v; phi.immediate = v;
        phi.edges = graph.pred[y]; phi.args.assign(phi.edges.size(), -1);
        f.blocks[y].instructions.insert(f.blocks[y].instructions.begin(), std::move(phi));
        if (visited.insert(y).second) work.push(y);
      }
    }
  }
  std::map<int, std::vector<int>> stack;
  std::vector<Instruction> initial;
  for (int v : variables) {
    Instruction zero{Op::Constant}; zero.result = newValue(f, f.values[v]);
    stack[v].push_back(zero.result); initial.push_back(std::move(zero));
  }
  f.blocks[0].instructions.insert(f.blocks[0].instructions.begin(), initial.begin(), initial.end());
  std::function<void(int)> rename = [&](int b) {
    std::vector<int> pushed;
    for (auto &i : f.blocks[b].instructions) {
      if (i.op != Op::Phi) for (int &a : i.args) if (variables.count(a)) a = stack[a].back();
      int original = i.result;
      if (original >= 0 && variables.count(original)) {
        i.result = newValue(f, f.values[original]); stack[original].push_back(i.result); pushed.push_back(original);
      }
    }
    for (int s : graph.succ[b]) for (auto &i : f.blocks[s].instructions) {
      if (i.op != Op::Phi) break;
      for (size_t n = 0; n < i.edges.size(); ++n) if (i.edges[n] == b) i.args[n] = stack[i.immediate].back();
    }
    for (int child : graph.children[b]) rename(child);
    for (auto it = pushed.rbegin(); it != pushed.rend(); ++it) stack[*it].pop_back();
  };
  rename(0); f.variables.clear(); f.ssa = true;
}

void inlineCalls(ModuleIR &module) {
  std::map<const Function *, FunctionIR> candidates;
  for (const auto &f : module.functions) {
    size_t count = 0; bool leaf = true;
    for (const auto &b : f.blocks) for (const auto &i : b.instructions) { ++count; leaf &= i.op != Op::Call; }
    if (leaf && count <= 160 && f.blocks.size() <= 24) candidates.emplace(f.source, f);
  }
  for (auto &f : module.functions) {
    size_t budget = 3000;
    for (size_t b = 0; b < f.blocks.size(); ++b) {
      auto &code = f.blocks[b].instructions;
      auto at = std::find_if(code.begin(), code.end(), [&](const Instruction &i) { return i.op == Op::Call && candidates.count(i.callee) && i.callee != f.source; });
      if (at == code.end() || budget == 0) continue;
      Instruction call = *at; const auto &callee = candidates.at(call.callee);
      size_t cost = 0; for (const auto &block : callee.blocks) cost += block.instructions.size();
      if (cost > 80 && loopDepth(f)[b] == 0) continue;
      if (cost > budget) continue;
      budget -= cost;
      std::vector<Instruction> before(code.begin(), at), after(at + 1, code.end());
      int continuation = f.blocks.size(); f.blocks.push_back({"inline.cont", std::move(after)});
      int entry = f.blocks.size();
      std::vector<int> values;
      for (auto type : callee.values) values.push_back(newValue(f, type));
      std::map<const Symbol *, const Symbol *> objects;
      for (const auto *s : callee.objects) {
        auto copy = std::make_unique<Symbol>(*s); copy->name += ".inl" + std::to_string(entry);
        auto pointer = copy.get(); module.temporaries.push_back(std::move(copy));
        objects[s] = pointer; f.objects.push_back(pointer);
      }
      for (const auto &block : callee.blocks) {
        BasicBlock copy{"inline." + block.name, {}};
        for (auto i : block.instructions) {
          for (int &a : i.args) a = values[a];
          if (i.result >= 0) i.result = values[i.result];
          if (i.symbol && objects.count(i.symbol)) i.symbol = objects.at(i.symbol);
          if (i.op == Op::Parameter) { i.op = Op::Move; i.args = {call.args.at(i.immediate)}; }
          if (i.op == Op::Return) {
            if (call.result >= 0) {
              Instruction move{Op::Move}; move.result = call.result; move.args = i.args; copy.instructions.push_back(std::move(move));
            }
            i = Instruction{Op::Jump}; i.target = continuation;
          } else {
            if (i.target >= 0) i.target += entry;
            if (i.otherwise >= 0) i.otherwise += entry;
          }
          copy.instructions.push_back(std::move(i));
        }
        f.blocks.push_back(std::move(copy));
      }
      for (int v : callee.variables) f.variables.push_back(values[v]);
      if (call.result >= 0) f.variables.push_back(call.result);
      Instruction enter{Op::Jump}; enter.target = entry; before.push_back(std::move(enter));
      f.blocks[b].instructions = std::move(before);
    }
  }
}

float fromBits(int64_t bits) { uint32_t raw = bits; float f; std::memcpy(&f, &raw, 4); return f; }
int64_t bits(float value) { uint32_t raw; std::memcpy(&raw, &value, 4); return raw; }
int64_t arithmeticBits(float value) { return std::isnan(value) ? 0x7fc00000 : bits(value); }

std::optional<int64_t> fold(const Instruction &i, const FunctionIR &f, const std::vector<const Instruction *> &defs) {
  for (int a : i.args) if (!defs[a] || defs[a]->op != Op::Constant) return {};
  if (i.args.empty()) return {};
  auto type = f.values[i.args[0]], to = i.result >= 0 ? f.values[i.result] : ValueType::Int;
  int64_t x = defs[i.args[0]]->immediate, y = i.args.size() > 1 ? defs[i.args[1]]->immediate : 0;
  if (i.op == Op::Neg) return type == ValueType::Float ? bits(-fromBits(x)) : static_cast<int32_t>(0u - static_cast<uint32_t>(x));
  if (i.op == Op::Convert) {
    if (type == ValueType::Int && to == ValueType::Float) return bits(static_cast<float>(static_cast<int32_t>(x)));
    if (type == ValueType::Float && to == ValueType::Int) {
      float value = fromBits(x);
      if (std::isfinite(value) && value >= -2147483648.0 && value < 2147483648.0) return static_cast<int32_t>(value);
    }
    if (type == ValueType::Float && to == ValueType::Double) {
      double value = fromBits(x); int64_t raw; std::memcpy(&raw, &value, 8);
      return std::isnan(value) ? INT64_C(0x7ff8000000000000) : raw;
    }
    return {};
  }
  if (i.args.size() != 2) return {};
  if (type == ValueType::Float) {
    float a = fromBits(x), b = fromBits(y);
    switch (i.op) {
    case Op::Add: return arithmeticBits(a + b); case Op::Sub: return arithmeticBits(a - b); case Op::Mul: return arithmeticBits(a * b); case Op::Div: return arithmeticBits(a / b);
    case Op::Eq: return a == b; case Op::Ne: return a != b; case Op::Lt: return a < b; case Op::Le: return a <= b; case Op::Gt: return a > b; case Op::Ge: return a >= b;
    default: return {};
    }
  }
  if (type != ValueType::Int) return {};
  int32_t a = x, b = y;
  switch (i.op) {
  case Op::Add: return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
  case Op::Sub: return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
  case Op::Mul: return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
  case Op::And: return a & b;
  case Op::Shr: return static_cast<int32_t>(static_cast<uint32_t>(a) >> (b & 31));
  case Op::Div: case Op::Rem:
    if (b == 0) return {};
    if (a == INT32_MIN && b == -1) return i.op == Op::Div ? INT32_MIN : 0;
    return i.op == Op::Div ? a / b : a % b;
  case Op::Eq: return a == b; case Op::Ne: return a != b; case Op::Lt: return a < b; case Op::Le: return a <= b; case Op::Gt: return a > b; case Op::Ge: return a >= b;
  default: return {};
  }
}

void simplify(FunctionIR &f) {
  if (!f.ssa) return;
  std::vector<int> aliases(f.values.size()); for (size_t v = 0; v < aliases.size(); ++v) aliases[v] = v;
  bool changed;
  do {
    changed = false; auto defs = definitions(f);
    for (auto &block : f.blocks) for (auto &i : block.instructions) {
      for (int &a : i.args) a = root(aliases, a);
      if (i.result < 0 || root(aliases, i.result) != i.result) continue;
      int replacement = -1;
      if (i.op == Op::Move) replacement = i.args[0];
      if (i.op == Op::Phi) {
        bool same = true;
        for (int a : i.args) if (a != i.result) { if (replacement < 0) replacement = a; else same &= replacement == a; }
        if (!same) replacement = -1;
      }
      if (i.op == Op::Offset && defs[i.args[1]] && defs[i.args[1]]->op == Op::Constant && defs[i.args[1]]->immediate == 0) replacement = i.args[0];
      if ((i.op == Op::Add || i.op == Op::Sub || i.op == Op::Mul) && f.values[i.result] == ValueType::Int) {
        auto b = defs[i.args[1]];
        if (b && b->op == Op::Constant && ((b->immediate == 0 && i.op != Op::Mul) || (b->immediate == 1 && i.op == Op::Mul))) replacement = i.args[0];
      }
      if (replacement >= 0 && replacement != i.result) { aliases[i.result] = replacement; changed = true; continue; }
      if (i.op != Op::Constant && i.op != Op::Phi && !effect(i.op)) if (auto value = fold(i, f, defs)) {
        i.op = Op::Constant; i.immediate = *value; i.args.clear(); i.symbol = nullptr; changed = true;
      }
    }
    for (auto &b : f.blocks) for (auto &i : b.instructions) for (int &a : i.args) a = root(aliases, a);
  } while (changed);
  for (auto &b : f.blocks) for (auto &i : b.instructions) {
    if (i.result >= 0 && aliases[i.result] != i.result) { i.op = Op::Move; i.args = {root(aliases, i.result)}; i.edges.clear(); }
  }
}

void deadSSA(FunctionIR &f) {
  auto defs = definitions(f); std::vector<bool> live(f.values.size(), false);
  std::vector<int> work;
  for (const auto &b : f.blocks) for (const auto &i : b.instructions) if (effect(i.op)) work.insert(work.end(),i.args.begin(),i.args.end());
  while (!work.empty()) {
    int v = work.back(); work.pop_back();
    if (live[v]) continue;
    live[v] = true; if (defs[v]) work.insert(work.end(),defs[v]->args.begin(),defs[v]->args.end());
  }
  for (auto &b : f.blocks) b.instructions.erase(std::remove_if(b.instructions.begin(), b.instructions.end(), [&](const Instruction &i) {
    return !effect(i.op) && i.result >= 0 && !live[i.result];
  }), b.instructions.end());
}

void sccp(FunctionIR &f) {
  if (!f.ssa) return;
  // Bottom / constant / overdefined, with executable edges tracked separately.
  struct Lattice { int kind = 0; int64_t value = 0; };
  std::vector<Lattice> lattice(f.values.size());
  std::vector<Instruction> constants(f.values.size(), Instruction{Op::Constant});
  std::vector<const Instruction *> known(f.values.size(), nullptr);
  std::vector<bool> executable(f.blocks.size(), false); executable[0] = true;
  std::set<std::pair<int, int>> edges;
  using Site = std::pair<int,int>;
  std::vector<std::vector<Site>> users(f.values.size());
  std::vector<std::vector<bool>> pending(f.blocks.size());
  std::queue<Site> work;
  auto enqueue = [&](int b, int n) {
    if (executable[b] && !pending[b][n]) { pending[b][n] = true; work.push({b,n}); }
  };
  for (size_t b = 0; b < f.blocks.size(); ++b) {
    pending[b].resize(f.blocks[b].instructions.size(),false);
    for (size_t n = 0; n < pending[b].size(); ++n)
      for (int a : f.blocks[b].instructions[n].args) users[a].push_back({b,n});
  }
  for (size_t n = 0; n < pending[0].size(); ++n) enqueue(0,n);
  auto merge = [&](int v, Lattice next) {
    auto &old = lattice[v];
    if (old.kind == 2 || next.kind == 0) return;
    if (old.kind == 1 && next.kind == 1 && old.value != next.value) next.kind = 2;
    if (old.kind == next.kind) return;
    old = next;
    if (old.kind == 1) { constants[v].immediate = old.value; known[v] = &constants[v]; }
    else known[v] = nullptr;
    for (auto [b,n] : users[v]) enqueue(b,n);
  };
  while (!work.empty()) {
    auto [b,n] = work.front(); work.pop(); pending[b][n] = false;
    const auto &i = f.blocks[b].instructions[n];
    if (i.result >= 0) {
      Lattice next;
      if (i.op == Op::Constant) next = {1,i.immediate};
      else if (i.op == Op::Move) next = lattice[i.args[0]];
      else if (i.op == Op::Phi) {
        for (size_t p = 0; p < i.args.size(); ++p) if (edges.count({i.edges[p],b})) {
          auto incoming = lattice[i.args[p]];
          if (!incoming.kind) continue;
          if (!next.kind) next = incoming;
          else if (incoming.kind == 2 || (next.kind == 1 && incoming.value != next.value)) next.kind = 2;
        }
      } else if (i.op == Op::Parameter || i.op == Op::Load || i.op == Op::Call || i.op == Op::Address || i.op == Op::String || i.op == Op::Offset) next.kind = 2;
      else {
        bool all = true, over = false;
        for (int a : i.args) { all &= lattice[a].kind == 1; over |= lattice[a].kind == 2; }
        if (all) { auto value = fold(i,f,known); next = value ? Lattice{1,*value} : Lattice{2,0}; }
        else if (over) next.kind = 2;
      }
      merge(i.result,next);
    }
    auto activate = [&](int successor) {
      if (successor < 0 || !edges.emplace(b,successor).second) return;
      bool first = !executable[successor]; executable[successor] = true;
      for (size_t p = 0; p < pending[successor].size(); ++p)
        if (first || f.blocks[successor].instructions[p].op == Op::Phi) enqueue(successor,p);
    };
    if (i.op == Op::Jump) activate(i.target);
    if (i.op == Op::Branch) {
      auto test = lattice[i.args[0]];
      if (test.kind == 1) activate(test.value ? i.target : i.otherwise);
      else if (test.kind == 2) { activate(i.target); activate(i.otherwise); }
    }
  }
  for (auto &b : f.blocks) for (auto &i : b.instructions) {
    if (i.result >= 0 && lattice[i.result].kind == 1 && !effect(i.op)) {
      i.op = Op::Constant; i.immediate = lattice[i.result].value; i.args.clear(); i.edges.clear(); i.symbol = nullptr;
    }
    if (i.op == Op::Branch && lattice[i.args[0]].kind == 1) {
      i.target = lattice[i.args[0]].value ? i.target : i.otherwise;
      i.op = Op::Jump; i.args.clear(); i.otherwise = -1;
    }
  }
}

struct ExpressionKey {
  Op op; ValueType type; int64_t immediate;
  const Symbol *symbol; const Function *callee;
  std::string text; std::vector<int> args;
  bool operator==(const ExpressionKey &other) const {
    return op == other.op && type == other.type && immediate == other.immediate &&
        symbol == other.symbol && callee == other.callee && text == other.text && args == other.args;
  }
};
struct ExpressionHash {
  size_t operator()(const ExpressionKey &k) const {
    size_t h = static_cast<size_t>(k.op);
    auto mix = [&](size_t v) { h ^= v + 0x9e3779b97f4a7c15ull+(h<<6)+(h>>2); };
    mix(static_cast<size_t>(k.type)); mix(k.immediate); mix(reinterpret_cast<uintptr_t>(k.symbol));
    mix(reinterpret_cast<uintptr_t>(k.callee)); mix(std::hash<std::string>{}(k.text));
    for (int a : k.args) mix(a);
    return h;
  }
};
ExpressionKey expressionKey(const Instruction &i, const FunctionIR &f) {
  ExpressionKey key{i.op,f.values[i.result],i.immediate,i.symbol,i.callee,i.text,i.args};
  if (key.args.size() == 2 && (i.op == Op::Eq || i.op == Op::Ne ||
      ((i.op == Op::Add || i.op == Op::Mul) && f.values[i.result] == ValueType::Int)) && key.args[0] > key.args[1]) std::swap(key.args[0],key.args[1]);
  return key;
}
void gvn(FunctionIR &f, const Summaries &summaries) {
  if (!f.ssa) return;
  const auto &graph = controlFlow(f); std::vector<int> aliases(f.values.size());
  for (size_t v = 0; v < aliases.size(); ++v) aliases[v] = v;
  std::unordered_map<ExpressionKey,int,ExpressionHash> expressions;
  std::function<void(int)> visit = [&](int b) {
    std::map<int, int> memory; std::vector<ExpressionKey> insertedKeys;
    for (auto &i : f.blocks[b].instructions) {
      for (int &a : i.args) a = root(aliases, a);
      if (i.op == Op::Store) { memory.clear(); memory[i.args[0]] = i.args[1]; }
      if (i.op == Op::Zero || (i.op == Op::Call && !constantCall(i, summaries))) memory.clear();
      if (i.result < 0) continue;
      if (i.op == Op::Move) { aliases[i.result] = i.args[0]; continue; }
      if (i.op == Op::Load) {
        auto it = memory.find(i.args[0]);
        if (it != memory.end()) aliases[i.result] = it->second;
        else memory[i.args[0]] = i.result;
        continue;
      }
      if (i.op == Op::Phi || i.op == Op::Parameter || (i.op == Op::Call && !constantCall(i, summaries))) continue;
      auto key = expressionKey(i,f);
      auto inserted = expressions.emplace(key, i.result);
      if (!inserted.second) aliases[i.result] = inserted.first->second;
      else insertedKeys.push_back(std::move(key));
    }
    for (int child : graph.children[b]) visit(child);
    for (const auto &key : insertedKeys) expressions.erase(key);
  };
  visit(0);
  for (auto &b : f.blocks) for (auto &i : b.instructions) {
    for (int &a : i.args) a = root(aliases, a);
    if (i.result >= 0 && aliases[i.result] != i.result) { i.op = Op::Move; i.args = {root(aliases, i.result)}; i.edges.clear(); }
  }
}

struct Loop { int header; std::set<int> blocks; };

// Whole-memory SSA keeps potentially aliasing parameters in one chain. The
// walker skips writes proven disjoint by byte ranges, provenance and summaries.
struct MemoryNode {
  enum Kind { Entry, Def, Phi } kind = Entry;
  int block = 0, position = -1, previous = 0;
  Regions writes;
  int pointer = -1;
  uint64_t size = 0;
  std::vector<int> inputs;
};
struct MemorySSA {
  std::vector<MemoryNode> nodes{MemoryNode{}};
  std::vector<std::vector<int>> before;
  std::vector<int> incoming, outgoing, phis;
  const AliasAnalysis &alias;
  mutable std::map<std::tuple<int,Regions,int,uint64_t>,int> answers;
  MemorySSA(const FunctionIR &f, const Summaries &summaries, const AliasAnalysis &analysis) : alias(analysis) {
    const auto &roots = alias.roots;
    const auto &g = controlFlow(f);
    size_t count = f.blocks.size();
    before.resize(count); incoming.assign(count, 0); outgoing.assign(count, 0); phis.assign(count, 0);
    std::vector<std::vector<int>> defs(count);
    for (size_t b = 0; b < count; ++b) {
      if (g.pred[b].size() > 1) {
        phis[b] = nodes.size(); MemoryNode phi; phi.kind = MemoryNode::Phi; phi.block = b;
        phi.inputs.resize(g.pred[b].size()); nodes.push_back(std::move(phi));
      }
      before[b].resize(f.blocks[b].instructions.size()); defs[b].resize(before[b].size());
      for (size_t n = 0; n < before[b].size(); ++n) {
        const auto &i = f.blocks[b].instructions[n]; Regions writes;
        if (i.op == Op::Store || i.op == Op::Zero) writes = roots[i.args[0]];
        else if (i.op == Op::Call) writes = callEffects(i, roots, summaries).writes;
        if (!writes.empty()) {
          defs[b][n] = nodes.size(); MemoryNode d; d.kind = MemoryNode::Def; d.block = b;
          d.position = n; d.writes = std::move(writes);
          if (i.op == Op::Store || i.op == Op::Zero) {
            d.pointer = i.args[0]; d.size = i.op == Op::Zero ? static_cast<uint64_t>(i.immediate) : accessSize(f.values[i.args[1]]);
          }
          nodes.push_back(std::move(d));
        }
      }
    }
    bool changed;
    do {
      changed = false;
      for (int b : g.order) {
        int v = phis[b] ? phis[b] : b == 0 || g.pred[b].empty() ? 0 : outgoing[g.pred[b][0]];
        incoming[b] = v;
        for (size_t n = 0; n < defs[b].size(); ++n) {
          before[b][n] = v;
          if (defs[b][n]) { nodes[defs[b][n]].previous = v; v = defs[b][n]; }
        }
        if (v != outgoing[b]) { outgoing[b] = v; changed = true; }
      }
    } while (changed);
    for (size_t b = 0; b < count; ++b) if (phis[b])
      for (size_t n = 0; n < g.pred[b].size(); ++n) nodes[phis[b]].inputs[n] = outgoing[g.pred[b][n]];
    for (size_t id = 1; id < nodes.size(); ++id) {
      const auto &node = nodes[id];
      if (node.kind == MemoryNode::Def && node.previous == static_cast<int>(id))
        throw std::logic_error("self-referencing memory definition");
    }
  }
  int clobber(int version, const Regions &reads, int pointer = -1, uint64_t size = 0) const {
    if (reads.empty()) return 0;
    auto key = std::make_tuple(version,reads,pointer,size); auto cached = answers.find(key);
    if (cached != answers.end()) return cached->second;
    std::set<int> terminal; std::vector<int> work{version};
    std::vector<bool> visited(nodes.size(), false); int firstPhi = -1;
    while (!work.empty()) {
      int v = work.back(); work.pop_back();
      if (visited[v]) continue;
      visited[v] = true; const auto &node = nodes[v];
      bool conflict = pointer >= 0 && node.pointer >= 0 ? alias.mayAlias(pointer,size,node.pointer,node.size) : overlaps(node.writes,reads);
      if (node.kind == MemoryNode::Entry || (node.kind == MemoryNode::Def && conflict)) terminal.insert(v);
      else if (node.kind == MemoryNode::Def) work.push_back(node.previous);
      else {
        if (firstPhi < 0) firstPhi = v;
        work.insert(work.end(), node.inputs.begin(), node.inputs.end());
      }
    }
    int answer = terminal.size() == 1 ? *terminal.begin() : firstPhi < 0 ? version : firstPhi;
    answers.emplace(std::move(key),answer); return answer;
  }
};

void memoryForward(FunctionIR &f, const Summaries &summaries) {
  if (!f.ssa) return;
  const auto &graph = controlFlow(f); AliasAnalysis alias(f); const auto &roots = alias.roots;
  MemorySSA memory(f, summaries, alias);
  using Key = std::tuple<PointerLocation, ValueType, int>;
  std::map<Key, int> loads;
  std::map<std::string, int> calls;
  std::unordered_map<ExpressionKey,int,ExpressionHash> expressions;
  std::vector<int> aliases(f.values.size());
  for (size_t v = 0; v < aliases.size(); ++v) aliases[v] = v;
  std::function<void(int)> visit = [&](int b) {
    std::vector<Key> addedLoads; std::vector<std::string> addedCalls;
    std::vector<ExpressionKey> addedExpressions;
    for (size_t n = 0; n < f.blocks[b].instructions.size(); ++n) {
      auto &i = f.blocks[b].instructions[n];
      for (int &a : i.args) a = root(aliases,a);
      if (i.op == Op::Load) {
        uint64_t size = accessSize(f.values[i.result]);
        int version = memory.clobber(memory.before[b][n], roots[i.args[0]], i.args[0], size);
        Key key{alias.locations[i.args[0]], f.values[i.result], version}; int available = -1;
        auto previous = loads.find(key);
        if (previous != loads.end()) available = previous->second;
        const auto &node = memory.nodes[version];
        if (available < 0 && node.kind == MemoryNode::Def && graph.dominates(node.block, b) &&
            (node.block != b || node.position < static_cast<int>(n))) {
          const auto &store = f.blocks[node.block].instructions[node.position];
          if (store.op == Op::Store && alias.covers(store.args[0],accessSize(f.values[store.args[1]]),i.args[0],size) &&
              f.values[store.args[1]] == f.values[i.result]) available = store.args[1];
        }
        if (available >= 0) aliases[i.result] = root(aliases,available);
        else { loads.emplace(key, i.result); addedLoads.push_back(key); }
      } else if (i.op == Op::Call && i.result >= 0) {
        auto e = callEffects(i, roots, summaries);
        if (e.observable || !e.writes.empty()) continue;
        std::ostringstream text; text << i.callee << ':' << memory.clobber(memory.before[b][n], e.reads);
        for (int a : i.args) text << ':' << a;
        auto key = text.str(); auto previous = calls.find(key);
        if (previous != calls.end()) aliases[i.result] = root(aliases,previous->second);
        else { calls.emplace(key, i.result); addedCalls.push_back(key); }
      } else if (i.result >= 0) {
        if (i.op == Op::Move) aliases[i.result] = i.args[0];
        else if (i.op != Op::Phi && i.op != Op::Parameter) {
          auto key = expressionKey(i,f); auto inserted = expressions.emplace(key,i.result);
          if (!inserted.second) aliases[i.result] = root(aliases,inserted.first->second);
          else addedExpressions.push_back(std::move(key));
        }
      }
    }
    for (int child : graph.children[b]) visit(child);
    for (const auto &key : addedLoads) loads.erase(key);
    for (const auto &key : addedCalls) calls.erase(key);
    for (const auto &key : addedExpressions) expressions.erase(key);
  };
  visit(0);
  for (auto &b : f.blocks) for (auto &i : b.instructions) {
    for (int &a : i.args) a = root(aliases,a);
    if (i.result >= 0 && aliases[i.result] != i.result) {
      i.op = Op::Move; i.args = {root(aliases,i.result)}; i.edges.clear(); i.callee = nullptr;
    }
  }
}

// Bound local DSE work independently of source size. Each retained later write
// can kill an earlier fully covered store, until a possibly aliasing read or
// call makes that write unavailable. Partial zero fills never kill live bytes.
void deadStores(FunctionIR &f, const Summaries &summaries) {
  if (!f.ssa) return;
  bool candidate = false;
  for (const auto &b : f.blocks) {
    int writes = 0;
    for (const auto &i : b.instructions) if (i.op == Op::Store || i.op == Op::Zero) ++writes;
    candidate |= writes > 1;
  }
  if (!candidate) return;
  AliasAnalysis alias(f);
  for (auto &b : f.blocks) {
    if (std::count_if(b.instructions.begin(),b.instructions.end(),[](const Instruction &i) { return i.op == Op::Store || i.op == Op::Zero; }) < 2) continue;
    struct Write { int pointer; uint64_t size; };
    std::vector<Write> later; std::vector<bool> dead(b.instructions.size(),false);
    for (size_t n = b.instructions.size(); n-- > 0;) {
      const auto &i = b.instructions[n];
      if (i.op == Op::Load) {
        later.erase(std::remove_if(later.begin(),later.end(),[&](Write w) {
          return alias.mayAlias(w.pointer,w.size,i.args[0],accessSize(f.values[i.result]));
        }),later.end());
      } else if (i.op == Op::Call) {
        auto e = callEffects(i,alias.roots,summaries); e.reads.insert(e.writes.begin(),e.writes.end());
        later.erase(std::remove_if(later.begin(),later.end(),[&](Write w) { return overlaps(alias.roots[w.pointer],e.reads); }),later.end());
      } else if (i.op == Op::Store || i.op == Op::Zero) {
        uint64_t size = i.op == Op::Zero ? static_cast<uint64_t>(i.immediate) : accessSize(f.values[i.args[1]]);
        dead[n] = !size || std::any_of(later.begin(),later.end(),[&](Write w) { return alias.covers(w.pointer,w.size,i.args[0],size); });
        if (!dead[n]) {
          if (later.size() == 64) later.erase(later.begin());
          later.push_back({i.args[0],size});
        }
      }
    }
    size_t n = 0;
    b.instructions.erase(std::remove_if(b.instructions.begin(),b.instructions.end(),[&](const Instruction &) { return dead[n++]; }),b.instructions.end());
  }
}

std::vector<Loop> loops(const Graph &graph) {
  std::map<int, std::set<int>> headers;
  for (int tail : graph.order) for (int h : graph.succ[tail]) if (graph.dominates(h, tail)) {
    auto &members = headers[h]; members.insert(h);
    std::vector<int> work{tail};
    while (!work.empty()) {
      int b = work.back(); work.pop_back();
      if (!members.insert(b).second) continue;
      for (int p : graph.pred[b]) if (p != h) work.push_back(p);
    }
  }
  std::vector<Loop> result;
  for (auto &entry : headers) result.push_back({entry.first, std::move(entry.second)});
  std::sort(result.begin(), result.end(), [](const Loop &a, const Loop &b) { return a.blocks.size() < b.blocks.size(); });
  return result;
}

void rotate(FunctionIR &f) {
  if (!f.ssa) return;
  auto nests = loops(controlFlow(f));
  for (const auto &original : nests) {
    const auto &graph = controlFlow(f);
    auto current = loops(graph);
    auto found = std::find_if(current.begin(), current.end(), [&](const Loop &l) { return l.header == original.header; });
    if (found == current.end()) continue;
    const auto loop = *found;
    const auto code = f.blocks[loop.header].instructions;
    if (code.size() < 2) continue;
    const auto compare = code[code.size()-2], branch = code.back();
    if (branch.op != Op::Branch || !loop.blocks.count(branch.target) || loop.blocks.count(branch.otherwise) ||
        compare.result != branch.args[0] || compare.op < Op::Eq || compare.op > Op::Ge) continue;
    bool valid = true;
    for (size_t n = 0; n+2 < code.size(); ++n) valid &= code[n].op == Op::Phi && code[n].edges.size() == 2;
    int entry = -1, latch = -1;
    for (int p : graph.pred[loop.header]) (loop.blocks.count(p) ? latch : entry) = p;
    int body = branch.target, exit = branch.otherwise;
    valid &= entry >= 0 && latch >= 0 && graph.pred[loop.header].size() == 2 && graph.succ[entry].size() == 1 &&
        graph.pred[body].size() == 1 && graph.pred[exit].size() == 1;
    for (int b : loop.blocks) {
      valid &= f.blocks[b].instructions.back().op != Op::Return;
      for (int s : graph.succ[b]) if (!loop.blocks.count(s)) valid &= s == exit;
    }
    if (!valid) continue;
    // Comparisons may only feed the loop test. The general case requires a
    // second value Phi for the cloned comparison, so leave it untouched.
    int uses = 0;
    for (const auto &b : f.blocks) for (const auto &i : b.instructions) for (int a : i.args) uses += a == compare.result;
    if (uses != 1) continue;
    std::map<int, int> initial, back, after;
    for (size_t n = 0; n+2 < code.size(); ++n) {
      const auto &phi = code[n]; int outside = phi.edges[0] == entry ? 0 : 1;
      initial[phi.result] = phi.args[outside]; back[phi.result] = phi.args[1-outside];
    }
    // A comparison operand must be a header Phi or a dominating invariant.
    for (int a : compare.args) if (!initial.count(a)) {
      for (size_t n = 0; n+2 < code.size(); ++n) valid &= code[n].result != a;
    }
    if (!valid) continue;
    int preheader = f.blocks.size(); Instruction jump{Op::Jump}; jump.target = body;
    f.blocks.push_back({"rotated.preheader", {jump}});
    auto first = compare; first.result = newValue(f, ValueType::Int);
    for (int &a : first.args) if (initial.count(a)) a = initial.at(a);
    auto guard = branch; guard.args = {first.result}; guard.target = preheader;
    auto &enter = f.blocks[entry].instructions; enter.back() = first; enter.push_back(guard);
    std::vector<Instruction> phis;
    for (size_t n = 0; n+2 < code.size(); ++n) {
      const auto &old = code[n]; auto phi = old;
      phi.edges = {preheader, loop.header}; phi.args = {initial.at(old.result), back.at(old.result)}; phis.push_back(phi);
      auto final = phi; final.result = newValue(f, f.values[old.result]); final.edges = {entry, loop.header};
      after[old.result] = final.result; f.blocks[exit].instructions.insert(f.blocks[exit].instructions.begin(), final);
    }
    auto &bodyCode = f.blocks[body].instructions; bodyCode.insert(bodyCode.begin(), phis.begin(), phis.end());
    auto last = compare; for (int &a : last.args) if (back.count(a)) a = back.at(a);
    f.blocks[loop.header].instructions = {last, branch};
    for (size_t b = 0; b < f.blocks.size(); ++b) if (!loop.blocks.count(b) && static_cast<int>(b) != entry && static_cast<int>(b) != preheader) {
      for (auto &i : f.blocks[b].instructions) {
        if (static_cast<int>(b) == exit && i.op == Op::Phi && std::any_of(after.begin(), after.end(), [&](auto pair) { return pair.second == i.result; })) continue;
        for (int &a : i.args) if (after.count(a)) a = after.at(a);
      }
    }
  }
}

void unrollRuntime(FunctionIR &f, int h, int test, const std::vector<Instruction> &body,
    const std::vector<Instruction> &phis, size_t first, int iv, const Instruction &compare) {
  const auto &graph = controlFlow(f);
  int pre = -1;
  for (int p : graph.pred[h]) if (p != test) pre = p;
  if (pre < 0 || graph.succ[pre].size() != 1 || body.size()-first-1 > 32) return;
  int bound = compare.args[1], site = -1;
  for (size_t b = 0; b < f.blocks.size(); ++b) for (const auto &i : f.blocks[b].instructions) if (i.result == bound) site = b;
  if (site < 0 || !graph.dominates(site, pre)) return;
  for (const auto &i : body) if (i.op == Op::Call) return;
  for (const auto &phi : phis) if (phi.edges.size() != 2 || std::find(phi.edges.begin(), phi.edges.end(), pre) == phi.edges.end()) return;
  int main = f.blocks.size(), check = main+1, gate = main+2;
  int exit = f.blocks[test].instructions.back().otherwise;
  std::map<int, int> remap, initial, carried, gateState;
  auto value = [&](int v) { auto it = remap.find(v); return it == remap.end() ? v : it->second; };
  std::vector<Instruction> mainPhis, mainCode, gateCode;
  for (const auto &old : phis) {
    int edge = old.edges[0] == pre ? 0 : 1;
    initial[old.result] = old.args[edge]; carried[old.result] = old.args[1-edge];
    auto phi = old; phi.result = newValue(f, f.values[old.result]); phi.edges = {pre, check}; phi.args = {old.args[edge], -1};
    remap[old.result] = phi.result; mainPhis.push_back(phi);
  }
  for (int iteration = 0; iteration < 4; ++iteration) {
    if (iteration) {
      std::map<int, int> next;
      for (auto pair : carried) next[pair.first] = value(pair.second);
      for (auto pair : next) remap[pair.first] = pair.second;
    }
    for (size_t n = first; n+1 < body.size(); ++n) {
      auto i = body[n]; for (int &a : i.args) a = value(a);
      if (i.result >= 0) { int old = i.result; i.result = newValue(f, f.values[old]); remap[old] = i.result; }
      mainCode.push_back(std::move(i));
    }
  }
  for (size_t n = 0; n < phis.size(); ++n) {
    mainPhis[n].args[1] = value(carried.at(phis[n].result));
    Instruction phi{Op::Phi}; phi.result = newValue(f, f.values[phis[n].result]);
    phi.edges = {pre, check}; phi.args = {initial.at(phis[n].result), mainPhis[n].args[1]};
    gateState[phis[n].result] = phi.result; gateCode.push_back(phi);
  }
  // Loop-closed values select the final unrolled result when there is no tail.
  for (auto &phi : f.blocks[exit].instructions) if (phi.op == Op::Phi) {
    auto edge = std::find(phi.edges.begin(), phi.edges.end(), test);
    if (edge == phi.edges.end()) continue;
    size_t n = edge-phi.edges.begin(), other = n == 0 ? 1 : 0;
    Instruction result{Op::Phi}; result.result = newValue(f, f.values[phi.result]); result.edges = {pre, check};
    result.args = {phi.args[other], value(phi.args[n])}; gateCode.push_back(result);
    phi.edges.push_back(gate); phi.args.push_back(result.result);
  }
  for (auto &phi : f.blocks[h].instructions) if (phi.op == Op::Phi) for (size_t n = 0; n < phi.edges.size(); ++n) if (phi.edges[n] == pre) {
    phi.edges[n] = gate; phi.args[n] = gateState.at(phi.result);
  }
  // The rotated entry guard proves bound > initial >= 0. bound-4 therefore
  // cannot wrap; each main iteration has four valid scalar iterations.
  Instruction four{Op::Constant}; four.result = newValue(f, ValueType::Int); four.immediate = 4;
  Instruction threshold{Op::Sub}; threshold.result = newValue(f, ValueType::Int); threshold.args = {bound, four.result};
  Instruction enough{Op::Le}; enough.result = newValue(f, ValueType::Int); enough.args = {initial.at(iv), threshold.result};
  Instruction enter{Op::Branch}; enter.args = {enough.result}; enter.target = main; enter.otherwise = gate;
  auto &preCode = f.blocks[pre].instructions; preCode.pop_back();
  preCode.insert(preCode.end(), {four, threshold, enough, enter});
  Instruction jump{Op::Jump}; jump.target = check;
  mainPhis.insert(mainPhis.end(), mainCode.begin(), mainCode.end()); mainPhis.push_back(jump);
  auto again = enough; again.result = newValue(f, ValueType::Int); again.args[0] = value(carried.at(iv));
  auto repeat = enter; repeat.args = {again.result};
  auto tail = compare; tail.result = newValue(f, ValueType::Int); tail.args[0] = gateState.at(iv);
  auto remainder = enter; remainder.args = {tail.result}; remainder.target = h; remainder.otherwise = exit;
  gateCode.push_back(tail); gateCode.push_back(remainder);
  f.blocks.push_back({"unroll.main", std::move(mainPhis)});
  f.blocks.push_back({"unroll.test", {again, repeat}});
  f.blocks.push_back({"unroll.tail", std::move(gateCode)});
}

void unroll(FunctionIR &f) {
  if (!f.ssa) return;
  auto nests = loops(controlFlow(f));
  for (const auto &loop : nests) {
    if (loop.blocks.size() != 2) continue;
    auto defs = definitions(f);
    int h = loop.header; const auto body = f.blocks[h].instructions;
    if (body.back().op != Op::Jump) continue;
    int test = body.back().target;
    if (test == h || !loop.blocks.count(test)) continue;
    const auto &condition = f.blocks[test].instructions;
    if (condition.size() != 2 || condition.back().op != Op::Branch || condition.back().target != h ||
        loop.blocks.count(condition.back().otherwise)) continue;
    const auto compare = condition[0];
    if ((compare.op != Op::Lt && compare.op != Op::Le) || f.values[compare.args[0]] != ValueType::Int) continue;
    auto limit = defs[compare.args[1]], update = defs[compare.args[0]];
    if (!limit || !update || update->op != Op::Add) continue;
    int iv = -1;
    for (int n = 0; n < 2; ++n) {
      auto step = defs[update->args[n]], phi = defs[update->args[1-n]];
      if (step && step->op == Op::Constant && step->immediate == 1 && phi && phi->op == Op::Phi) iv = phi->result;
    }
    if (iv < 0) continue;
    std::vector<Instruction> phis;
    size_t first = 0;
    while (first < body.size() && body[first].op == Op::Phi) phis.push_back(body[first++]);
    auto induction = std::find_if(phis.begin(), phis.end(), [&](const Instruction &i) { return i.result == iv; });
    if (induction == phis.end() || induction->edges.size() != 2) continue;
    int incoming = induction->edges[0] == test ? 1 : 0;
    auto start = defs[induction->args[incoming]];
    if (!start || start->op != Op::Constant || induction->args[1-incoming] != update->result) continue;
    if (limit->op != Op::Constant) {
      if (compare.op == Op::Lt && start->immediate >= 0) unrollRuntime(f,h,test,body,phis,first,iv,compare);
      continue;
    }
    int64_t end = limit->immediate + (compare.op == Op::Le);
    int64_t trips = end - start->immediate;
    if (trips <= 1 || end > INT32_MAX) continue;
    size_t cost = body.size() - first - 1;
    int factor = 1;
    for (int candidate : {4, 3, 2, 5}) if (trips % candidate == 0 && cost * candidate <= 160) { factor = candidate; break; }
    if (factor == 1 || std::any_of(body.begin()+first, body.end(), [](const Instruction &i) { return i.op == Op::Call; })) continue;
    bool valid = true;
    for (const auto &phi : phis) valid &= phi.edges.size() == 2 && std::find(phi.edges.begin(), phi.edges.end(), test) != phi.edges.end();
    if (!valid) continue;
    std::map<int, int> remap;
    auto value = [&](int v) { auto it = remap.find(v); return it == remap.end() ? v : it->second; };
    std::vector<Instruction> expanded(body.begin(), body.end()-1);
    for (int iteration = 1; iteration < factor; ++iteration) {
      std::map<int, int> carried;
      for (const auto &phi : phis) {
        size_t edge = std::find(phi.edges.begin(), phi.edges.end(), test) - phi.edges.begin();
        carried[phi.result] = value(phi.args[edge]);
      }
      for (auto pair : carried) remap[pair.first] = pair.second;
      for (size_t n = first; n+1 < body.size(); ++n) {
        auto i = body[n]; for (int &a : i.args) a = value(a);
        if (i.result >= 0) { int old = i.result; i.result = newValue(f, f.values[old]); remap[old] = i.result; }
        expanded.push_back(std::move(i));
      }
    }
    expanded.push_back(body.back());
    for (auto &phi : expanded) if (phi.op == Op::Phi) for (size_t n = 0; n < phi.edges.size(); ++n) if (phi.edges[n] == test) phi.args[n] = value(phi.args[n]);
    f.blocks[h].instructions = std::move(expanded);
    for (size_t b = 0; b < f.blocks.size(); ++b) if (static_cast<int>(b) != h)
      for (auto &i : f.blocks[b].instructions) for (int &a : i.args) a = value(a);
  }
}

void tailRecursion(FunctionIR &f) {
  if (!f.ssa || !f.objects.empty()) return;
  struct Tail { int block; size_t position; Instruction call; };
  std::vector<Tail> tails;
  for (size_t b = 0; b < f.blocks.size(); ++b) {
    const auto &code = f.blocks[b].instructions;
    if (code.size() < 2) continue;
    size_t pos = code.size()-2; const auto &call = code[pos];
    if (call.op != Op::Call || call.callee != f.source) continue;
    Instruction terminator = code.back(); std::set<int> seen;
    while (terminator.op == Op::Jump && seen.insert(terminator.target).second) {
      const auto &next = f.blocks[terminator.target].instructions;
      if (next.size() != 1) break;
      terminator = next.back();
    }
    if (terminator.op != Op::Return) continue;
    bool valid = f.source->result == Base::Void ? terminator.args.empty() : terminator.args.size() == 1 && terminator.args[0] == call.result;
    if (valid) tails.push_back({static_cast<int>(b), pos, call});
  }
  if (tails.empty()) return;
  const auto entry = f.blocks[0].instructions;
  int header = f.blocks.size();
  std::vector<Instruction> parameters, body, phis;
  for (auto i : entry) {
    if (i.op != Op::Parameter) { body.push_back(i); continue; }
    Instruction phi{Op::Phi}; phi.result = i.result;
    i.result = newValue(f, f.values[i.result]); phi.args = {i.result}; phi.edges = {0};
    for (const auto &tail : tails) { phi.args.push_back(tail.call.args.at(i.immediate)); phi.edges.push_back(tail.block == 0 ? header : tail.block); }
    parameters.push_back(i); phis.push_back(phi);
  }
  for (auto &b : f.blocks) for (auto &i : b.instructions) {
    if (i.target == 0) i.target = header;
    if (i.otherwise == 0) i.otherwise = header;
    for (int &edge : i.edges) if (edge == 0) edge = header;
  }
  phis.insert(phis.end(), body.begin(), body.end()); f.blocks.push_back({"tail.header", std::move(phis)});
  for (const auto &tail : tails) {
    auto &code = f.blocks[tail.block == 0 ? header : tail.block].instructions;
    // Entry parameter instructions have moved out of a tail in block zero.
    size_t position = tail.position;
    if (tail.block == 0) position = tail.position + parameters.size() - std::count_if(entry.begin(), entry.begin()+tail.position, [](const Instruction &i) { return i.op == Op::Parameter; });
    code.erase(code.begin()+position, code.end());
    Instruction jump{Op::Jump}; jump.target = header; code.push_back(jump);
  }
  Instruction jump{Op::Jump}; jump.target = header; parameters.push_back(jump); f.blocks[0].instructions = std::move(parameters);
}

void arithmetic(FunctionIR &f) {
  if (!f.ssa) return;
  auto borrowed = definitions(f); std::vector<std::optional<Instruction>> defs(f.values.size());
  for (size_t v = 0; v < defs.size(); ++v) if (borrowed[v]) defs[v] = *borrowed[v];
  using Range = std::optional<std::pair<int64_t, int64_t>>;
  std::vector<Range> ranges(f.values.size());
  std::vector<int> sites(f.values.size(), -1);
  for (size_t b = 0; b < f.blocks.size(); ++b) for (const auto &i : f.blocks[b].instructions) if (i.result >= 0) {
    sites[i.result] = b;
    if (i.op == Op::Constant && f.values[i.result] == ValueType::Int) ranges[i.result] = {{i.immediate, i.immediate}};
    if (i.op >= Op::Eq && i.op <= Op::Ge) ranges[i.result] = {{0, 1}};
  }
  const auto &graph = controlFlow(f);
  for (const auto &loop : loops(graph)) {
    const auto &branch = f.blocks[loop.header].instructions.back();
    if (branch.op != Op::Branch || !loop.blocks.count(branch.target) || loop.blocks.count(branch.otherwise)) continue;
    auto compare = defs[branch.args[0]];
    if (!compare || compare->op != Op::Lt) continue;
    for (const auto &phi : f.blocks[loop.header].instructions) {
      if (phi.op != Op::Phi || phi.result != compare->args[0] || phi.args.size() != 2 || f.values[phi.result] != ValueType::Int) continue;
      int outside = loop.blocks.count(phi.edges[0]) ? 1 : 0;
      if (loop.blocks.count(phi.edges[outside])) continue;
      auto init = defs[phi.args[outside]], update = defs[phi.args[1-outside]];
      if (!init || init->op != Op::Constant || init->immediate < 0 || !update || update->op != Op::Add ||
          !graph.dominates(branch.target, sites[update->result])) continue;
      int step = update->args[0] == phi.result ? update->args[1] : update->args[1] == phi.result ? update->args[0] : -1;
      if (step < 0 || !defs[step] || defs[step]->op != Op::Constant || defs[step]->immediate != 1) continue;
      auto limit = defs[compare->args[1]];
      int64_t high = limit && limit->op == Op::Constant ? std::max(init->immediate, limit->immediate) : INT32_MAX;
      ranges[phi.result] = {{init->immediate, high}};
      // The update executes only on the true edge, where the old IV < INT_MAX.
      ranges[update->result] = {{1, high}};
    }
  }
  bool changed;
  do {
    changed = false;
    for (size_t v = 0; v < defs.size(); ++v) {
      if (!defs[v] || ranges[v] || f.values[v] != ValueType::Int) continue;
      const auto &i = *defs[v]; Range next;
      if (i.op == Op::Move) next = ranges[i.args[0]];
      if (i.op == Op::Phi && !i.args.empty()) {
        bool all = true; int64_t lo = INT32_MAX, hi = INT32_MIN;
        for (int a : i.args) { if (!ranges[a]) { all = false; break; } lo = std::min(lo, ranges[a]->first); hi = std::max(hi, ranges[a]->second); }
        if (all) next = {{lo, hi}};
      }
      if (i.op == Op::Add || i.op == Op::Sub || i.op == Op::Mul) {
        auto a = ranges[i.args[0]], b = ranges[i.args[1]];
        if (a && b) {
          int64_t lo = 0, hi = 0;
          if (i.op == Op::Add) { lo = a->first+b->first; hi = a->second+b->second; }
          if (i.op == Op::Sub) { lo = a->first-b->second; hi = a->second-b->first; }
          if (i.op == Op::Mul) {
            int64_t p[] = {a->first*b->first,a->first*b->second,a->second*b->first,a->second*b->second};
            lo = *std::min_element(p,p+4); hi = *std::max_element(p,p+4);
          }
          if (lo >= INT32_MIN && hi <= INT32_MAX) next = {{lo, hi}};
        }
      }
      if (i.op == Op::Rem && defs[i.args[1]] && defs[i.args[1]]->op == Op::Constant) {
        int64_t d = defs[i.args[1]]->immediate, magnitude = std::abs(d);
        if (magnitude) next = {{ranges[i.args[0]] && ranges[i.args[0]]->first >= 0 ? 0 : 1-magnitude, magnitude-1}};
      }
      if (next) { ranges[v] = next; changed = true; }
    }
  } while (changed);
  std::vector<bool> zeroTest(defs.size(), true);
  for (const auto &b : f.blocks) for (const auto &i : b.instructions) for (int a : i.args) {
    bool test = (i.op == Op::Eq || i.op == Op::Ne) && i.args.size() == 2;
    int other = test ? i.args[i.args[0] == a ? 1 : 0] : -1;
    zeroTest[a] = zeroTest[a] && test && defs[other] && defs[other]->op == Op::Constant && defs[other]->immediate == 0;
  }
  for (auto &b : f.blocks) {
    std::vector<Instruction> code;
    for (auto i : b.instructions) {
      if ((i.op == Op::Div || i.op == Op::Rem) && f.values[i.result] == ValueType::Int && defs[i.args[1]] && defs[i.args[1]]->op == Op::Constant) {
        int64_t divisor = defs[i.args[1]]->immediate, magnitude = std::abs(divisor);
        bool nonnegative = ranges[i.args[0]] && ranges[i.args[0]]->first >= 0;
        bool power = magnitude && !(magnitude & (magnitude-1));
        if (power && (nonnegative || (i.op == Op::Rem && zeroTest[i.result]))) {
          Instruction c{Op::Constant}; c.result = newValue(f, ValueType::Int);
          if (i.op == Op::Rem) c.immediate = magnitude-1;
          else for (int64_t n = magnitude; n > 1; n >>= 1) ++c.immediate;
          code.push_back(c); bool negative = divisor < 0 && i.op == Op::Div;
          i.op = i.op == Op::Rem ? Op::And : Op::Shr; i.args[1] = c.result;
          if (negative) {
            int result = i.result; i.result = newValue(f, ValueType::Int); code.push_back(i);
            Instruction negate{Op::Neg}; negate.result = result; negate.args = {i.result}; i = negate;
          }
        }
      }
      code.push_back(std::move(i));
    }
    b.instructions = std::move(code);
  }
}

void licm(FunctionIR &f, const Summaries &summaries) {
  if (!f.ssa) return;
  auto nests = loops(controlFlow(f));
  if (nests.empty()) return;
  AliasAnalysis alias(f); const auto &roots = alias.roots;
  for (const auto &original : nests) {
    const auto &graph = controlFlow(f); auto current = loops(graph);
    auto found = std::find_if(current.begin(), current.end(), [&](const Loop &l) { return l.header == original.header; });
    if (found == current.end()) continue;
    const auto loop = *found; std::vector<int> outside;
    for (int p : graph.pred[loop.header]) if (!loop.blocks.count(p)) outside.push_back(p);
    if (outside.size() != 1) continue;
    int preheader = outside[0];
    if (graph.succ[preheader].size() != 1) {
      int split = f.blocks.size(); auto &last = f.blocks[preheader].instructions.back();
      if (last.target == loop.header) last.target = split;
      if (last.otherwise == loop.header) last.otherwise = split;
      Instruction jump{Op::Jump}; jump.target = loop.header; f.blocks.push_back({"loop.preheader", {jump}});
      for (auto &i : f.blocks[loop.header].instructions) if (i.op == Op::Phi) for (int &edge : i.edges) if (edge == preheader) edge = split;
      preheader = split;
    }
    std::vector<int> site(f.values.size(), -1);
    for (size_t b = 0; b < f.blocks.size(); ++b) for (const auto &i : f.blocks[b].instructions) if (i.result >= 0) site[i.result] = b;
    Regions callWrites;
    std::vector<std::pair<int,uint64_t>> writes;
    for (int b : loop.blocks) for (const auto &i : f.blocks[b].instructions) {
      if (i.op == Op::Store || i.op == Op::Zero) writes.push_back({i.args[0],i.op == Op::Zero ? static_cast<uint64_t>(i.immediate) : accessSize(f.values[i.args[1]])});
      if (i.op == Op::Call) { auto e = callEffects(i, roots, summaries); callWrites.insert(e.writes.begin(), e.writes.end()); }
    }
    bool changed;
    do {
      changed = false;
      for (int b : loop.blocks) {
        auto &code = f.blocks[b].instructions;
        bool priorEffect = false;
        for (size_t n = 0; n < code.size();) {
          auto &i = code[n]; bool movable = i.result >= 0 && !effect(i.op) && i.op != Op::Phi && i.op != Op::Parameter;
          if (constantCall(i, summaries)) movable = b == loop.header && !priorEffect;
          for (int a : i.args) movable &= !loop.blocks.count(site[a]);
          if (i.op == Op::Load) {
            auto pointer = alias.locations[i.args[0]]; uint64_t size = accessSize(f.values[i.result]);
            // Speculate only fixed, valid object accesses. Unknown addresses
            // retain the existing must-execute guard, including zero trips.
            uint64_t extent = pointer.object && !pointer.object->parameter ? static_cast<uint64_t>(pointer.object->type.elements()) * 4 : 0;
            bool fixed = extent >= size && pointer.offset <= extent - size;
            bool clobbered = overlaps(roots[i.args[0]],callWrites) ||
                std::any_of(writes.begin(),writes.end(),[&](auto w) { return alias.mayAlias(i.args[0],size,w.first,w.second); });
            movable &= !clobbered && (fixed || (b == loop.header && !priorEffect));
          }
          if (movable) {
            site[i.result] = preheader; auto moved = std::move(i);
            code.erase(code.begin() + n);
            auto &pre = f.blocks[preheader].instructions; pre.insert(pre.end() - 1, std::move(moved)); changed = true;
          } else { priorEffect |= effect(i.op) && i.op != Op::Jump; ++n; }
        }
      }
    } while (changed);
  }
}
void compact(FunctionIR &f) {
  std::vector<int> ids(f.values.size(), -1); std::vector<ValueType> values;
  auto map = [&](int v) {
    if (ids[v] < 0) { ids[v] = values.size(); values.push_back(f.values[v]); }
    return ids[v];
  };
  std::set<const Symbol *> objects;
  for (auto &b : f.blocks) for (auto &i : b.instructions) {
    if (i.result >= 0) i.result = map(i.result);
    for (int &a : i.args) a = map(a);
    if (i.symbol) objects.insert(i.symbol);
  }
  for (int &v : f.variables) v = map(v);
  f.values = std::move(values);
  f.objects.erase(std::remove_if(f.objects.begin(), f.objects.end(), [&](const Symbol *s) { return !objects.count(s); }), f.objects.end());
}

void strength(FunctionIR &f) {
  if (!f.ssa) return;
  auto nests = loops(controlFlow(f));
  for (const auto &loop : nests) {
    const auto &graph = controlFlow(f); auto original = definitions(f);
    // Own the facts: insertion into blocks must not invalidate analysis pointers.
    std::vector<std::optional<Instruction>> defs(f.values.size());
    std::vector<int> site(f.values.size(), -1);
    for (size_t v = 0; v < original.size(); ++v) if (original[v]) defs[v] = *original[v];
    for (size_t b = 0; b < f.blocks.size(); ++b) for (const auto &i : f.blocks[b].instructions) if (i.result >= 0) site[i.result] = b;
    struct IV { int phi, initial, step, preheader, latch; int64_t low, high; };
    std::vector<IV> induction;
    const auto &branch = f.blocks[loop.header].instructions.back();
    if (branch.op != Op::Branch || !loop.blocks.count(branch.target) || loop.blocks.count(branch.otherwise)) continue;
    auto compare = defs[branch.args[0]];
    if (!compare || compare->op != Op::Lt) continue;
    for (const auto &phi : f.blocks[loop.header].instructions) {
      if (phi.op != Op::Phi || f.values[phi.result] != ValueType::Int || phi.edges.size() != 2 || compare->args[0] != phi.result) continue;
      int outside = loop.blocks.count(phi.edges[0]) ? 1 : 0, inside = 1 - outside;
      if (loop.blocks.count(phi.edges[outside]) || !loop.blocks.count(phi.edges[inside])) continue;
      auto init = defs[phi.args[outside]], update = defs[phi.args[inside]];
      if (!init || init->op != Op::Constant || init->immediate < 0 || !update || update->op != Op::Add) continue;
      int step = update->args[0] == phi.result ? update->args[1] : update->args[1] == phi.result ? update->args[0] : -1;
      if (step < 0 || !defs[step] || defs[step]->op != Op::Constant || defs[step]->immediate != 1) continue;
      auto bound = defs[compare->args[1]];
      int64_t high = bound && bound->op == Op::Constant ? bound->immediate - 1 : INT32_MAX - 1;
      if (high <= init->immediate || graph.succ[phi.edges[outside]].size() != 1) continue;
      induction.push_back({phi.result, phi.args[outside], step, phi.edges[outside], phi.edges[inside], init->immediate, high});
    }
    for (const auto &iv : induction) {
      using Range = std::optional<std::pair<int64_t, int64_t>>;
      std::set<int> visiting;
      std::function<Range(int)> range = [&](int v) -> Range {
        if (v == iv.phi) return std::pair<int64_t, int64_t>{iv.low, iv.high};
        if (!defs[v] || !visiting.insert(v).second) return {};
        const auto &i = *defs[v]; Range result;
        if (i.op == Op::Constant && f.values[v] == ValueType::Int) result = {{i.immediate, i.immediate}};
        if ((i.op == Op::Add || i.op == Op::Sub || i.op == Op::Mul) && f.values[v] == ValueType::Int) {
          auto a = range(i.args[0]), b = range(i.args[1]);
          if (a && b) {
            int64_t low = 0, high = 0;
            if (i.op == Op::Add) { low = a->first + b->first; high = a->second + b->second; }
            if (i.op == Op::Sub) { low = a->first - b->second; high = a->second - b->first; }
            if (i.op == Op::Mul) {
              int64_t products[] = {a->first*b->first, a->first*b->second, a->second*b->first, a->second*b->second};
              low = *std::min_element(products, products+4); high = *std::max_element(products, products+4);
            }
            if (low >= INT32_MIN && high <= INT32_MAX) result = {{low, high}};
          }
        }
        // Other canonical nonnegative induction variables are invariant within
        // this loop. Their literal bounds prove that flattened indices don't wrap.
        if (i.op == Op::Phi && f.values[v] == ValueType::Int) {
          const auto &last = f.blocks[site[v]].instructions.back();
          if (last.op == Op::Branch && defs[last.args[0]] && defs[last.args[0]]->op == Op::Lt && defs[last.args[0]]->args[0] == v) {
            auto limit = defs[defs[last.args[0]]->args[1]];
            bool canonical = limit && limit->op == Op::Constant && limit->immediate > 0;
            bool initial = false, increment = false;
            for (int a : i.args) if (defs[a]) {
              if (defs[a]->op == Op::Constant && defs[a]->immediate == 0) initial = true;
              if (defs[a]->op == Op::Add) {
                int s = defs[a]->args[0] == v ? defs[a]->args[1] : defs[a]->args[1] == v ? defs[a]->args[0] : -1;
                increment |= s >= 0 && defs[s] && defs[s]->op == Op::Constant && defs[s]->immediate == 1;
              }
            }
            if (canonical && initial && increment && i.args.size() == 2) result = {{0, limit->immediate-1}};
          }
        }
        visiting.erase(v); return result;
      };
      std::function<std::optional<int64_t>(int)> coefficient = [&](int v) -> std::optional<int64_t> {
        if (v == iv.phi) return 1;
        if (!loop.blocks.count(site[v])) return 0;
        if (!defs[v]) return {};
        const auto &i = *defs[v];
        if (i.op == Op::Add || i.op == Op::Sub) {
          auto a = coefficient(i.args[0]), b = coefficient(i.args[1]);
          if (a && b) return i.op == Op::Add ? *a + *b : *a - *b;
        }
        if (i.op == Op::Mul) for (int n = 0; n < 2; ++n) if (defs[i.args[n]] && defs[i.args[n]]->op == Op::Constant) {
          auto a = coefficient(i.args[1-n]); if (a) return *a * defs[i.args[n]]->immediate;
        }
        return {};
      };
      std::vector<int> candidates;
      for (int b : loop.blocks) for (const auto &i : f.blocks[b].instructions) if (i.op == Op::Offset && !loop.blocks.count(site[i.args[0]])) {
        if (!range(i.args[1])) continue;
        auto c = coefficient(i.args[1]);
        if (c && *c > 0 && i.immediate > 0 && *c <= INT64_MAX / i.immediate) candidates.push_back(i.result);
      }
      for (int candidate : candidates) {
        const auto source = *defs[candidate]; auto scale = *coefficient(source.args[1]);
        std::vector<Instruction> initial;
        std::map<int, int> cloned;
        std::function<int(int)> start = [&](int v) {
          if (v == iv.phi) return iv.initial;
          if (!loop.blocks.count(site[v])) return v;
          if (cloned.count(v)) return cloned[v];
          auto i = *defs[v]; for (int &a : i.args) a = start(a);
          i.result = newValue(f, f.values[v]); cloned[v] = i.result; initial.push_back(i); return i.result;
        };
        int index = start(source.args[1]);
        Instruction pointer{Op::Offset}; pointer.result = newValue(f, ValueType::Pointer); pointer.args = {source.args[0], index}; pointer.immediate = source.immediate;
        int first = pointer.result; initial.push_back(pointer);
        int carried = newValue(f, ValueType::Pointer), next = newValue(f, ValueType::Pointer);
        Instruction phi{Op::Phi}; phi.result = carried; phi.args = {first, next}; phi.edges = {iv.preheader, iv.latch};
        f.blocks[loop.header].instructions.insert(f.blocks[loop.header].instructions.begin(), phi);
        auto &pre = f.blocks[iv.preheader].instructions; pre.insert(pre.end()-1, initial.begin(), initial.end());
        Instruction update{Op::Offset}; update.result = next; update.args = {carried, iv.step}; update.immediate = scale * source.immediate;
        auto &latch = f.blocks[iv.latch].instructions; latch.insert(latch.end()-1, update);
        for (int b : loop.blocks) for (auto &i : f.blocks[b].instructions) if (i.result == candidate) { i.op = Op::Move; i.args = {carried}; i.immediate = 0; }
      }
    }
  }
}
} // namespace

void printEffects(const ModuleIR &module, std::ostream &out) {
  auto summaries = summarize(module);
  for (const auto &f : module.functions) {
    const auto &e = summaries.at(f.source);
    auto regions = [&](const Regions &set) {
      std::vector<std::string> names;
      for (auto s : set) names.push_back(!s ? "unknown" : std::string(s->global ? "global:" : "argument:") + s->name);
      std::sort(names.begin(), names.end());
      out << '{'; for (size_t n = 0; n < names.size(); ++n) out << (n ? ", " : "") << names[n]; out << '}';
    };
    out << "function " << f.source->name << " reads="; regions(e.reads);
    out << " writes="; regions(e.writes); out << " observable=" << (e.observable ? "yes" : "no") << '\n';
  }
}

void printMemoryIR(const ModuleIR &module, std::ostream &out) {
  auto summaries = summarize(module);
  for (const auto &f : module.functions) {
    AliasAnalysis alias(f); MemorySSA memory(f, summaries, alias);
    out << "function " << f.source->name << " memory\n  m0 = entry\n";
    for (size_t id = 1; id < memory.nodes.size(); ++id) {
      const auto &n = memory.nodes[id]; out << "  m" << id << " = ";
      if (n.kind == MemoryNode::Phi) {
        out << "phi b" << n.block;
        const auto &pred = controlFlow(f).pred[n.block];
        for (size_t p = 0; p < n.inputs.size(); ++p) out << " [b" << pred[p] << ": m" << n.inputs[p] << ']';
      } else out << "def m" << n.previous << " at b" << n.block << '.' << n.position;
      out << '\n';
    }
    for (size_t b = 0; b < f.blocks.size(); ++b) for (size_t n = 0; n < f.blocks[b].instructions.size(); ++n) {
      const auto &i = f.blocks[b].instructions[n];
      if (i.op == Op::Load) out << "  use b" << b << '.' << n << " m" << memory.before[b][n]
          << " clobber=m" << memory.clobber(memory.before[b][n], alias.roots[i.args[0]],i.args[0],accessSize(f.values[i.result])) << '\n';
    }
  }
}

bool runAdvancedPass(ModuleIR &module, const std::string &name) {
  if (name == "globaldce") {
    std::map<const Function *, const FunctionIR *> functions;
    for (const auto &f : module.functions) functions[f.source] = &f;
    std::set<const Function *> live;
    std::function<void(const Function *)> visit = [&](const Function *source) {
      if (!live.insert(source).second || !functions.count(source)) return;
      for (const auto &b : functions.at(source)->blocks) for (const auto &i : b.instructions) if (i.op == Op::Call) visit(i.callee);
    };
    for (const auto &f : module.functions) if (f.source->name == "main") visit(f.source);
    module.functions.erase(std::remove_if(module.functions.begin(), module.functions.end(), [&](const FunctionIR &f) { return !live.count(f.source); }), module.functions.end());
    return true;
  }
  if (name == "isel" || name == "layout") {
    for (auto &f : module.functions) { destroySSA(f); if (name == "isel") selectInstructions(f); else layoutBlocks(f); }
    return true;
  }
  if (name == "inline") {
    if (std::any_of(module.functions.begin(), module.functions.end(), [](const FunctionIR &f) { return f.ssa; }))
      throw std::runtime_error("inline must precede SSA construction");
    inlineCalls(module); return true;
  }
  if (name == "sroa" || name == "dse" || name == "affine" || name == "egraph" || name == "ssa" || name == "simplify" || name == "lower-phi" || name == "sccp" || name == "gvn" || name == "licm" || name == "compact" || name == "strength" || name == "memory" || name == "rotate" || name == "unroll" || name == "tail" || name == "arith") {
    Summaries summaries;
    if (name == "gvn" || name == "licm" || name == "memory" || name == "dse") summaries = summarize(module);
    for (auto &f : module.functions) {
      if (name == "sroa") scalarReplace(f);
      else if (name == "dse") deadStores(f,summaries);
      else if (name == "affine") normalizeAffine(f);
      else if (name == "egraph") rewriteEqualities(f);
      else if (name == "ssa") ssa(f);
      else if (name == "simplify") simplify(f);
      else if (name == "sccp") { sccp(f); repairPhi(f); }
      else if (name == "gvn") gvn(f, summaries);
      else if (name == "licm") licm(f, summaries);
      else if (name == "memory") memoryForward(f, summaries);
      else if (name == "rotate") rotate(f);
      else if (name == "unroll") unroll(f);
      else if (name == "tail") tailRecursion(f);
      else if (name == "arith") arithmetic(f);
      else if (name == "compact") compact(f);
      else if (name == "strength") strength(f);
      else destroySSA(f);
    }
    return true;
  }
  if (name == "dce" && std::all_of(module.functions.begin(), module.functions.end(), [](const FunctionIR &f) { return f.ssa; })) {
    for (auto &f : module.functions) deadSSA(f);
    return true;
  }
  return false;
}

std::vector<int> loopDepth(const FunctionIR &f) {
  std::vector<int> depth(f.blocks.size(), 0);
  for (const auto &loop : loops(controlFlow(f))) for (int b : loop.blocks) ++depth[b];
  return depth;
}

void layoutBlocks(FunctionIR &f) {
  const auto &graph = controlFlow(f); std::vector<int> ids(f.blocks.size(), -1); std::vector<BasicBlock> blocks;
  for (int b : graph.order) { ids[b] = blocks.size(); blocks.push_back(std::move(f.blocks[b])); }
  for (auto &b : blocks) for (auto &i : b.instructions) {
    if (i.target >= 0) i.target = ids[i.target];
    if (i.otherwise >= 0) i.otherwise = ids[i.otherwise];
    for (int &edge : i.edges) edge = ids[edge];
  }
  f.blocks = std::move(blocks);
}

void repairPhi(FunctionIR &f) {
  if (!f.ssa) return;
  const auto &graph = controlFlow(f);
  for (size_t b = 0; b < f.blocks.size(); ++b) for (auto &i : f.blocks[b].instructions) if (i.op == Op::Phi) {
    std::vector<int> args, edges;
    for (size_t n = 0; n < i.edges.size(); ++n)
      if (std::find(graph.pred[b].begin(), graph.pred[b].end(), i.edges[n]) != graph.pred[b].end()) {
        args.push_back(i.args[n]); edges.push_back(i.edges[n]);
      }
    i.args = std::move(args); i.edges = std::move(edges);
  }
}

void verifySSA(const FunctionIR &f) {
  if (!f.ssa) return;
  const auto &graph = controlFlow(f); std::vector<int> sites(f.values.size(), -1), positions(f.values.size(), -1);
  for (size_t b = 0; b < f.blocks.size(); ++b) for (size_t n = 0; n < f.blocks[b].instructions.size(); ++n) {
    const auto &i = f.blocks[b].instructions[n];
    if (i.result >= 0) {
      if (sites[i.result] >= 0) throw std::logic_error("SSA value has multiple definitions");
      sites[i.result] = b; positions[i.result] = i.op == Op::Phi ? -1 : static_cast<int>(n);
    }
  }
  for (size_t b = 0; b < f.blocks.size(); ++b) for (size_t n = 0; n < f.blocks[b].instructions.size(); ++n) {
    const auto &i = f.blocks[b].instructions[n];
    if (i.op == Op::Phi && i.edges != graph.pred[b]) {
      auto edges = i.edges; std::sort(edges.begin(),edges.end());
      // CFG predecessors are unique and ordered by source block ID. Compare
      // vectors exactly: set equality previously hid duplicate incoming edges.
      if (edges != graph.pred[b]) throw std::logic_error("SSA phi does not match CFG predecessors");
    }
    for (size_t a = 0; a < i.args.size(); ++a) {
      int v = i.args[a], use = i.op == Op::Phi ? i.edges[a] : static_cast<int>(b);
      if (graph.rank[use] < 0) continue;
      if (sites[v] < 0 || !graph.dominates(sites[v], use)) throw std::logic_error("SSA definition does not dominate its use");
      if (i.op != Op::Phi && sites[v] == static_cast<int>(b) && positions[v] >= static_cast<int>(n))
        throw std::logic_error("SSA value is used before its definition");
    }
  }
}

void destroySSA(FunctionIR &f) {
  if (!f.ssa) return;
  struct Edge { int pred, target; std::vector<std::pair<int, int>> copies; };
  std::map<std::pair<int, int>, Edge> edges;
  std::set<int> variables;
  for (size_t b = 0; b < f.blocks.size(); ++b) for (const auto &i : f.blocks[b].instructions) if (i.op == Op::Phi) {
    variables.insert(i.result);
    for (size_t n = 0; n < i.args.size(); ++n) {
      auto key = std::make_pair(i.edges[n], static_cast<int>(b));
      if (!edges.count(key)) edges.emplace(key, Edge{i.edges[n], static_cast<int>(b), {}});
      if (i.result != i.args[n]) edges.at(key).copies.emplace_back(i.result, i.args[n]);
    }
  }
  for (auto &entry : edges) {
    auto &edge = entry.second; std::vector<Instruction> copies;
    while (!edge.copies.empty()) {
      auto ready = std::find_if(edge.copies.begin(), edge.copies.end(), [&](auto pair) {
        return std::none_of(edge.copies.begin(), edge.copies.end(), [&](auto other) { return other.second == pair.first; });
      });
      if (ready == edge.copies.end()) {
        int source = edge.copies.front().second, temp = newValue(f, f.values[source]);
        Instruction save{Op::Move}; save.result = temp; save.args = {source}; copies.push_back(std::move(save));
        for (auto &pair : edge.copies) if (pair.second == source) pair.second = temp;
      } else {
        Instruction move{Op::Move}; move.result = ready->first; move.args = {ready->second}; copies.push_back(std::move(move)); edge.copies.erase(ready);
      }
    }
    auto &code = f.blocks[edge.pred].instructions;
    if (code.back().op == Op::Jump) code.insert(code.end() - 1, copies.begin(), copies.end());
    else if (!copies.empty()) {
      int split = f.blocks.size();
      if (code.back().target == edge.target) code.back().target = split;
      if (code.back().otherwise == edge.target) code.back().otherwise = split;
      Instruction jump{Op::Jump}; jump.target = edge.target; copies.push_back(std::move(jump));
      f.blocks.push_back({"phi.edge", std::move(copies)});
    }
  }
  for (auto &b : f.blocks) b.instructions.erase(std::remove_if(b.instructions.begin(), b.instructions.end(), [](const Instruction &i) { return i.op == Op::Phi; }), b.instructions.end());
  f.variables.insert(f.variables.end(), variables.begin(), variables.end());
  f.ssa = false;
}
} // namespace sysy
