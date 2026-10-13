#pragma once
#include "ir.hpp"
#include <ostream>

namespace sysy {
// All instruction results are virtual registers. Mutable source objects are
// explicit memory objects; control flow is explicit and every block terminates.
enum class ValueType { Int, Float, Pointer, Double };
enum class Op {
  Constant, Address, String, Parameter, Move, Load, Store, Offset, Zero,
  Add, Sub, Mul, Div, Rem, Neg, Convert, Eq, Ne, Lt, Le, Gt, Ge,
  Call, Jump, Branch, Return, Phi, BranchCompare
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
struct FunctionIR {
  const Function *source;
  std::vector<ValueType> values;
  std::vector<BasicBlock> blocks;
  std::vector<const Symbol *> objects;
  std::vector<int> variables;
  bool ssa = false;
};
struct ModuleIR {
  const ProgramIR *source;
  std::vector<FunctionIR> functions;
  std::vector<std::unique_ptr<Symbol>> temporaries;
};

ModuleIR lower(const ProgramIR &program);
void optimize(ModuleIR &module, int level);
void runPasses(ModuleIR &module, const std::string &pipeline);
void verify(const ModuleIR &module);
void printIR(const ModuleIR &module, std::ostream &out);
void emitAssembly(const ModuleIR &module, std::ostream &out);
bool runAdvancedPass(ModuleIR &module, const std::string &name);
void destroySSA(FunctionIR &function);
std::vector<int> loopDepth(const FunctionIR &function);
void layoutBlocks(FunctionIR &function);
void selectInstructions(FunctionIR &function);
void verifySSA(const FunctionIR &function);
void repairPhi(FunctionIR &function);
} // namespace sysy
