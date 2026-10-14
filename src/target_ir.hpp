#pragma once
#include "machine_ir.hpp"
#include <algorithm>
#include <initializer_list>

namespace sysy {
// Pool indices are shared by selection, allocation and symbolic checking.
// t0/t1/t3/t4/t6 and ft0/ft1/ft2 remain reserved for pseudo expansion.
inline constexpr int integerRegisterCount = 22, floatRegisterCount = 29;
inline constexpr int integerArgumentStart = 13, floatArgumentStart = 21;
// Target instructions have explicit virtual def/use operands. ABI, stack-frame
// and rematerializable operations remain typed pseudos until frame layout.
enum class RV {
  Pseudo, Add, Addi, Addw, Addiw, Subw, Mul, Mulw, Divw, Remw, And, Andi,
  Slli, Slliw, Srlw, Srliw, Srai, Sraiw, Negw, Xor, Xori, Slt, Seqz, Snez,
  FaddS, FsubS, FmulS, FdivS, FnegS, FeqS, FltS, FleS,
  FcvtWS, FcvtSW, FcvtDS, FmvWX, FmvDX
};
const char *rvName(RV opcode);
// Most RV instructions have at most two uses. Keep them inline; only ABI calls
// need a separately allocated operand array. The representation owns its data.
class MachineArgs {
  size_t count = 0;
  union Storage { int small[2]; int *large; Storage() : small{} {} } storage;
  void assign(const int *source, size_t size) {
    count = size;
    if (count > 2) storage.large = new int[count];
    if (count) std::copy_n(source,count,begin());
  }
public:
  MachineArgs() = default;
  MachineArgs(std::initializer_list<int> args) { assign(args.begin(),args.size()); }
  MachineArgs(const std::vector<int> &args) { assign(args.data(),args.size()); }
  MachineArgs(const MachineArgs &other) { assign(other.begin(),other.size()); }
  MachineArgs(MachineArgs &&other) noexcept : count(other.count), storage(other.storage) { other.count = 0; }
  ~MachineArgs() { if (count > 2) delete[] storage.large; }
  MachineArgs &operator=(MachineArgs other) noexcept { std::swap(count,other.count); std::swap(storage,other.storage); return *this; }
  int *begin() { return count > 2 ? storage.large : storage.small; }
  const int *begin() const { return count > 2 ? storage.large : storage.small; }
  int *end() { return begin()+count; }
  const int *end() const { return begin()+count; }
  int &operator[](size_t n) { return begin()[n]; }
  int operator[](size_t n) const { return begin()[n]; }
  size_t size() const { return count; }
  bool empty() const { return !count; }
};
struct MachineInstruction {
  Op op;
  int result = -1;
  MachineArgs args;
  int64_t immediate = 0;
  const Symbol *symbol = nullptr;
  const Function *callee = nullptr;
  std::string text;
  int target = -1, otherwise = -1;
  RV opcode = RV::Pseudo;
  // Allocatable-register masks; operands are read early, results written late.
  uint32_t integerClobbers = 0, floatClobbers = 0;
  explicit MachineInstruction(Op op) : op(op) {}
};
struct MachineBlock { std::string name; std::vector<MachineInstruction> instructions; };
struct MachineFunction {
  const Function *source = nullptr;
  std::vector<ValueType> values;
  std::vector<MachineBlock> blocks;
  std::vector<const Symbol *> objects;
  std::vector<int> depth;
  std::vector<ValueType> slots;
};
MachineFunction selectTarget(FunctionIR function, Stats *stats = nullptr);
void verifyTarget(const MachineFunction &function);
void printTarget(const ModuleIR &module, std::ostream &out);
void printAllocated(const ModuleIR &module, std::ostream &out, Stats *stats = nullptr, bool checkAllocation = false);
} // namespace sysy
