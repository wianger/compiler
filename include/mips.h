#pragma once
#include "parser.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Mips {
public:
  Mips(std::vector<Quadruple> &tac, std::shared_ptr<SymbolTable> table);
  std::vector<std::string> generateMips();

public:
  std::vector<Quadruple> tac;
  std::shared_ptr<SymbolTable> table;
  std::vector<std::string> mipsCode;
  std::vector<std::string> data, text;
  int labelCount = 0;
  std::unordered_map<std::string, int> maxTempIndices;
  std::unordered_map<std::string, int> tempStackSpace;
  std::string currentFunctionName;

private:
  std::string indent(int level);
  void buildin_func();
  void generateDataSection();
  void generateTextSection();
  void precomputeTempUsage();

  std::string getTempReg();
  void releaseTempReg(const std::string &reg);
  void resetTempRegs();
  bool isNumber(const std::string &s);
  bool isTempVar(const std::string &s);
  std::string loadToReg(const TacOperand &src_op, const std::string &destReg);
  void storeFromReg(const TacOperand &dest_op, const std::string &srcReg);

  // Helper for TAC printing
  std::string getOperandNameForDebug(const TacOperand &op);
  std::string getOptionalOperandNameForDebug(const TacOperand &op);

  std::vector<bool> tempRegsInUse;
  static const int NUM_TEMP_REGS = 10;
};