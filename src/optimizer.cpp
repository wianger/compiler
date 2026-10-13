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
namespace {
struct Graph {
  std::vector<std::vector<int>> pred, succ, children;
  std::vector<int> order, rank, idom;
  explicit Graph(const FunctionIR &f) {
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
  }
  bool dominates(int a, int b) const {
    while (b >= 0 && b != a && b != idom[b]) b = idom[b];
    return a == b;
  }
};

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

void ssa(FunctionIR &f) {
  if (f.ssa) return;
  Graph graph(f);
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
    if (leaf && count <= 80 && f.blocks.size() <= 12) candidates.emplace(f.source, f);
  }
  for (auto &f : module.functions) {
    size_t budget = 3000;
    for (size_t b = 0; b < f.blocks.size(); ++b) {
      auto &code = f.blocks[b].instructions;
      auto at = std::find_if(code.begin(), code.end(), [&](const Instruction &i) { return i.op == Op::Call && candidates.count(i.callee) && i.callee != f.source; });
      if (at == code.end() || budget == 0) continue;
      Instruction call = *at; const auto &callee = candidates.at(call.callee);
      size_t cost = 0; for (const auto &block : callee.blocks) cost += block.instructions.size();
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
  std::function<void(int)> mark = [&](int v) {
    if (live[v]) return;
    live[v] = true; if (defs[v]) for (int arg : defs[v]->args) mark(arg);
  };
  for (const auto &b : f.blocks) for (const auto &i : b.instructions) if (effect(i.op)) for (int arg : i.args) mark(arg);
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
  bool changed;
  do {
    changed = false;
    auto merge = [&](int v, Lattice next) {
      auto &old = lattice[v];
      if (old.kind == 2 || next.kind == 0) return;
      if (old.kind == 1 && next.kind == 1 && old.value != next.value) next.kind = 2;
      if (old.kind != next.kind || (old.kind == 0 && next.kind == 1)) {
        old = next; changed = true;
        if (old.kind == 1) { constants[v].immediate = old.value; known[v] = &constants[v]; }
        else known[v] = nullptr;
      }
    };
    for (size_t b = 0; b < f.blocks.size(); ++b) if (executable[b]) {
      for (const auto &i : f.blocks[b].instructions) {
        if (i.result < 0) continue;
        Lattice next;
        if (i.op == Op::Constant) next = {1, i.immediate};
        else if (i.op == Op::Move) next = lattice[i.args[0]];
        else if (i.op == Op::Phi) {
          for (size_t n = 0; n < i.args.size(); ++n) if (edges.count({i.edges[n], static_cast<int>(b)})) {
            auto incoming = lattice[i.args[n]];
            if (incoming.kind == 0) continue;
            if (next.kind == 0) next = incoming;
            else if (incoming.kind == 2 || (next.kind == 1 && incoming.value != next.value)) next.kind = 2;
          }
        } else if (i.op == Op::Parameter || i.op == Op::Load || i.op == Op::Call || i.op == Op::Address || i.op == Op::String || i.op == Op::Offset) next.kind = 2;
        else {
          bool all = true, over = false;
          for (int a : i.args) { all &= lattice[a].kind == 1; over |= lattice[a].kind == 2; }
          if (all) { auto value = fold(i, f, known); next = value ? Lattice{1, *value} : Lattice{2, 0}; }
          else if (over) next.kind = 2;
        }
        merge(i.result, next);
      }
      const auto &last = f.blocks[b].instructions.back();
      auto activate = [&](int s) { if (s >= 0) { changed |= edges.emplace(b, s).second; executable[s] = true; } };
      if (last.op == Op::Jump) activate(last.target);
      if (last.op == Op::Branch) {
        auto test = lattice[last.args[0]];
        if (test.kind == 1) activate(test.value ? last.target : last.otherwise);
        if (test.kind == 2) { activate(last.target); activate(last.otherwise); }
      }
    }
  } while (changed);
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

void gvn(FunctionIR &f) {
  if (!f.ssa) return;
  Graph graph(f); std::vector<int> aliases(f.values.size());
  for (size_t v = 0; v < aliases.size(); ++v) aliases[v] = v;
  using Table = std::map<std::string, int>;
  std::function<void(int, Table)> visit = [&](int b, Table expressions) {
    std::map<int, int> memory;
    for (auto &i : f.blocks[b].instructions) {
      for (int &a : i.args) a = root(aliases, a);
      if (i.op == Op::Store) { memory.clear(); memory[i.args[0]] = i.args[1]; }
      if (i.op == Op::Zero || i.op == Op::Call) memory.clear();
      if (i.result < 0) continue;
      if (i.op == Op::Move) { aliases[i.result] = i.args[0]; continue; }
      if (i.op == Op::Load) {
        auto it = memory.find(i.args[0]);
        if (it != memory.end()) aliases[i.result] = it->second;
        else memory[i.args[0]] = i.result;
        continue;
      }
      if (i.op == Op::Phi || i.op == Op::Parameter || i.op == Op::Call) continue;
      std::ostringstream key;
      key << static_cast<int>(i.op) << ':' << static_cast<int>(f.values[i.result]) << ':' << i.immediate << ':' << i.symbol << ':' << i.text;
      auto args = i.args;
      if (args.size() == 2 && (i.op == Op::Eq || i.op == Op::Ne ||
          ((i.op == Op::Add || i.op == Op::Mul) && f.values[i.result] == ValueType::Int)) && args[0] > args[1]) std::swap(args[0], args[1]);
      for (int a : args) key << ':' << a;
      auto inserted = expressions.emplace(key.str(), i.result);
      if (!inserted.second) aliases[i.result] = inserted.first->second;
    }
    for (int child : graph.children[b]) visit(child, expressions);
  };
  visit(0, {});
  for (auto &b : f.blocks) for (auto &i : b.instructions) {
    for (int &a : i.args) a = root(aliases, a);
    if (i.result >= 0 && aliases[i.result] != i.result) { i.op = Op::Move; i.args = {root(aliases, i.result)}; i.edges.clear(); }
  }
}

struct Loop { int header; std::set<int> blocks; };
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

void licm(FunctionIR &f) {
  if (!f.ssa) return;
  auto nests = loops(Graph(f));
  for (const auto &original : nests) {
    Graph graph(f); auto current = loops(graph);
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
    bool memory = false;
    for (int b : loop.blocks) for (const auto &i : f.blocks[b].instructions) memory |= i.op == Op::Store || i.op == Op::Zero || i.op == Op::Call;
    bool changed;
    do {
      changed = false;
      std::vector<std::pair<const Symbol *, int64_t>> addresses(f.values.size(), {nullptr, 0});
      for (const auto &block : f.blocks) for (const auto &i : block.instructions)
        if (i.op == Op::Address) addresses[i.result] = {i.symbol, i.immediate};
      for (int b : loop.blocks) {
        auto &code = f.blocks[b].instructions;
        for (size_t n = 0; n < code.size();) {
          auto &i = code[n]; bool movable = i.result >= 0 && !effect(i.op) && i.op != Op::Phi && i.op != Op::Parameter;
          for (int a : i.args) movable &= !loop.blocks.count(site[a]);
          if (i.op == Op::Load) {
            auto pointer = addresses[i.args[0]];
            // Speculate only fixed, valid object accesses, and only in a loop
            // with no writes or calls. Unknown addresses remain guarded.
            movable &= !memory && pointer.first && pointer.second >= 0 &&
                !pointer.first->parameter && pointer.second / 4 < pointer.first->type.elements();
          }
          if (movable) {
            site[i.result] = preheader; auto moved = std::move(i);
            code.erase(code.begin() + n);
            auto &pre = f.blocks[preheader].instructions; pre.insert(pre.end() - 1, std::move(moved)); changed = true;
          } else ++n;
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
  auto nests = loops(Graph(f));
  for (const auto &loop : nests) {
    Graph graph(f); auto original = definitions(f);
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
  if (name == "ssa" || name == "simplify" || name == "lower-phi" || name == "sccp" || name == "gvn" || name == "licm" || name == "compact" || name == "strength") {
    for (auto &f : module.functions) {
      if (name == "ssa") ssa(f);
      else if (name == "simplify") simplify(f);
      else if (name == "sccp") { sccp(f); repairPhi(f); }
      else if (name == "gvn") gvn(f);
      else if (name == "licm") licm(f);
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
  for (const auto &loop : loops(Graph(f))) for (int b : loop.blocks) ++depth[b];
  return depth;
}

void layoutBlocks(FunctionIR &f) {
  Graph graph(f); std::vector<int> ids(f.blocks.size(), -1); std::vector<BasicBlock> blocks;
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
  Graph graph(f);
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
  Graph graph(f); std::vector<int> sites(f.values.size(), -1), positions(f.values.size(), -1);
  for (size_t b = 0; b < f.blocks.size(); ++b) for (size_t n = 0; n < f.blocks[b].instructions.size(); ++n) {
    const auto &i = f.blocks[b].instructions[n];
    if (i.result >= 0) {
      if (sites[i.result] >= 0) throw std::logic_error("SSA value has multiple definitions");
      sites[i.result] = b; positions[i.result] = i.op == Op::Phi ? -1 : static_cast<int>(n);
    }
  }
  for (size_t b = 0; b < f.blocks.size(); ++b) for (size_t n = 0; n < f.blocks[b].instructions.size(); ++n) {
    const auto &i = f.blocks[b].instructions[n];
    if (i.op == Op::Phi && std::set<int>(i.edges.begin(), i.edges.end()) != std::set<int>(graph.pred[b].begin(), graph.pred[b].end()))
      throw std::logic_error("SSA phi does not match CFG predecessors");
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
