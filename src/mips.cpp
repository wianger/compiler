#include "mips.h"
#include <algorithm> // For std::max
#include <iostream>  // For debugging
#include <stdexcept>
#include <string> // Ensure std::string is fully defined
#include <vector> // Ensure std::vector is fully defined

// Helper to manage temporary registers. For simplicity, we'll use $t0-$t9
// In a real compiler, a more sophisticated register allocation is needed.
const int NUM_TEMP_REGS = 10;

// Mips Constructor
Mips::Mips(std::vector<Quadruple> &tac, std::shared_ptr<SymbolTable> table)
    : tac(tac), table(table), labelCount(0),
      tempRegsInUse(NUM_TEMP_REGS, false) {
  // currentFunctionName will be set during processing
  // maxTempIndices and tempStackSpace will be populated by precomputeTempUsage
}

// Helper to get a temporary register
std::string Mips::getTempReg() {
  for (int i = 0; i < NUM_TEMP_REGS; ++i) {
    if (!this->tempRegsInUse[i]) {
      this->tempRegsInUse[i] = true;
      return "$t" + std::to_string(i);
    }
  }
  throw std::runtime_error("Out of temporary registers!");
  return "";
}

// Helper to release a temporary register
void Mips::releaseTempReg(const std::string &reg) {
  if (reg.rfind("$t", 0) == 0) {
    int regNum = std::stoi(reg.substr(2));
    if (regNum >= 0 && regNum < NUM_TEMP_REGS) {
      this->tempRegsInUse[regNum] = false;
    }
  }
}

// Helper to reset all temporary registers state
void Mips::resetTempRegs() {
  std::fill(this->tempRegsInUse.begin(), this->tempRegsInUse.end(), false);
}

// Helper to determine if a string is a number
bool Mips::isNumber(const std::string &s) {
  if (s.empty()) {
    return false;
  }
  size_t start_index = 0;
  if (s[0] == '-') {
    if (s.length() == 1)
      return false; // Just a "-" is not a number
    start_index = 1;
  }
  for (size_t i = start_index; i < s.length(); ++i) {
    if (!isdigit(s[i]))
      return false;
  }
  return true; // Original used std::all_of, requires <algorithm> which is fine,
               // but loop is also C++11 safe.
}

// Helper to determine if a string is a temporary variable (e.g., "t1", "t23")
bool Mips::isTempVar(const std::string &s) {
  return s.length() > 1 && s[0] == 't' && isNumber(s.substr(1));
}

// Load value into a register. Value can be immediate, global, local, or temp.
std::string Mips::loadToReg(const TacOperand &src_op,
                            const std::string &destReg) {
  if (src_op.isString()) {
    const std::string &src_str = src_op.getString();
    if (isNumber(src_str)) {
      this->text.push_back(this->indent(1) + "li " + destReg + ", " + src_str);
    } else if (isTempVar(src_str)) {
      int temp_num = std::stoi(src_str.substr(1));
      int temp_actual_offset_from_fp =
          temp_num * 4; // Temps start at 0($fp) relative to temp base
      this->text.push_back(this->indent(1) + "lw " + destReg + ", " +
                           std::to_string(temp_actual_offset_from_fp) +
                           "($fp) # Temp var " + src_str);
    } else {
      // Assumed to be a global label (e.g., for string literals like
      // printfstr0, or function names if loaded this way)
      this->text.push_back(this->indent(1) + "la " + destReg + ", " + src_str);
    }
  } else if (src_op.isSymbol()) {
    const auto &symbol = src_op.getSymbol();
    if (!symbol) {
      throw std::runtime_error(
          "loadToReg: TacOperand held a null Symbol pointer.");
    }
    if (symbol->isGlobalVar()) {
      if (symbol->getType() == SymbolType::ARRAY ||
          symbol->getType() ==
              SymbolType::STR) { // Address of global array/string
        this->text.push_back(this->indent(1) + "la " + destReg + ", " +
                             symbol->getName());
      } else { // Value of global int
        this->text.push_back(this->indent(1) + "lw " + destReg + ", " +
                             symbol->getName());
      }
    } else { // Local var or parameter (parser determined its offset relative to
             // func arg/local base)
      // The symbol->getOffset() is already the flat offset from the start of
      // arg/local block. tempStackSpace[currentFunctionName] is the size of the
      // temp variable block preceding args/locals.
      int baseOffsetForLocalsAndParams =
          this->tempStackSpace.count(this->currentFunctionName)
              ? this->tempStackSpace[this->currentFunctionName]
              : 0;

      // 检查是否是数组参数（多维数组的第一维是-1）
      if (symbol->getType() == SymbolType::ARRAY &&
          !symbol->getDimensions().empty() &&
          symbol->getDimensions()[0] == -1) {
        // 数组参数是指向数组基址的指针，需要先加载指针值
        this->text.push_back(
            this->indent(1) + "lw " + destReg + ", " +
            std::to_string(symbol->getOffset() + baseOffsetForLocalsAndParams) +
            "($fp) # Array param " + symbol->getName());
      } else if (symbol->getType() == SymbolType::ARRAY) {
        // 局部数组变量，计算基址
        if (symbol->isGlobalVar()) {
          // 全局数组
          this->text.push_back(this->indent(1) + "la " + destReg + ", " +
                               symbol->getName());
        } else {
          // 局部数组，使用fp相对偏移
          this->text.push_back(this->indent(1) + "addi " + destReg + ", $fp, " +
                               std::to_string(symbol->getOffset() +
                                              baseOffsetForLocalsAndParams) +
                               " # Local array " + symbol->getName());
        }
      } else {
        // 普通局部变量或参数
        this->text.push_back(
            this->indent(1) + "lw " + destReg + ", " +
            std::to_string(symbol->getOffset() + baseOffsetForLocalsAndParams) +
            "($fp) # Local/Param " + symbol->getName());
      }
    }
  } else {
    throw std::runtime_error(
        "loadToReg: Unknown source operand type (empty variant?)");
  }
  return destReg;
}

// Store register value to memory (global or local).
void Mips::storeFromReg(const TacOperand &dest_op, const std::string &srcReg) {
  if (dest_op.isString()) {
    const std::string &dest_str = dest_op.getString();
    if (isTempVar(dest_str)) {
      int temp_num = std::stoi(dest_str.substr(1));
      int temp_actual_offset_from_fp =
          temp_num * 4; // Temps start at 0($fp) relative to temp base
      this->text.push_back(this->indent(1) + "sw " + srcReg + ", " +
                           std::to_string(temp_actual_offset_from_fp) +
                           "($fp) # Temp var " + dest_str);
    } else {
      throw std::runtime_error(
          "storeFromReg: Cannot store to non-temporary string: " + dest_str);
    }
  } else if (dest_op.isSymbol()) {
    const auto &symbol = dest_op.getSymbol();
    if (!symbol) {
      throw std::runtime_error("storeFromReg: TacOperand held a null Symbol "
                               "pointer for destination.");
    }
    if (symbol->isGlobalVar()) {
      if (symbol->getType() == SymbolType::ARRAY) {
        throw std::runtime_error(
            "storeFromReg: Direct store to global array name not supported by "
            "this path, use []= TAC for elements.");
      }
      this->text.push_back(this->indent(1) + "sw " + srcReg + ", " +
                           symbol->getName());
    } else { // Local var or parameter
      int baseOffsetForLocalsAndParams =
          this->tempStackSpace.count(this->currentFunctionName)
              ? this->tempStackSpace[this->currentFunctionName]
              : 0;
      this->text.push_back(
          this->indent(1) + "sw " + srcReg + ", " +
          std::to_string(symbol->getOffset() + baseOffsetForLocalsAndParams) +
          "($fp) # Local/Param " + symbol->getName());
    }
  } else {
    throw std::runtime_error(
        "storeFromReg: Unknown destination operand type (empty variant?)");
  }
}

void Mips::precomputeTempUsage() {
  std::string funcNameForPass;
  int currentMaxTemp = -1;
  // int mipsTempCounter = 0; // Removed, Mips class's tempCount is not for this

  for (const auto &quad : this->tac) {
    if (quad.op == "func") {
      if (!funcNameForPass.empty() && currentMaxTemp != -1) {
        this->maxTempIndices[funcNameForPass] = currentMaxTemp;
        this->tempStackSpace[funcNameForPass] = (currentMaxTemp + 1) * 4;
      }
      if (quad.arg1.isString()) { // Function name is a string
        funcNameForPass = quad.arg1.getString();
      } else {
        throw std::runtime_error(
            "Func TAC expects function name as string in arg1");
      }
      currentMaxTemp = -1;
      // mipsTempCounter = 0; // Reset for this function in MIPS context
    } else if (quad.op == "endfunc") {
      if (!funcNameForPass.empty()) {
        this->maxTempIndices[funcNameForPass] = currentMaxTemp;
        this->tempStackSpace[funcNameForPass] = (currentMaxTemp + 1) * 4;
      }
      funcNameForPass = "";
    }

    if (!funcNameForPass.empty()) {
      auto check_and_update_temp =
          [&](const TacOperand &operand) { // Pass TacOperand by const ref
            if (!operand.isEmpty() && operand.isString()) {
              const std::string &s = operand.getString();
              if (isTempVar(s)) {
                int temp_idx = std::stoi(s.substr(1));
                currentMaxTemp = std::max(currentMaxTemp, temp_idx);
              }
            }
          };

      check_and_update_temp(
          quad.arg1); // arg1 can be string (temp, num, label) or symbol
      check_and_update_temp(quad.arg2);   // arg2 might be empty
      check_and_update_temp(quad.result); // result might be empty
    }
  }
  if (!funcNameForPass.empty() && currentMaxTemp != -1) {
    this->maxTempIndices[funcNameForPass] = currentMaxTemp;
    this->tempStackSpace[funcNameForPass] = (currentMaxTemp + 1) * 4;
  }
}

std::vector<std::string> Mips::generateMips() {
  this->precomputeTempUsage();
  this->generateDataSection();
  this->generateTextSection();
  this->buildin_func();
  this->mipsCode.insert(this->mipsCode.end(), this->data.begin(),
                        this->data.end());
  this->mipsCode.insert(this->mipsCode.end(), this->text.begin(),
                        this->text.end());
  return this->mipsCode;
}

std::string Mips::indent(int level) { return std::string(level * 4, ' '); }

void Mips::generateDataSection() {
  this->data.push_back(".data");
  // this->data.push_back(this->indent(1) + "newline_debug_char: .asciiz
  // \"\\n\""); // REMOVED for debugging calls

  // Create a vector of pairs to store global symbols for sorting
  std::vector<std::pair<std::string, std::shared_ptr<Symbol>>> sorted_globals;
  for (const auto &entry : this->table->getTable()) {
    if (entry.second->isGlobalVar()) {
      sorted_globals.push_back(entry);
    }
  }

  // Sort the global symbols by name (alphabetically)
  std::sort(sorted_globals.begin(), sorted_globals.end(),
            [](const std::pair<std::string, std::shared_ptr<Symbol>> &a,
               const std::pair<std::string, std::shared_ptr<Symbol>> &b) {
              return a.first < b.first;
            });

  bool last_was_string =
      false; // To potentially add .align, though assemblers usually handle it

  for (const auto &entry :
       sorted_globals) { // Iterate over sorted global symbols
    const std::string &name = entry.first;
    const std::shared_ptr<Symbol> &symbol = entry.second;
    // No need for: if (!symbol->isGlobalVar()) continue; as we filtered above

    // Most MIPS assemblers automatically align .word and .space (if size is
    // multiple of 4) So, explicit .align 2 might be redundant. We'll rely on
    // assembler defaults for now.
    /*
    if (symbol->getType() == SymbolType::INT || symbol->getType() ==
    SymbolType::ARRAY) { if (last_was_string) {
        // this->data.push_back(this->indent(1) + ".align 2");
      }
    }
    */

    if (symbol->getType() == SymbolType::INT) {
      this->data.push_back(this->indent(1) + name + ": .word " +
                           std::to_string(symbol->getValue()));
      last_was_string = false;
    } else if (symbol->getType() == SymbolType::ARRAY) {
      if (symbol->isInitValue()) {
        std::string array_str = this->indent(1) + name + ": .word ";
        const auto &arr_vals = symbol->getArray();
        for (size_t i = 0; i < arr_vals.size(); ++i) {
          array_str += std::to_string(arr_vals[i]);
          if (i != arr_vals.size() - 1)
            array_str += ", ";
        }
        this->data.push_back(array_str);
      } else {
        // 为数组分配空间，使用总大小（所有维度乘积）* 4字节
        int totalSize = symbol->getTotalSize() * 4;
        // 确保分配的空间足够大
        if (totalSize <= 0) {
          totalSize = 4; // 至少分配4字节
        }
        this->data.push_back(this->indent(1) + name + ": .space " +
                             std::to_string(totalSize));
      }
      last_was_string = false;
    } else if (symbol->getType() == SymbolType::STR) {
      std::string escaped_str = "";
      for (char c : symbol->getStr()) {
        if (c == '\\')
          escaped_str += "\\";
        else if (c == '\n')
          escaped_str += "\\n"; // MIPS .asciiz needs actual backslash-n
        else if (c == '\t')
          escaped_str += "\\t"; // MIPS .asciiz needs actual backslash-t
        else if (c == '\"')
          escaped_str += "\\\""; // Escape double quote for MIPS string
        else
          escaped_str += c;
      }
      this->data.push_back(this->indent(1) + name + ": .asciiz \"" +
                           escaped_str + "\"");
      last_was_string = true;
    }
  }
}

void Mips::buildin_func() {
  this->text.push_back("");
  this->text.push_back("# Built-in functions");

  this->text.push_back("getint:");
  this->text.push_back(this->indent(1) + "li $v0, 5");
  this->text.push_back(this->indent(1) + "syscall");
  this->text.push_back(this->indent(1) + "jr $ra");
  this->text.push_back("");

  this->text.push_back("printstr:");
  this->text.push_back(this->indent(1) + "li $v0, 4");
  this->text.push_back(this->indent(1) + "syscall");
  this->text.push_back(this->indent(1) + "jr $ra");
  this->text.push_back("");

  this->text.push_back("printint:");
  this->text.push_back(this->indent(1) + "li $v0, 1");
  this->text.push_back(this->indent(1) + "syscall");
  this->text.push_back(this->indent(1) + "jr $ra");
  this->text.push_back("");
}

void Mips::generateTextSection() {
  this->text.push_back(".text");
  // Add a jump to main as the very first executable instruction in the .text
  // segment. This is to handle cases where the simulator starts at the
  // beginning of the .text segment and the option "Initialize program counter
  // to global 'main' if defined" is NOT set.
  this->text.push_back(this->indent(1) + "j main");
  this->text.push_back(this->indent(1) + "nop # Branch delay slot filler");

  this->text.push_back(".globl main");

  int currentArgReg = 0; // For outgoing call argument registers $a0-$a3
  int formalParamCounter = 0;
  int expectedFormalParams = 0;

  for (size_t i = 0; i < this->tac.size(); ++i) {
    const auto &quad = this->tac[i];

    this->resetTempRegs();

    // Updated TAC printing using helper functions
    // this->text.push_back("# TAC: " + quad.op + " " +
    //                      getOperandNameForDebug(quad.arg1) + " " +
    //                      getOptionalOperandNameForDebug(quad.arg2) + " " +
    //                      getOptionalOperandNameForDebug(quad.result));

    if (quad.op == "func") {
      // quad.arg1 is TacOperandType (std::string func_name)
      // quad.arg2 is std::optional<TacOperandType> (std::string num_params_str)
      this->currentFunctionName = quad.arg1.getString();
      formalParamCounter = 0;
      if (!quad.arg2.isEmpty() && quad.arg2.isString()) {
        const std::string &num_params_str = quad.arg2.getString();
        if (!num_params_str.empty() && isNumber(num_params_str)) {
          expectedFormalParams = std::stoi(num_params_str);
        } else {
          expectedFormalParams = 0;
        }
      } else {
        expectedFormalParams = 0;
      }

      auto symbol = this->table->lookup(
          this->currentFunctionName); // Global table lookup for func symbol
      if (!symbol)
        throw std::runtime_error("Function symbol not found: " +
                                 this->currentFunctionName);

      int localsAndParamsSize =
          symbol->getBlockSize(); // Size of params + declared locals
      int tempsSize = this->tempStackSpace.count(this->currentFunctionName)
                          ? this->tempStackSpace[this->currentFunctionName]
                          : 0;
      int frameSize =
          localsAndParamsSize + tempsSize + 8; // Add 8 for $ra and old $fp

      if (frameSize % 8 != 0)
        frameSize = ((frameSize / 8) + 1) * 8;

      this->text.push_back(this->currentFunctionName + ":");
      // Prologue
      this->text.push_back(this->indent(1) + "addi $sp, $sp, -" +
                           std::to_string(frameSize));
      this->text.push_back(this->indent(1) + "sw $ra, " +
                           std::to_string(frameSize - 4) + "($sp)");
      this->text.push_back(this->indent(1) + "sw $fp, " +
                           std::to_string(frameSize - 8) + "($sp)");
      this->text.push_back(this->indent(1) + "move $fp, $sp");

    } else if (quad.op == "endfunc") {
      // quad.arg1 is TacOperandType (std::string func_name)
      std::string func_name_to_end = quad.arg1.getString();
      auto symbol = this->table->lookup(func_name_to_end);
      if (!symbol)
        throw std::runtime_error("Function symbol not found for endfunc: " +
                                 func_name_to_end);

      expectedFormalParams = 0;

      int localsAndParamsSize = symbol->getBlockSize();
      int tempsSize = this->tempStackSpace.count(func_name_to_end)
                          ? this->tempStackSpace[func_name_to_end]
                          : 0;
      int frameSize = localsAndParamsSize + tempsSize + 8;
      if (frameSize % 8 != 0)
        frameSize = ((frameSize / 8) + 1) * 8;

      this->text.push_back(this->currentFunctionName + "_epilogue:");

      if (func_name_to_end == "main") {
        this->text.push_back(this->indent(1) + "lw $fp, " +
                             std::to_string(frameSize - 8) + "($fp)");
        this->text.push_back(this->indent(1) + "addi $sp, $sp, " +
                             std::to_string(frameSize));
        this->text.push_back(this->indent(1) + "li $v0, 10");
        this->text.push_back(this->indent(1) + "syscall");
      } else {
        this->text.push_back(this->indent(1) + "lw $ra, " +
                             std::to_string(frameSize - 4) + "($fp)");
        this->text.push_back(this->indent(1) + "lw $fp, " +
                             std::to_string(frameSize - 8) + "($fp)");
        this->text.push_back(this->indent(1) + "addi $sp, $sp, " +
                             std::to_string(frameSize));
        this->text.push_back(this->indent(1) + "jr $ra");
      }
      this->currentFunctionName = "";
      this->resetTempRegs();

    } else if (quad.op == "=") {
      // quad.arg1 is TacOperandType (src)
      // quad.result is std::optional<TacOperandType> (dest)
      if (quad.result.isEmpty())
        throw std::runtime_error("'=' TAC missing result operand.");
      std::string reg1 = this->getTempReg();
      this->loadToReg(quad.arg1, reg1);      // No more quad.scope
      this->storeFromReg(quad.result, reg1); // No more quad.scope
      this->releaseTempReg(reg1);

    } else if (quad.op == "+" || quad.op == "-" || quad.op == "*" ||
               quad.op == "/" || quad.op == "%") {
      // quad.arg1 is TacOperandType (src1)
      // quad.arg2 is std::optional<TacOperandType> (src2)
      // quad.result is std::optional<TacOperandType> (dest_temp)
      if (quad.arg2.isEmpty() || quad.result.isEmpty())
        throw std::runtime_error("Binary op TAC missing arg2 or result.");

      std::string reg1 = this->getTempReg();
      std::string reg2 = this->getTempReg();
      std::string resReg = this->getTempReg();

      this->loadToReg(quad.arg1, reg1);
      this->loadToReg(quad.arg2, reg2);

      if (quad.op == "+")
        this->text.push_back(this->indent(1) + "add " + resReg + ", " + reg1 +
                             ", " + reg2);
      else if (quad.op == "-")
        this->text.push_back(this->indent(1) + "sub " + resReg + ", " + reg1 +
                             ", " + reg2);
      else if (quad.op == "*") {
        this->text.push_back(this->indent(1) + "mult " + reg1 + ", " + reg2);
        this->text.push_back(this->indent(1) + "mflo " + resReg);
      } else if (quad.op == "/") {
        this->text.push_back(this->indent(1) + "div " + reg1 + ", " + reg2);
        this->text.push_back(this->indent(1) + "mflo " + resReg);
      } else if (quad.op == "%") {
        this->text.push_back(this->indent(1) + "div " + reg1 + ", " + reg2);
        this->text.push_back(this->indent(1) + "mfhi " + resReg);
      }
      this->storeFromReg(quad.result, resReg);
      this->releaseTempReg(reg1);
      this->releaseTempReg(reg2);
      this->releaseTempReg(resReg);

    } else if (quad.op == "neg") {
      // quad.arg1 is TacOperandType (src)
      // quad.result is std::optional<TacOperandType> (dest_temp)
      if (quad.result.isEmpty())
        throw std::runtime_error("'neg' TAC missing result operand.");

      std::string reg1 = this->getTempReg();
      std::string resReg = this->getTempReg();
      this->loadToReg(quad.arg1, reg1);
      this->text.push_back(this->indent(1) + "subu " + resReg + ", $zero, " +
                           reg1);
      this->storeFromReg(quad.result, resReg);
      this->releaseTempReg(reg1);
      this->releaseTempReg(resReg);

    } else if (quad.op == "==" || quad.op == "!=" || quad.op == "<" ||
               quad.op == "<=" || quad.op == ">" || quad.op == ">=") {
      // quad.arg1, quad.arg2.value(), quad.result.value()
      if (quad.arg2.isEmpty() || quad.result.isEmpty())
        throw std::runtime_error("Comparison op TAC missing arg2 or result.");

      std::string reg1 = this->getTempReg();
      std::string reg2 = this->getTempReg();
      std::string resReg = this->getTempReg();

      this->loadToReg(quad.arg1, reg1);
      this->loadToReg(quad.arg2, reg2);

      if (quad.op == "==")
        this->text.push_back(this->indent(1) + "seq " + resReg + ", " + reg1 +
                             ", " + reg2);
      else if (quad.op == "!=")
        this->text.push_back(this->indent(1) + "sne " + resReg + ", " + reg1 +
                             ", " + reg2);
      else if (quad.op == "<")
        this->text.push_back(this->indent(1) + "slt " + resReg + ", " + reg1 +
                             ", " + reg2);
      else if (quad.op == "<=") {
        std::string sgt_res_reg = this->getTempReg();
        this->text.push_back(this->indent(1) + "sgt " + sgt_res_reg + ", " +
                             reg1 + ", " + reg2);
        this->text.push_back(this->indent(1) + "xori " + resReg + ", " +
                             sgt_res_reg + ", 1");
        this->releaseTempReg(sgt_res_reg);
      } else if (quad.op == ">") {
        this->text.push_back(this->indent(1) + "sgt " + resReg + ", " + reg1 +
                             ", " + reg2);
      } else if (quad.op == ">=") {
        std::string slt_res_reg = this->getTempReg();
        this->text.push_back(this->indent(1) + "slt " + slt_res_reg + ", " +
                             reg1 + ", " + reg2);
        this->text.push_back(this->indent(1) + "xori " + resReg + ", " +
                             slt_res_reg + ", 1");
        this->releaseTempReg(slt_res_reg);
      }

      this->storeFromReg(quad.result, resReg);
      this->releaseTempReg(reg1);
      this->releaseTempReg(reg2);
      this->releaseTempReg(resReg);
    } else if (quad.op == "&&" || quad.op == "||" || quad.op == "!") {
      // quad.arg1, quad.arg2 (optional for !), quad.result
      if (quad.result.isEmpty())
        throw std::runtime_error("Logical op TAC missing result.");
      std::string reg1 = this->getTempReg();
      std::string resReg = this->getTempReg();
      this->loadToReg(quad.arg1, reg1);

      if (quad.op == "!") {
        this->text.push_back(this->indent(1) + "sltiu " + resReg + ", " + reg1 +
                             ", 1");
      } else {
        if (quad.arg2.isEmpty())
          throw std::runtime_error("'&&' or '||' TAC missing arg2.");
        std::string reg2 = this->getTempReg();
        this->loadToReg(quad.arg2, reg2);
        if (quad.op == "&&") {
          this->text.push_back(this->indent(1) + "and " + resReg + ", " + reg1 +
                               ", " + reg2);
        } else if (quad.op == "||") {
          this->text.push_back(this->indent(1) + "or " + resReg + ", " + reg1 +
                               ", " + reg2);
        }
        this->text.push_back(this->indent(1) + "sltu " + resReg + ", $zero, " +
                             resReg);
        this->releaseTempReg(reg2);
      }
      this->storeFromReg(quad.result, resReg);
      this->releaseTempReg(reg1);
      this->releaseTempReg(resReg);

    } else if (quad.op.rfind("L", 0) == 0 && quad.arg1.isString() &&
               quad.arg1.getString().empty() && quad.arg2.isEmpty() &&
               quad.result.isEmpty()) {
      // Label definition like "L1:". Parser emits: op="L1",
      // arg1=TacOperand(""), arg2=empty, result=empty
      this->text.push_back(quad.op + ":");

    } else if (quad.op == "if_false") {
      // quad.arg1 (condition), quad.arg2 (label string TacOperand)
      if (quad.arg2.isEmpty() || !quad.arg2.isString())
        throw std::runtime_error(
            "'if_false' TAC missing label string in arg2.");
      std::string condReg = this->getTempReg();
      this->loadToReg(quad.arg1, condReg);
      this->text.push_back(this->indent(1) + "beq " + condReg + ", $zero, " +
                           quad.arg2.getString());
      this->releaseTempReg(condReg);

    } else if (quad.op == "goto") {
      // quad.arg1 (label string TacOperand)
      if (quad.arg1.isEmpty() || !quad.arg1.isString())
        throw std::runtime_error("'goto' TAC missing label string in arg1.");
      this->text.push_back(this->indent(1) + "j " + quad.arg1.getString());

    } else if (quad.op == "param") {
      // quad.arg1 is TacOperand (param_value or param_symbol)
      if (formalParamCounter <
          expectedFormalParams) { // Formal parameter processing from func
                                  // definition
        if (!quad.arg1.isSymbol())
          throw std::runtime_error(
              "Formal param TAC expects a Symbol in arg1.");
        const auto &paramSymbol = quad.arg1.getSymbol();
        if (!paramSymbol)
          throw std::runtime_error("Formal param Symbol is null.");
        if (paramSymbol->isGlobalVar())
          throw std::runtime_error("Formal parameter cannot be global: " +
                                   paramSymbol->getName());

        int baseOffsetForLocalsAndParams =
            this->tempStackSpace.count(this->currentFunctionName)
                ? this->tempStackSpace[this->currentFunctionName]
                : 0;
        int effectiveParamSymbolOffsetOnStack =
            paramSymbol->getOffset() + baseOffsetForLocalsAndParams;

        if (formalParamCounter < 4) {
          std::string argReg = "$a" + std::to_string(formalParamCounter);
          this->text.push_back(
              this->indent(1) + "sw " + argReg + ", " +
              std::to_string(effectiveParamSymbolOffsetOnStack) +
              "($fp) # Store formal param " + paramSymbol->getName() +
              " from " + argReg);
        } else { // Passed on stack by caller
          auto funcSymbol = this->table->lookup(this->currentFunctionName);
          if (!funcSymbol)
            throw std::runtime_error(
                "Function symbol not found for stack param: " +
                this->currentFunctionName);
          int localsAndDeclaredParamsSize = funcSymbol->getBlockSize();
          int tempsSize = this->tempStackSpace.count(this->currentFunctionName)
                              ? this->tempStackSpace[this->currentFunctionName]
                              : 0;
          int currentFrameTotalSize =
              localsAndDeclaredParamsSize + tempsSize + 8;
          if (currentFrameTotalSize % 8 != 0)
            currentFrameTotalSize = ((currentFrameTotalSize / 8) + 1) * 8;
          // stack_arg_offset_from_caller_fp_perspective is how caller placed it
          // relative to its $sp ($fp of current func)
          int stack_arg_offset_from_current_fp =
              currentFrameTotalSize + (formalParamCounter - 4) * 4;

          std::string tempReg = this->getTempReg();
          this->text.push_back(
              this->indent(1) + "lw " + tempReg + ", " +
              std::to_string(stack_arg_offset_from_current_fp) +
              "($fp) # Load stack-passed param " + paramSymbol->getName());
          this->text.push_back(
              this->indent(1) + "sw " + tempReg + ", " +
              std::to_string(effectiveParamSymbolOffsetOnStack) +
              "($fp) # Store stack-passed param " + paramSymbol->getName() +
              " to its final slot");
          this->releaseTempReg(tempReg);
        }
        formalParamCounter++;
      } else { // Actual argument for an outgoing call
        if (currentArgReg < 4) {
          std::string argReg = "$a" + std::to_string(currentArgReg);
          // 检查不同类型参数的传递方式
          if (quad.arg1.isSymbol() && quad.arg1.getSymbol()) {
            auto symbol = quad.arg1.getSymbol();
            if (symbol->getType() == SymbolType::STR && symbol->isGlobalVar()) {
              // 传递字符串参数
              this->text.push_back(this->indent(1) + "la " + argReg + ", " +
                                   symbol->getName());
            } else if (symbol->getType() == SymbolType::ARRAY) {
              // 传递数组参数
              std::string tempReg = this->getTempReg();
              if (symbol->isGlobalVar()) {
                // 全局数组，传递基地址
                this->text.push_back(this->indent(1) + "la " + tempReg + ", " +
                                     symbol->getName());
              } else if (!symbol->getDimensions().empty() &&
                         symbol->getDimensions()[0] == -1) {
                // 已经是数组参数，传递其地址（指针）
                int baseOffsetForLocalsAndParams =
                    this->tempStackSpace.count(this->currentFunctionName)
                        ? this->tempStackSpace[this->currentFunctionName]
                        : 0;
                this->text.push_back(
                    this->indent(1) + "lw " + tempReg + ", " +
                    std::to_string(symbol->getOffset() +
                                   baseOffsetForLocalsAndParams) +
                    "($fp) # Load array param pointer for " +
                    symbol->getName());
              } else {
                // 局部数组，传递地址
                int baseOffsetForLocalsAndParams =
                    this->tempStackSpace.count(this->currentFunctionName)
                        ? this->tempStackSpace[this->currentFunctionName]
                        : 0;
                this->text.push_back(
                    this->indent(1) + "addi " + tempReg + ", $fp, " +
                    std::to_string(symbol->getOffset() +
                                   baseOffsetForLocalsAndParams) +
                    " # Local array address " + symbol->getName());
              }
              this->text.push_back(this->indent(1) + "move " + argReg + ", " +
                                   tempReg + " # Pass array address");
              this->releaseTempReg(tempReg);
            } else {
              // 普通变量参数
              std::string tempParamReg = this->getTempReg();
              this->loadToReg(quad.arg1, tempParamReg);
              this->text.push_back(this->indent(1) + "move " + argReg + ", " +
                                   tempParamReg);
              this->releaseTempReg(tempParamReg);
            }
          } else {
            // 非符号参数（字面量等）
            std::string tempParamReg = this->getTempReg();
            this->loadToReg(quad.arg1, tempParamReg);
            this->text.push_back(this->indent(1) + "move " + argReg + ", " +
                                 tempParamReg);
            this->releaseTempReg(tempParamReg);
          }
        } else {
          // 栈参数处理
          std::string tempParamReg = this->getTempReg();
          this->loadToReg(quad.arg1, tempParamReg);
          int stack_param_offset =
              (currentArgReg - 4) * 4; // Offset from $sp for outgoing params
          this->text.push_back(this->indent(1) + "sw " + tempParamReg + ", " +
                               std::to_string(stack_param_offset) +
                               "($sp)  # Param " +
                               std::to_string(currentArgReg + 1) + " on stack");
          this->releaseTempReg(tempParamReg);
        }
        currentArgReg++;
      }

    } else if (quad.op == "call") {
      if (!quad.arg1.isString())
        throw std::runtime_error(
            "'call' TAC expects function name string in arg1.");
      this->text.push_back(this->indent(1) + "jal " + quad.arg1.getString());
      currentArgReg = 0; // Reset for next call
      // expectedFormalParams is for func definition context, not reset here.
      if (!quad.result.isEmpty()) {
        this->storeFromReg(quad.result, "$v0");
      }
      this->resetTempRegs(); // Temps are invalidated by call

    } else if (quad.op == "return") {
      if (!quad.arg1.isEmpty()) {
        std::string retReg = this->getTempReg();
        this->loadToReg(quad.arg1, retReg);
        this->text.push_back(this->indent(1) + "move $v0, " + retReg);
        this->releaseTempReg(retReg);
      }
      if (!this->currentFunctionName.empty()) {
        this->text.push_back(this->indent(1) + "j " +
                             this->currentFunctionName + "_epilogue");
      } else {
        this->text.push_back(
            "# WARNING: 'return' TAC outside of a function context.");
      }

    } else if (quad.op == "[]=") { // Array store: array_sym[index_op] = val_op
      if (quad.arg2.isEmpty() || quad.result.isEmpty() || !quad.arg1.isSymbol())
        throw std::runtime_error("'[]=' TAC format error. arg1 must be symbol, "
                                 "arg2/result present.");
      const auto &arraySymbol = quad.arg1.getSymbol();
      if (!arraySymbol || arraySymbol->getType() != SymbolType::ARRAY)
        throw std::runtime_error(
            "Symbol for '[]=' is not an array or is null.");

      std::string baseAddrReg = this->getTempReg(),
                  indexReg = this->getTempReg(), valReg = this->getTempReg(),
                  offsetReg = this->getTempReg();

      // 获取数组基址
      if (arraySymbol->isGlobalVar()) {
        this->text.push_back(this->indent(1) + "la " + baseAddrReg + ", " +
                             arraySymbol->getName());
      } else if (!arraySymbol->getDimensions().empty() &&
                 arraySymbol->getDimensions()[0] == -1) {
        // 数组参数（指针）
        int baseOff = this->tempStackSpace.count(this->currentFunctionName)
                          ? this->tempStackSpace[this->currentFunctionName]
                          : 0;
        this->text.push_back(
            this->indent(1) + "lw " + baseAddrReg + ", " +
            std::to_string(arraySymbol->getOffset() + baseOff) +
            "($fp) # Load array param pointer");
      } else {
        // 局部数组
        int baseOff = this->tempStackSpace.count(this->currentFunctionName)
                          ? this->tempStackSpace[this->currentFunctionName]
                          : 0;
        this->text.push_back(
            this->indent(1) + "addi " + baseAddrReg + ", $fp, " +
            std::to_string(arraySymbol->getOffset() + baseOff) +
            " # Local array base");
      }

      // 加载索引和值
      this->loadToReg(quad.arg2, indexReg);
      this->loadToReg(quad.result, valReg);

      // 计算元素偏移（每个元素4字节）
      this->text.push_back(this->indent(1) + "sll " + offsetReg + ", " +
                           indexReg + ", 2 # Multiply index by 4");
      // 计算元素地址
      this->text.push_back(this->indent(1) + "add " + baseAddrReg + ", " +
                           baseAddrReg + ", " + offsetReg +
                           " # Add offset to base");
      // 存储值到数组元素
      this->text.push_back(this->indent(1) + "sw " + valReg + ", 0(" +
                           baseAddrReg + ") # Store value to array");

      this->releaseTempReg(baseAddrReg);
      this->releaseTempReg(indexReg);
      this->releaseTempReg(valReg);
      this->releaseTempReg(offsetReg);

    } else if (quad.op == "=[]") { // Array load: dest_temp_operand =
                                   // array_sym_operand[index_operand]
      if (quad.arg2.isEmpty() || quad.result.isEmpty() || !quad.arg1.isSymbol())
        throw std::runtime_error("'=[]' TAC format error. arg1 must be symbol, "
                                 "arg2/result present.");
      const auto &arraySymbol = quad.arg1.getSymbol();
      if (!arraySymbol || arraySymbol->getType() != SymbolType::ARRAY)
        throw std::runtime_error(
            "Symbol for '=[]' is not an array or is null.");

      std::string baseAddrReg = this->getTempReg(),
                  indexReg = this->getTempReg(), valReg = this->getTempReg(),
                  offsetReg = this->getTempReg();

      // 获取数组基址
      if (arraySymbol->isGlobalVar()) {
        this->text.push_back(this->indent(1) + "la " + baseAddrReg + ", " +
                             arraySymbol->getName());
      } else if (!arraySymbol->getDimensions().empty() &&
                 arraySymbol->getDimensions()[0] == -1) {
        // 数组参数（指针）
        int baseOff = this->tempStackSpace.count(this->currentFunctionName)
                          ? this->tempStackSpace[this->currentFunctionName]
                          : 0;
        this->text.push_back(
            this->indent(1) + "lw " + baseAddrReg + ", " +
            std::to_string(arraySymbol->getOffset() + baseOff) +
            "($fp) # Load array param pointer");
      } else {
        // 局部数组
        int baseOff = this->tempStackSpace.count(this->currentFunctionName)
                          ? this->tempStackSpace[this->currentFunctionName]
                          : 0;
        this->text.push_back(
            this->indent(1) + "addi " + baseAddrReg + ", $fp, " +
            std::to_string(arraySymbol->getOffset() + baseOff) +
            " # Local array base");
      }

      // 加载索引
      this->loadToReg(quad.arg2, indexReg);

      // 计算元素偏移（每个元素4字节）
      this->text.push_back(this->indent(1) + "sll " + offsetReg + ", " +
                           indexReg + ", 2 # Multiply index by 4");
      // 计算元素地址
      this->text.push_back(this->indent(1) + "add " + baseAddrReg + ", " +
                           baseAddrReg + ", " + offsetReg +
                           " # Add offset to base");
      // 读取数组元素值
      this->text.push_back(this->indent(1) + "lw " + valReg + ", 0(" +
                           baseAddrReg + ") # Load value from array");
      // 将值存储到目标位置
      this->storeFromReg(quad.result, valReg);

      this->releaseTempReg(baseAddrReg);
      this->releaseTempReg(indexReg);
      this->releaseTempReg(valReg);
      this->releaseTempReg(offsetReg);
    } else {
      this->text.push_back("# UNHANDLED TAC: " + quad.op + " " +
                           getOperandNameForDebug(quad.arg1) + " " +
                           getOptionalOperandNameForDebug(quad.arg2) + " " +
                           getOptionalOperandNameForDebug(quad.result));
    }
  }
}

// Helper function implementations for debug printing TAC operands
std::string Mips::getOperandNameForDebug(const TacOperand &op) {
  if (op.isString()) {
    return op.getString();
  } else if (op.isSymbol()) {
    const auto &sym_ptr = op.getSymbol();
    if (sym_ptr) {
      return sym_ptr->getName(); // Returns symbol's name
    } else {
      return "<null_symbol_ptr>";
    }
  } else if (op.isEmpty()) { // Explicitly check for EMPTY type
    return "<empty_operand>";
  }
  return "<unknown_operand_type>"; // Should not happen with defined types
}

std::string Mips::getOptionalOperandNameForDebug(
    const TacOperand &op) { // Takes TacOperand directly
  if (op.isEmpty()) {
    return ""; // Return empty string if operand is marked as EMPTY
  }
  return getOperandNameForDebug(op); // Otherwise, get its debug name
}