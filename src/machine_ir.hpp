#pragma once
#include "ir.hpp"
#include "stats.hpp"
#include <ostream>
#include <array>

namespace sysy {
// All instruction results are virtual registers. Mutable source objects are
// explicit memory objects; control flow is explicit and every block terminates.
enum class ValueType { Int, Float, Pointer, Double };
enum class Op {
  Constant, Address, String, Parameter, Move, Load, Store, Offset, Zero,
  Add, Sub, Mul, Div, Rem, And, Shr, Neg, Convert, Eq, Ne, Lt, Le, Gt, Ge,
  Call, Jump, Branch, Return, Phi, BranchCompare, Target, SpillLoad, SpillStore
};
struct Instruction {
  Op op;
  int result = -1;
  std::vector<int> args;
  int64_t immediate = 0;
  const Symbol *symbol = nullptr;
  const Function *callee = nullptr;
  std::string text;
  int target = -1, otherwise = -1;
  std::vector<int> edges;
  explicit Instruction(Op op) : op(op) {}
};
struct BasicBlock { std::string name; std::vector<Instruction> instructions; };
struct CFGAnalysis;
struct FunctionIR {
  const Function *source;
  std::vector<ValueType> values;
  std::vector<BasicBlock> blocks;
  std::vector<const Symbol *> objects;
  std::vector<int> variables;
  bool ssa = false;
  mutable std::shared_ptr<const CFGAnalysis> cfg;
  mutable size_t cfgHits = 0, cfgBuilds = 0;
};
// Cached by an exact topology snapshot: rewriting instructions preserves this
// analysis; changing any edge or block numbering invalidates it automatically.
struct CFGAnalysis {
  std::vector<std::array<int, 2>> topology;
  std::vector<std::vector<int>> pred, succ, children;
  std::vector<int> order, rank, idom;
  explicit CFGAnalysis(const FunctionIR &function);
  bool dominates(int a, int b) const;
};
const CFGAnalysis &controlFlow(const FunctionIR &function);

struct ModuleIR {
  const ProgramIR *source;
  std::vector<FunctionIR> functions;
  std::vector<std::unique_ptr<Symbol>> temporaries;
};

ModuleIR lower(const ProgramIR &program);
void optimize(ModuleIR &module, int level, Stats *stats = nullptr);
void runPasses(ModuleIR &module, const std::string &pipeline, Stats *stats = nullptr);
void verify(const ModuleIR &module);
void printIR(const ModuleIR &module, std::ostream &out);
void printMemoryIR(const ModuleIR &module, std::ostream &out);
void printEffects(const ModuleIR &module, std::ostream &out);
void emitAssembly(const ModuleIR &module, std::ostream &out, Stats *stats = nullptr, bool checkAllocation = false);
void rewriteEqualities(FunctionIR &function);
bool runAdvancedPass(ModuleIR &module, const std::string &name);
void destroySSA(FunctionIR &function);
std::vector<int> loopDepth(const FunctionIR &function);
void layoutBlocks(FunctionIR &function);
void selectInstructions(FunctionIR &function);
void verifySSA(const FunctionIR &function);
void repairPhi(FunctionIR &function);
} // namespace sysy
