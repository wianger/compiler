#pragma once
#include "lexer.h"
#include "symbol_table.h"
#include <iostream>
#include <memory>
#include <string>
#include <vector>

struct TacOperand {
  enum class Type {
    STRING,     // For identifiers (temp, global label), numbers
    SYMBOL_PTR, // For symbols resolved at parse time
    EMPTY       // Represents an absent operand
  };

  Type type;
  std::string strValue;             // Used if type is STRING
  std::shared_ptr<Symbol> symValue; // Used if type is SYMBOL_PTR

  TacOperand() : type(Type::EMPTY) {}
  TacOperand(const std::string &s) : type(Type::STRING), strValue(s) {}
  TacOperand(std::shared_ptr<Symbol> sym)
      : type(Type::SYMBOL_PTR), symValue(std::move(sym)) {}

  bool isString() const { return type == Type::STRING; }
  bool isSymbol() const { return type == Type::SYMBOL_PTR; }
  bool isEmpty() const { return type == Type::EMPTY; }

  const std::string &getString() const {
    if (type != Type::STRING)
      throw std::runtime_error("TacOperand is not a string");
    return strValue;
  }
  std::shared_ptr<Symbol> getSymbol() const {
    if (type != Type::SYMBOL_PTR)
      throw std::runtime_error("TacOperand is not a symbol pointer");
    return symValue;
  }
  std::string getName() const {
    if (isString())
      return getString();
    if (isSymbol() && symValue)
      return symValue->getName();
    return "";
  }
};

struct Quadruple {
  std::string op; // "+", "*", "=", etc
  TacOperand arg1;
  TacOperand arg2;
  TacOperand result;
  std::shared_ptr<SymbolTable> scope;
};

// For debug printing TacOperand with std::cout
std::ostream &operator<<(std::ostream &os, const TacOperand &operand);

enum class ExpType { Num, Temp, Void };

class Parser {
public:
  Parser(std::vector<Token> tokens)
      : tokens(tokens), position(0), tempCount(0), labelCount(0),
        printfstrCount(0) {
    currentTable = std::make_shared<SymbolTable>();
    root = currentTable;
  }
  std::vector<Quadruple> parse();
  std::shared_ptr<SymbolTable> getSymbolTable() { return root; }

private:
  std::vector<Token> tokens;
  std::vector<Quadruple> tac;
  size_t position;
  int tempCount;
  int labelCount;
  int printfstrCount;
  std::shared_ptr<SymbolTable> root;
  std::shared_ptr<SymbolTable> currentTable;

  Token currentToken() const;
  Token peek(int offset) const;
  bool match(const TokenType &type, int offset = 0);
  void consume(const TokenType &type);
  void splitStringByDelimiter(const std::string &str,
                              const std::string &delimiter);
  int getTempVar();
  int getLabel();
  int getPrintfStrCount();
  void enterScope(bool isFunctionScope = false);
  void exitScope(bool isFunctionScope = false);
  void emit(const std::string &op, TacOperand arg1,
            TacOperand arg2 = TacOperand(), TacOperand result = TacOperand());
  void preprocess();

  void CompUnit();
  void Decl(bool isglobal,
            std::shared_ptr<SymbolTable> funcTopLevelTable = nullptr);
  void FuncDef();
  void MainFuncDef();
  void Block(std::string func_name_or_empty = "", bool is_func_body = false,
             std::shared_ptr<SymbolTable> funcTopLevelTableIfAny = nullptr,
             int startlabel_param = 0, int endlabel_param = 0);
  void Stmt(int startlabel = 0, int endlabel = 0,
            std::shared_ptr<SymbolTable> funcTopLevelTable = nullptr);
  std::pair<ExpType, int> AddExp();
  std::pair<ExpType, int> LOrExp();
  std::pair<ExpType, int> MulExp();
  std::pair<ExpType, int> UnaryExp();
  std::pair<ExpType, int> PrimaryExp();
  std::pair<ExpType, int> LAndExp();
  std::pair<ExpType, int> EqExp();
  std::pair<ExpType, int> RelExp();
};