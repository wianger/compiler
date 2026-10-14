#include "machine_ir.hpp"
#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <tuple>

namespace sysy {
namespace {
// A bounded equality graph for pure int32 regions. Effects and floating-point
// computations remain in the CFG; extraction cannot move across those barriers.
struct Node {
  Op op;
  int a = -1, b = -1, leaf = -1;
  int32_t number = 0;
};
using Key = std::tuple<Op, int, int, int, int32_t>;
class EGraph {
  std::vector<Node> nodes;
  std::vector<int> parent;
  std::map<Key, int> hash;
  size_t limit;
  bool changed = false;
public:
  explicit EGraph(size_t budget) : limit(budget) {}
  int root(int id) {
    if (parent[id] != id) parent[id] = root(parent[id]);
    return parent[id];
  }
  Key key(Node &n) {
    if (n.a >= 0) n.a = root(n.a);
    if (n.b >= 0) n.b = root(n.b);
    if ((n.op == Op::Add || n.op == Op::Mul || n.op == Op::And) && n.b < n.a) std::swap(n.a,n.b);
    return {n.op,n.a,n.b,n.leaf,n.number};
  }
  int add(Node n) {
    if (n.leaf < 0 && n.op != Op::Constant && (n.a < 0 || (n.op != Op::Neg && n.b < 0))) return -1;
    auto k = key(n); auto found = hash.find(k);
    if (found != hash.end()) return root(found->second);
    if (nodes.size() >= limit) return -1;
    int id = nodes.size(); nodes.push_back(n); parent.push_back(id); hash.emplace(k,id);
    changed = true; return id;
  }
  int constant(uint32_t n) { return add({Op::Constant,-1,-1,-1,static_cast<int32_t>(n)}); }
  void merge(int a, int b) {
    if (a < 0 || b < 0) return;
    a = root(a); b = root(b);
    if (a != b) { parent[std::max(a,b)] = std::min(a,b); changed = true; }
  }
  void rebuild() {
    bool again;
    do {
      again = false; hash.clear();
      for (size_t id = 0; id < nodes.size(); ++id) {
        auto inserted = hash.emplace(key(nodes[id]), id);
        if (!inserted.second && root(id) != root(inserted.first->second)) {
          merge(id, inserted.first->second); again = true;
        }
      }
    } while (again);
  }
  void saturate() {
    for (int round = 0; round < 5; ++round) {
      changed = false;
      std::map<int,uint32_t> constants;
      std::map<int,std::vector<Node>> members;
      for (size_t id = 0; id < nodes.size(); ++id) {
        Node n = nodes[id]; key(n); members[root(id)].push_back(n);
        if (n.op == Op::Constant) constants[root(id)] = static_cast<uint32_t>(n.number);
      }
      size_t count = nodes.size(), attempts = 0;
      for (size_t id = 0; id < count; ++id) {
        Node n = nodes[id]; key(n);
        if (n.leaf >= 0 || n.op == Op::Constant) continue;
        auto a = constants.find(n.a), b = constants.find(n.b);
        if (n.op == Op::Neg) {
          if (a != constants.end()) merge(id,constant(0u-a->second));
          continue;
        }
        if (a != constants.end() && b != constants.end()) {
          uint32_t value = n.op == Op::Add ? a->second+b->second : n.op == Op::Sub ? a->second-b->second :
                           n.op == Op::Mul ? a->second*b->second : a->second&b->second;
          merge(id,constant(value));
        }
        if (n.op == Op::Sub && n.a == n.b) merge(id,constant(0));
        for (int side = 0; side < 2; ++side) {
          int x = side ? n.b : n.a, c = side ? n.a : n.b;
          auto value = constants.find(c);
          if (value != constants.end()) {
            uint32_t v = value->second;
            if ((n.op == Op::Add && v == 0) || (n.op == Op::Mul && v == 1) || (n.op == Op::And && v == UINT32_MAX)) merge(id,x);
            if ((n.op == Op::Mul || n.op == Op::And) && v == 0) merge(id,c);
            if (n.op == Op::Sub && !side) { int k = constant(0u-v); if (k >= 0) merge(id,add({Op::Add,x,k})); }
            if (n.op == Op::Add || n.op == Op::Mul || n.op == Op::And) for (Node child : members[x]) {
              if (child.op != n.op) continue;
              for (int inner = 0; inner < 2; ++inner) {
                if (++attempts > limit*16) goto rebuild_graph;
                int y = inner ? child.b : child.a, d = inner ? child.a : child.b;
                auto v2 = constants.find(d); if (v2 == constants.end()) continue;
                uint32_t folded = n.op == Op::Add ? v+v2->second : n.op == Op::Mul ? v*v2->second : v&v2->second;
                int k = constant(folded); if (k >= 0) merge(id,add({n.op,y,k}));
              }
            }
          }
        }
        if (n.op == Op::Add) for (Node left : members[n.a]) if (left.op == Op::Mul)
          for (Node right : members[n.b]) if (right.op == Op::Mul)
            for (int x = 0; x < 2; ++x) for (int y = 0; y < 2; ++y) {
              if (++attempts > limit*16) goto rebuild_graph;
              int common = x ? left.b : left.a;
              if (common != (y ? right.b : right.a)) continue;
              int sum = add({Op::Add,x ? left.a : left.b,y ? right.a : right.b});
              if (sum >= 0) merge(id,add({Op::Mul,common,sum}));
            }
      }
      rebuild_graph:
      rebuild(); if (!changed || nodes.size() >= limit) break;
    }
  }
  void extract(FunctionIR &f, const std::vector<Instruction> &original, const std::vector<int> &roots,
               std::vector<Instruction> &out) {
    const double infinity = std::numeric_limits<double>::infinity();
    std::vector<double> cost(nodes.size(), infinity);
    std::vector<int> best(nodes.size(), -1);
    for (size_t round = 0; round <= nodes.size(); ++round) {
      bool progress = false;
      for (size_t id = 0; id < nodes.size(); ++id) {
        const auto &n = nodes[id]; int r = root(id);
        double c = n.leaf >= 0 ? 0 : n.op == Op::Constant ? (n.number == 0 ? 0 : n.number >= -2048 && n.number <= 2047 ? 1 : 2) : 1;
        if (n.a >= 0) c += cost[root(n.a)];
        if (n.b >= 0) c += cost[root(n.b)];
        if (n.op == Op::Mul) c += 2;
        if (c < cost[r]) { cost[r] = c; best[r] = id; progress = true; }
      }
      if (!progress) break;
    }
    std::map<int,int> available;
    std::function<int(int,int)> emit = [&](int id, int desired) -> int {
      int r = root(id); auto previous = available.find(r);
      if (previous != available.end()) return previous->second;
      if (best[r] < 0) throw std::logic_error("egraph has no acyclic extraction");
      Node n = nodes[best[r]];
      if (n.leaf >= 0) return available[r] = n.leaf;
      Instruction i{n.op}; i.immediate = n.number;
      if (n.a >= 0) i.args.push_back(emit(n.a,-1));
      if (n.b >= 0) i.args.push_back(emit(n.b,-1));
      if (desired < 0) { desired = f.values.size(); f.values.push_back(ValueType::Int); }
      i.result = desired; out.push_back(std::move(i)); return available[r] = desired;
    };
    for (size_t n = 0; n < original.size(); ++n) {
      int result = emit(roots[n], original[n].result);
      if (result != original[n].result) {
        Instruction copy{Op::Move}; copy.result = original[n].result; copy.args = {result}; out.push_back(std::move(copy));
      }
    }
  }
};
bool pure(const FunctionIR &f, const Instruction &i) {
  return i.result >= 0 && f.values[i.result] == ValueType::Int &&
      (i.op == Op::Constant || i.op == Op::Add || i.op == Op::Sub || i.op == Op::Mul || i.op == Op::And || i.op == Op::Neg);
}
} // namespace
void rewriteEqualities(FunctionIR &f) {
  if (!f.ssa) return;
  for (auto &block : f.blocks) {
    std::vector<Instruction> output;
    for (size_t start = 0; start < block.instructions.size();) {
      size_t end = start;
      while (end < block.instructions.size() && end-start < 256 && pure(f,block.instructions[end])) ++end;
      if (end-start < 2) { output.push_back(block.instructions[start++]); continue; }
      EGraph graph(std::min<size_t>(4096,(end-start)*12+32)); std::map<int,int> values;
      std::vector<Instruction> original; std::vector<int> roots;
      for (size_t n = start; n < end; ++n) {
        const auto &i = block.instructions[n]; Node node{i.op}; node.number = i.immediate;
        std::vector<int> args;
        for (int a : i.args) {
          auto found = values.find(a);
          if (found == values.end()) found = values.emplace(a,graph.add({Op::Move,-1,-1,a})).first;
          args.push_back(found->second);
        }
        if (!args.empty()) node.a = args[0];
        if (args.size() > 1) node.b = args[1];
        int id = graph.add(node); values[i.result] = id; roots.push_back(id); original.push_back(i);
      }
      graph.saturate(); graph.extract(f,original,roots,output); start = end;
    }
    block.instructions = std::move(output);
  }
}
} // namespace sysy
