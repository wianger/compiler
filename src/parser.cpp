#include "parser.h"
#include <iostream>
#include <memory>

std::string tokenTypeToString(TokenType type) {
  switch (type) {
  case TokenType::IDENFR:
    return "IDENFR";
  case TokenType::INTCON:
    return "INTCON";
  case TokenType::STRCON:
    return "STRCON";
  case TokenType::MAINTK:
    return "MAINTK";
  case TokenType::CONSTTK:
    return "CONSTTK";
  case TokenType::INTTK:
    return "INTTK";
  case TokenType::BREAKTK:
    return "BREAKTK";
  case TokenType::CONTINUETK:
    return "CONTINUETK";
  case TokenType::IFTK:
    return "IFTK";
  case TokenType::ELSETK:
    return "ELSETK";
  case TokenType::WHILETK:
    return "WHILETK";
  case TokenType::GETINTTK:
    return "GETINTTK";
  case TokenType::PRINTFTK:
    return "PRINTFTK";
  case TokenType::RETURNTK:
    return "RETURNTK";
  case TokenType::VOIDTK:
    return "VOIDTK";
  case TokenType::NOT:
    return "NOT";
  case TokenType::AND:
    return "AND";
  case TokenType::OR:
    return "OR";
  case TokenType::PLUS:
    return "PLUS";
  case TokenType::MINU:
    return "MINU";
  case TokenType::MULT:
    return "MULT";
  case TokenType::DIV:
    return "DIV";
  case TokenType::MOD:
    return "MOD";
  case TokenType::LSS:
    return "LSS";
  case TokenType::LEQ:
    return "LEQ";
  case TokenType::GRE:
    return "GRE";
  case TokenType::GEQ:
    return "GEQ";
  case TokenType::EQL:
    return "EQL";
  case TokenType::NEQ:
    return "NEQ";
  case TokenType::ASSIGN:
    return "ASSIGN";
  case TokenType::SEMICN:
    return "SEMICN";
  case TokenType::COMMA:
    return "COMMA";
  case TokenType::LPARENT:
    return "LPARENT";
  case TokenType::RPARENT:
    return "RPARENT";
  case TokenType::LBRACK:
    return "LBRACK";
  case TokenType::RBRACK:
    return "RBRACK";
  case TokenType::LBRACE:
    return "LBRACE";
  case TokenType::RBRACE:
    return "RBRACE";
  case TokenType::ERROR:
    return "ERROR";
  }
  return "UNKNOWN";
}

void Parser::splitStringByDelimiter(const std::string &str,
                                    const std::string &delimiter) {
  size_t start = 0;
  size_t end = str.find(delimiter);
  while (end != std::string::npos) {
    std::string token = str.substr(start, end - start);
    if (!token.empty()) {
      std::string name = "printfstr" + std::to_string(getPrintfStrCount());
      Symbol symbol(name, SymbolType::STR, true);
      symbol.setStr(token);
      auto shared_symbol = std::make_shared<Symbol>(symbol);
      root->insert(name, shared_symbol);
      emit("param", TacOperand(shared_symbol));
      emit("call", TacOperand("printstr"));
    }
    if (match(TokenType::COMMA)) {
      consume(TokenType::COMMA);
      auto exp = AddExp();
      std::string exp_value = (exp.first == ExpType::Num)
                                  ? std::to_string(exp.second)
                                  : "t" + std::to_string(exp.second);
      emit("param", TacOperand(exp_value));
      emit("call", TacOperand("printint"));
    } else {
      // This case implies a format string like "Hello World\\n" without any %d,
      // or the last segment of a format string " ... %d end_string_part"
      // If `end == std::string::npos` was true, this is the last part.
      // If `token` was empty, it means "%%d" or start/end with %d.
      // The original code didn't seem to handle an expression if no comma,
      // might be an issue. For now, if no comma, assume no expression for this
      // segment.
    }
    start = end + delimiter.length();
    end = str.find(delimiter, start);
  }
  std::string lastToken = str.substr(start, end);
  if (!lastToken.empty()) {
    std::string name = "printfstr" + std::to_string(getPrintfStrCount());
    Symbol symbol(name, SymbolType::STR, true);
    symbol.setStr(lastToken);
    auto shared_symbol = std::make_shared<Symbol>(symbol);
    root->insert(name, shared_symbol);
    emit("param", TacOperand(shared_symbol));
    emit("call", TacOperand("printstr"));
  }
}

void Parser::emit(const std::string &op, TacOperand arg1, TacOperand arg2,
                  TacOperand result) {
  tac.push_back(
      {op, std::move(arg1), std::move(arg2), std::move(result), currentTable});
}

// Helper to manage a stack of base offsets for nested blocks within a function
std::vector<int> blockBaseOffsetStack;
int currentFuncArgAndLocalSize = 0; // Used by Decl to calc flat offset

int Parser::getTempVar() { return tempCount++; }

int Parser::getLabel() { return labelCount++; }

int Parser::getPrintfStrCount() { return printfstrCount++; }

void Parser::enterScope(bool isFunctionScope) {
  if (isFunctionScope) {
    blockBaseOffsetStack.clear();
    blockBaseOffsetStack.push_back(
        0);                         // Base offset for function's own scope is 0
    currentFuncArgAndLocalSize = 0; // Reset for the new function
  } else {
    if (blockBaseOffsetStack.empty()) {
      // Should not happen if not a function scope, implies global scope or
      // error For safety, push 0, but this indicates an issue if we expect to
      // be in a func.
      blockBaseOffsetStack.push_back(0);
    } else {
      // New nested block starts at the current size of its parent block (in
      // terms of locals) The parent block's current accumulated size
      // (currentFuncArgAndLocalSize for the function, or currentTable->Offset()
      // if we were tracking per-block non-flat size) For flat offsets, new
      // nested block's symbols will add to currentFuncArgAndLocalSize. The base
      // offset for symbols *within this new block* relative to func start is
      // currentFuncArgAndLocalSize.
      blockBaseOffsetStack.push_back(currentFuncArgAndLocalSize);
    }
  }
  auto newTable = std::make_shared<SymbolTable>(currentTable);
  if (currentTable) { // currentTable might be nullptr for the very first global
                      // scope
    currentTable->addChild(newTable);
  }
  currentTable = newTable;
}

void Parser::exitScope(bool isFunctionScope) {
  if (!blockBaseOffsetStack.empty()) {
    blockBaseOffsetStack.pop_back();
  }
  if (isFunctionScope) {
    // currentFuncArgAndLocalSize is already the total for the function.
    // This will be set to the funcSymbol by FuncDef/MainFuncDef after Block
    // returns.
  }
  if (currentTable->getParent() != nullptr)
    currentTable = currentTable->getParent();
  // else: attempting to exit global scope, currentTable might become nullptr if
  // it was root. Or throw error if currentTable was already root and parent is
  // null. For simplicity, assume valid nesting.
}

Token Parser::currentToken() const {
  if (position < tokens.size())
    return tokens[position];
  return {"", TokenType::ERROR};
}

Token Parser::peek(int offset) const {
  if (position + offset < tokens.size())
    return tokens[position + offset];
  return {"", TokenType::ERROR};
}

bool Parser::match(const TokenType &type, int offset) {
  if (peek(offset).second == type)
    return true;
  return false;
}

void Parser::consume(const TokenType &type) {
  if (match(type)) {
    position++;
  } else {
    throw std::runtime_error("Expected token " + tokenTypeToString(type) +
                             " type not found");
  }
}

std::vector<Quadruple> Parser::parse() {
  preprocess();
  CompUnit();
  return tac;
}

// Implementation for TacOperand printing
std::ostream &operator<<(std::ostream &os, const TacOperand &operand) {
  if (operand.isString()) {
    os << operand.getString();
  } else if (operand.isSymbol()) {
    if (operand.getSymbol()) {
      os << operand.getSymbol()->getName(); // Or more detailed info
    } else {
      os << "<null_symbol_ptr>";
    }
  } else if (operand.isEmpty()) {
    os << "<empty>"; // Or just print nothing, or a placeholder
  }
  return os;
}

void Parser::CompUnit() {
  // Global scope setup
  currentTable = std::make_shared<SymbolTable>(); // Global scope table
  root = currentTable; // root is the global symbol table.
  blockBaseOffsetStack.push_back(
      0); // Base for global scope (though not used for func flat offsets)

  while (position < tokens.size() &&
         (match(TokenType::CONSTTK) ||
          (match(TokenType::INTTK) && !match(TokenType::LPARENT, 2))))
    Decl(true, nullptr); // Global declarations
  while (position < tokens.size() &&
         (match(TokenType::VOIDTK) ||
          (match(TokenType::INTTK) && match(TokenType::IDENFR, 1) &&
           match(TokenType::LPARENT, 2))))
    FuncDef();
  if (match(TokenType::INTTK) && match(TokenType::MAINTK, 1) &&
      match(TokenType::LPARENT, 2))
    MainFuncDef();
  else
    throw std::runtime_error("Main function not found");

  // 安全检查：确保在弹出前栈不为空
  if (!blockBaseOffsetStack.empty()) {
    blockBaseOffsetStack.pop_back(); // Pop global scope base offset
  }
}

void Parser::Decl(bool isglobal,
                  std::shared_ptr<SymbolTable> funcTopLevelTable) {
  bool isConst = false;
  if (match(TokenType::CONSTTK)) {
    consume(TokenType::CONSTTK);
    isConst = true; // Mark as const, though not fully utilized in this snippet
  }
  consume(TokenType::INTTK);

  while (match(TokenType::IDENFR)) {
    std::string name = currentToken().first;
    consume(TokenType::IDENFR);

    SymbolType type = SymbolType::INT;
    std::vector<int> dimensions; // 存储数组维度

    // 处理可能存在的多个维度
    while (match(TokenType::LBRACK)) {
      type = SymbolType::ARRAY;
      consume(TokenType::LBRACK);
      auto exp = AddExp();
      if (exp.first == ExpType::Num)
        dimensions.push_back(exp.second);
      else
        throw std::runtime_error("Array dimension must be a constant");
      consume(TokenType::RBRACK);
    }

    auto symbol = std::make_shared<Symbol>(name, type, isglobal);

    if (type == SymbolType::ARRAY) {
      symbol->setDimensions(dimensions);
      symbol->setSize(symbol->getTotalSize());
    } else {
      // 对于非数组类型，确保size为1（表示一个整数的大小）
      symbol->setSize(1);
    }

    int symbolSizeOnStack =
        (type == SymbolType::ARRAY) ? (4 * symbol->getTotalSize()) : 4;

    if (!isglobal) {
      symbol->setOffset(currentFuncArgAndLocalSize);
      currentFuncArgAndLocalSize += symbolSizeOnStack;
    }
    currentTable->insert(name, symbol);

    if (match(TokenType::ASSIGN)) {
      consume(TokenType::ASSIGN);
      if (type == SymbolType::ARRAY && match(TokenType::LBRACE)) {
        consume(TokenType::LBRACE);
        std::vector<int> array_values;
        bool can_init_statically = true;
        std::vector<std::pair<ExpType, int>> init_expressions;

        if (!match(TokenType::RBRACE)) {
          // 处理一维数组或多维数组的第一层初始化
          if (dimensions.size() == 1 || !match(TokenType::LBRACE)) {
            // 一维数组直接初始化元素列表
            do {
              if (match(TokenType::COMMA))
                consume(TokenType::COMMA);
              auto exp = AddExp();
              init_expressions.push_back(exp);
              if (exp.first == ExpType::Num) {
                array_values.push_back(exp.second);
              } else {
                can_init_statically = false;
              }
            } while (match(TokenType::COMMA));
          } else {
            // 处理多维数组（嵌套花括号）的情况
            int outer_index = 0;
            do {
              if (match(TokenType::COMMA))
                consume(TokenType::COMMA);

              if (match(TokenType::LBRACE)) {
                consume(TokenType::LBRACE);
                int inner_index = 0;

                if (!match(TokenType::RBRACE)) {
                  do {
                    if (match(TokenType::COMMA))
                      consume(TokenType::COMMA);
                    auto exp = AddExp();

                    // 计算多维数组的偏移量：outer_index * 第二维大小 +
                    // inner_index
                    if (dimensions.size() > 1) {
                      // 确保安全访问第二维度
                      int second_dim =
                          dimensions.size() > 1 ? dimensions[1] : 1;
                      int flat_index = outer_index * second_dim + inner_index;

                      if (exp.first == ExpType::Num) {
                        // 扩展数组大小
                        while (array_values.size() <= flat_index) {
                          array_values.push_back(0);
                        }
                        array_values[flat_index] = exp.second;
                      } else {
                        can_init_statically = false;
                      }

                      // 构建初始化表达式，包含扁平索引和值
                      init_expressions.push_back(exp);
                      inner_index++;
                    }
                  } while (match(TokenType::COMMA));
                }

                consume(TokenType::RBRACE);
                outer_index++;
              }
            } while (match(TokenType::COMMA));
          }
        }
        consume(TokenType::RBRACE);
        if (can_init_statically && isglobal) {
          symbol->setArray(array_values);
        } else if (!isglobal || !can_init_statically) {
          auto array_sym_for_emit = currentTable->lookup(symbol->getName());
          if (!array_sym_for_emit) {
            throw std::runtime_error(
                "Array symbol not found immediately after insertion: " +
                symbol->getName());
          }

          // 多维数组初始化
          if (dimensions.size() > 1 && init_expressions.size() > 0) {
            // 对于动态初始化，需要逐个元素赋值
            int expr_idx = 0;
            for (size_t i = 0;
                 i < dimensions[0] && expr_idx < init_expressions.size(); ++i) {
              // 确保第二维大小有效
              int second_dim_size = dimensions.size() > 1 ? dimensions[1] : 1;
              for (size_t j = 0;
                   j < second_dim_size && expr_idx < init_expressions.size();
                   ++j) {
                // 计算扁平索引
                int flat_index = i * second_dim_size + j;

                // 使用临时变量计算索引
                int temp_idx = getTempVar();
                emit("=", TacOperand(std::to_string(flat_index)), TacOperand(),
                     TacOperand("t" + std::to_string(temp_idx)));

                std::string exp_val_str =
                    (init_expressions[expr_idx].first == ExpType::Num)
                        ? std::to_string(init_expressions[expr_idx].second)
                        : "t" +
                              std::to_string(init_expressions[expr_idx].second);

                // 使用计算出的扁平索引初始化数组元素
                emit("[]=", TacOperand(array_sym_for_emit),
                     TacOperand("t" + std::to_string(temp_idx)),
                     TacOperand(exp_val_str));

                expr_idx++;
              }
            }
          } else {
            // 一维数组初始化（原有代码）
            for (size_t i = 0; i < init_expressions.size(); ++i) {
              std::string exp_val_str =
                  (init_expressions[i].first == ExpType::Num)
                      ? std::to_string(init_expressions[i].second)
                      : "t" + std::to_string(init_expressions[i].second);
              emit("[]=", TacOperand(array_sym_for_emit),
                   TacOperand(std::to_string(i)), TacOperand(exp_val_str));
            }
          }
        }
      } else {
        auto exp = AddExp();
        std::string exp_value_str = (exp.first == ExpType::Num)
                                        ? std::to_string(exp.second)
                                        : "t" + std::to_string(exp.second);
        if (exp.first == ExpType::Num && isglobal && type == SymbolType::INT) {
          symbol->setValue(exp.second);
        }

        if (!isglobal ||
            !(exp.first == ExpType::Num && type == SymbolType::INT)) {
          emit("=", TacOperand(exp_value_str), TacOperand(),
               TacOperand(symbol));
        }
      }
    }
    if (match(TokenType::COMMA))
      consume(TokenType::COMMA);
    else
      break;
  }
  consume(TokenType::SEMICN);
}

void Parser::FuncDef() {
  RetType retType = RetType::INT;
  if (match(TokenType::VOIDTK)) {
    consume(TokenType::VOIDTK);
    retType = RetType::VOID;
  } else
    consume(TokenType::INTTK);

  std::string name = currentToken().first;
  consume(TokenType::IDENFR);

  auto funcSymbol = std::make_shared<Symbol>(name, SymbolType::FUNC, false);
  funcSymbol->setRetType(retType);
  root->insert(name, funcSymbol); // Add function to global scope

  // Create the function's top-level scope (for params and locals)
  // This scope becomes the funcSymbol's scopeTable
  enterScope(true); // true indicates this is a new function's scope
  std::shared_ptr<SymbolTable> funcActualScopeTable = currentTable;
  funcSymbol->setScopeTable(funcActualScopeTable);

  consume(TokenType::LPARENT);
  std::vector<std::string> params_names;
  std::vector<std::shared_ptr<Symbol>> params_symbols;

  while (match(TokenType::INTTK)) {
    consume(TokenType::INTTK);
    std::string paramName = currentToken().first;
    params_names.push_back(paramName);
    consume(TokenType::IDENFR);

    // 默认为普通整型参数
    SymbolType paramType = SymbolType::INT;
    std::vector<int> dimensions;

    // 检查是否是数组参数
    if (match(TokenType::LBRACK)) {
      paramType = SymbolType::ARRAY;
      consume(TokenType::LBRACK);
      consume(TokenType::RBRACK); // 数组参数第一维空，表示大小未知

      dimensions.push_back(-1); // -1表示第一维大小未知

      // 检查后续维度
      if (match(TokenType::LBRACK)) {
        consume(TokenType::LBRACK);
        auto dim_exp = AddExp();
        if (dim_exp.first != ExpType::Num) {
          throw std::runtime_error("Array dimension must be constant");
        }
        dimensions.push_back(dim_exp.second);
        consume(TokenType::RBRACK);
      } else {
        // 如果是一维数组参数，添加默认的第二维大小1
        dimensions.push_back(1);
      }
    }

    // 创建参数符号
    auto paramSymbol = std::make_shared<Symbol>(paramName, paramType, false);

    if (paramType == SymbolType::ARRAY) {
      paramSymbol->setDimensions(dimensions);
      // 数组参数只传递基址，实际只占用4字节
      paramSymbol->setSize(1); // 作为指针，只占一个整数大小
    }

    paramSymbol->setOffset(currentFuncArgAndLocalSize);
    currentFuncArgAndLocalSize += 4; // 普通参数或数组参数（指针）均占4字节

    funcActualScopeTable->insert(paramName, paramSymbol);
    params_symbols.push_back(paramSymbol);

    if (match(TokenType::COMMA))
      consume(TokenType::COMMA);
  }
  consume(TokenType::RPARENT);

  emit("func", TacOperand(name),
       TacOperand(std::to_string(params_names.size())));

  for (const auto &p_sym : params_symbols) {
    emit("param", TacOperand(p_sym));
  }

  Block(name, true,
        funcActualScopeTable); // Pass the function's top-level scope table

  funcSymbol->setBlockSize(
      currentFuncArgAndLocalSize); // Total size for args and all locals in the
                                   // function

  exitScope(true); // Exit function scope
  emit("endfunc", TacOperand(name));
}

void Parser::MainFuncDef() {
  consume(TokenType::INTTK);
  consume(TokenType::MAINTK);

  std::string name = "main";
  auto funcSymbol = std::make_shared<Symbol>(name, SymbolType::FUNC, false);
  funcSymbol->setRetType(RetType::INT);
  root->insert(name, funcSymbol);

  enterScope(true); // true for function scope
  std::shared_ptr<SymbolTable> mainActualScopeTable = currentTable;
  funcSymbol->setScopeTable(mainActualScopeTable);

  consume(TokenType::LPARENT);
  consume(TokenType::RPARENT);
  emit("func", TacOperand(name), TacOperand("0"));

  Block(name, true, mainActualScopeTable); // Pass main's top-level scope table

  funcSymbol->setBlockSize(
      currentFuncArgAndLocalSize); // Total size for args and all locals in main

  exitScope(true); // Exit main's function scope
  emit("endfunc", TacOperand(name));
}

void Parser::Block(std::string func_name_for_debug_or_empty, bool is_func_body,
                   std::shared_ptr<SymbolTable> funcTopLevelTable,
                   int startlabel_param, int endlabel_param) {
  consume(TokenType::LBRACE);
  if (!is_func_body) { // If it's a nested block, not the function's main block
    enterScope(false);
  }
  // If is_func_body, enterScope(true) was already called by
  // FuncDef/MainFuncDef, and currentTable is already the function's top-level
  // scope. Parameters are already added to funcTopLevelTableIfAny by FuncDef.

  // funcTopLevelTableIfAny should be the symbol table of the function this
  // block belongs to, or nullptr if global.

  while (position < tokens.size() && !match(TokenType::RBRACE)) {
    if (match(TokenType::CONSTTK) || match(TokenType::INTTK))
      Decl(false,
           funcTopLevelTable); // Pass the function's top-level table
    else
      Stmt(startlabel_param, endlabel_param,
           funcTopLevelTable); // Pass received labels
  }
  consume(TokenType::RBRACE);

  if (!is_func_body) {
    exitScope(false);
  }
  // If is_func_body, exitScope(true) will be called by FuncDef/MainFuncDef
  // setBlockSize for the function symbol is also handled there using
  // currentFuncArgAndLocalSize.
}

void Parser::Stmt(int startlabel_param, int endlabel_param,
                  std::shared_ptr<SymbolTable> funcTopLevelTable) {
  if (match(TokenType::IFTK)) {
    consume(TokenType::IFTK);
    consume(TokenType::LPARENT);
    auto exp = LOrExp();
    consume(TokenType::RPARENT);
    int elselabel = getLabel();
    int endstmtlabel = getLabel();

    std::string exp_value = (exp.first == ExpType::Num)
                                ? std::to_string(exp.second)
                                : "t" + std::to_string(exp.second);

    emit("if_false", TacOperand(exp_value),
         TacOperand("L" + std::to_string(elselabel)));

    // Use copies of params to ensure no modification by other parts of the
    // function affects these calls
    int current_stmt_startlabel = startlabel_param;
    int current_stmt_endlabel = endlabel_param;

    Stmt(current_stmt_startlabel, current_stmt_endlabel, funcTopLevelTable);

    if (match(TokenType::ELSETK)) {
      emit("goto", TacOperand("L" + std::to_string(endstmtlabel)));
      emit("L" + std::to_string(elselabel), TacOperand(""));
      consume(TokenType::ELSETK);
      Stmt(current_stmt_startlabel, current_stmt_endlabel, funcTopLevelTable);
      emit("L" + std::to_string(endstmtlabel), TacOperand(""));
    } else {
      emit("L" + std::to_string(elselabel), TacOperand(""));
    }

  } else if (match(TokenType::WHILETK)) {
    consume(TokenType::WHILETK);
    int loopstartlabel = getLabel();
    int loopendlabel = getLabel();
    emit("L" + std::to_string(loopstartlabel), TacOperand(""));
    consume(TokenType::LPARENT);
    auto exp = LOrExp();
    consume(TokenType::RPARENT);
    std::string exp_value = (exp.first == ExpType::Num)
                                ? std::to_string(exp.second)
                                : "t" + std::to_string(exp.second);
    emit("if_false", TacOperand(exp_value),
         TacOperand("L" + std::to_string(loopendlabel)));

    Stmt(loopstartlabel, loopendlabel, funcTopLevelTable);

    emit("goto", TacOperand("L" + std::to_string(loopstartlabel)));
    emit("L" + std::to_string(loopendlabel), TacOperand(""));

  } else if (match(TokenType::BREAKTK)) {
    consume(TokenType::BREAKTK);
    consume(TokenType::SEMICN);
    emit("goto", TacOperand("L" + std::to_string(endlabel_param)), TacOperand(),
         TacOperand("# break to L" + std::to_string(endlabel_param)));
  } else if (match(TokenType::CONTINUETK)) {
    consume(TokenType::CONTINUETK);
    consume(TokenType::SEMICN);
    emit("goto", TacOperand("L" + std::to_string(startlabel_param)),
         TacOperand(),
         TacOperand("# continue to L" + std::to_string(startlabel_param)));
  } else if (match(TokenType::RETURNTK)) {
    consume(TokenType::RETURNTK);
    if (match(TokenType::SEMICN))
      emit("return", TacOperand());
    else {
      auto exp = AddExp();
      if (exp.first != ExpType::Void) {
        std::string exp_value = (exp.first == ExpType::Num)
                                    ? std::to_string(exp.second)
                                    : "t" + std::to_string(exp.second);
        emit("return", TacOperand(exp_value));
      } else {
        emit("return", TacOperand());
      }
    }
    consume(TokenType::SEMICN);
  } else if (match(TokenType::PRINTFTK)) {
    consume(TokenType::PRINTFTK);
    consume(TokenType::LPARENT);
    std::string str = currentToken().first;
    consume(TokenType::STRCON);
    splitStringByDelimiter(str, "%d");
    consume(TokenType::RPARENT);
    consume(TokenType::SEMICN);
  } else if (match(TokenType::LBRACE)) {
    Block("", false, funcTopLevelTable, startlabel_param,
          endlabel_param); // Pass current labels
  } else if (match(TokenType::IDENFR)) {
    int temp_pos = position;
    try {
      std::string name_str = currentToken().first;
      std::vector<std::pair<ExpType, int>> indices;

      consume(TokenType::IDENFR);

      // 收集所有维度的索引
      while (match(TokenType::LBRACK)) {
        consume(TokenType::LBRACK);
        auto index = AddExp();
        indices.push_back(index);
        consume(TokenType::RBRACK);
      }

      consume(TokenType::ASSIGN);

      auto lhs_symbol = currentTable->lookup(name_str);
      if (!lhs_symbol) {
        throw std::runtime_error(
            "Undeclared identifier on LHS of assignment: " + name_str);
      }

      if (match(TokenType::GETINTTK)) {
        consume(TokenType::GETINTTK);
        consume(TokenType::LPARENT);
        int tempVar_for_getint_val = getTempVar();
        std::string getint_temp_name =
            "t" + std::to_string(tempVar_for_getint_val);
        emit("call", TacOperand("getint"), TacOperand(),
             TacOperand(getint_temp_name));

        if (!indices.empty()) {
          // 数组赋值
          if (indices.size() == 1) {
            // 一维数组赋值
            std::string index_value_for_assign =
                (indices[0].first == ExpType::Num)
                    ? std::to_string(indices[0].second)
                    : "t" + std::to_string(indices[0].second);
            emit("[]=", TacOperand(lhs_symbol),
                 TacOperand(index_value_for_assign),
                 TacOperand(getint_temp_name));
          } else if (indices.size() == 2) {
            // 二维数组赋值
            const auto &dimensions = lhs_symbol->getDimensions();
            if (dimensions.size() < 2) {
              throw std::runtime_error(
                  "Invalid array dimensions for assignment: " + name_str);
            }

            // 计算多维数组的偏移量: row_index * col_size + col_index
            int row_temp = getTempVar();
            int col_temp = getTempVar();
            int col_size_temp = getTempVar();
            int final_index_temp = getTempVar();
            int result_temp = getTempVar();

            // 获取行索引和列索引
            std::string row_index_str =
                (indices[0].first == ExpType::Num)
                    ? std::to_string(indices[0].second)
                    : "t" + std::to_string(indices[0].second);

            std::string col_index_str =
                (indices[1].first == ExpType::Num)
                    ? std::to_string(indices[1].second)
                    : "t" + std::to_string(indices[1].second);

            // 将行索引加载到临时变量
            emit("=", TacOperand(row_index_str), TacOperand(),
                 TacOperand("t" + std::to_string(row_temp)));

            // 将列索引加载到临时变量
            emit("=", TacOperand(col_index_str), TacOperand(),
                 TacOperand("t" + std::to_string(col_temp)));

            // 设置列大小（第二维的大小），确保安全访问
            int col_size = dimensions.size() > 1 ? dimensions[1] : 1;
            emit("=", TacOperand(std::to_string(col_size)), TacOperand(),
                 TacOperand("t" + std::to_string(col_size_temp)));

            // 计算：row_index * col_size
            emit("*", TacOperand("t" + std::to_string(row_temp)),
                 TacOperand("t" + std::to_string(col_size_temp)),
                 TacOperand("t" + std::to_string(final_index_temp)));

            // 计算：(row_index * col_size) + col_index
            emit("+", TacOperand("t" + std::to_string(final_index_temp)),
                 TacOperand("t" + std::to_string(col_temp)),
                 TacOperand("t" + std::to_string(final_index_temp)));

            // 使用计算出的索引设置数组元素
            emit("[]=", TacOperand(lhs_symbol),
                 TacOperand("t" + std::to_string(final_index_temp)),
                 TacOperand(getint_temp_name));
          }
        } else {
          emit("=", TacOperand(getint_temp_name), TacOperand(),
               TacOperand(lhs_symbol));
        }
        consume(TokenType::RPARENT);
        consume(TokenType::SEMICN);
      } else {
        auto exp = AddExp();
        std::string exp_value_str = (exp.first == ExpType::Num)
                                        ? std::to_string(exp.second)
                                        : "t" + std::to_string(exp.second);
        if (!indices.empty()) {
          // 数组赋值
          if (indices.size() == 1) {
            // 一维数组
            std::string index_value_for_assign =
                (indices[0].first == ExpType::Num)
                    ? std::to_string(indices[0].second)
                    : "t" + std::to_string(indices[0].second);
            emit("[]=", TacOperand(lhs_symbol),
                 TacOperand(index_value_for_assign), TacOperand(exp_value_str));
          } else if (indices.size() == 2) {
            // 二维数组
            const auto &dimensions = lhs_symbol->getDimensions();
            if (dimensions.size() < 2) {
              throw std::runtime_error(
                  "Invalid array dimensions for assignment: " + name_str);
            }

            // 计算多维数组的偏移量: row_index * col_size + col_index
            int row_temp = getTempVar();
            int col_temp = getTempVar();
            int col_size_temp = getTempVar();
            int final_index_temp = getTempVar();
            int result_temp = getTempVar();

            // 获取行索引和列索引
            std::string row_index_str =
                (indices[0].first == ExpType::Num)
                    ? std::to_string(indices[0].second)
                    : "t" + std::to_string(indices[0].second);

            std::string col_index_str =
                (indices[1].first == ExpType::Num)
                    ? std::to_string(indices[1].second)
                    : "t" + std::to_string(indices[1].second);

            // 将行索引加载到临时变量
            emit("=", TacOperand(row_index_str), TacOperand(),
                 TacOperand("t" + std::to_string(row_temp)));

            // 将列索引加载到临时变量
            emit("=", TacOperand(col_index_str), TacOperand(),
                 TacOperand("t" + std::to_string(col_temp)));

            // 设置列大小（第二维的大小），确保安全访问
            int col_size = dimensions.size() > 1 ? dimensions[1] : 1;
            emit("=", TacOperand(std::to_string(col_size)), TacOperand(),
                 TacOperand("t" + std::to_string(col_size_temp)));

            // 计算：row_index * col_size
            emit("*", TacOperand("t" + std::to_string(row_temp)),
                 TacOperand("t" + std::to_string(col_size_temp)),
                 TacOperand("t" + std::to_string(final_index_temp)));

            // 计算：(row_index * col_size) + col_index
            emit("+", TacOperand("t" + std::to_string(final_index_temp)),
                 TacOperand("t" + std::to_string(col_temp)),
                 TacOperand("t" + std::to_string(final_index_temp)));

            // 使用计算出的索引设置数组元素
            emit("[]=", TacOperand(lhs_symbol),
                 TacOperand("t" + std::to_string(final_index_temp)),
                 TacOperand(exp_value_str));
          }
        } else {
          emit("=", TacOperand(exp_value_str), TacOperand(),
               TacOperand(lhs_symbol));
        }
        consume(TokenType::SEMICN);
      }
    } catch (const std::exception &e) {
      position = temp_pos;
      if (!match(TokenType::SEMICN))
        AddExp();
      consume(TokenType::SEMICN);
    }
  } else {
    AddExp();
    consume(TokenType::SEMICN);
  }
}

std::pair<ExpType, int> Parser::AddExp() {
  auto exp1 = MulExp();
  while (match(TokenType::PLUS) || match(TokenType::MINU)) {
    std::string op = currentToken().first;
    consume(currentToken().second);
    auto exp2 = MulExp();
    int result = 0;
    if (exp1.first == ExpType::Num && exp2.first == ExpType::Num) {
      if (op == "+")
        result = exp1.second + exp2.second;
      else
        result = exp1.second - exp2.second;
      exp1 = {ExpType::Num, result};
    } else {
      result = getTempVar();
      std::string exp1_value = (exp1.first == ExpType::Num)
                                   ? std::to_string(exp1.second)
                                   : "t" + std::to_string(exp1.second);
      std::string exp2_value = (exp2.first == ExpType::Num)
                                   ? std::to_string(exp2.second)
                                   : "t" + std::to_string(exp2.second);
      emit(op, TacOperand(exp1_value), TacOperand(exp2_value),
           TacOperand("t" + std::to_string(result)));
      exp1 = {ExpType::Temp, result};
    }
  }
  return exp1;
}

std::pair<ExpType, int> Parser::MulExp() {
  auto exp1 = UnaryExp();
  while (match(TokenType::MULT) || match(TokenType::DIV) ||
         match(TokenType::MOD)) {
    std::string op = currentToken().first;
    consume(currentToken().second);
    auto exp2 = UnaryExp();
    int result = 0;
    if (exp1.first == ExpType::Num && exp2.first == ExpType::Num) {
      if (op == "*")
        result = exp1.second * exp2.second;
      else if (op == "/")
        result = exp1.second / exp2.second;
      else
        result = exp1.second % exp2.second;
      exp1 = {ExpType::Num, result};
    } else {
      result = getTempVar();
      std::string exp1_value = (exp1.first == ExpType::Num)
                                   ? std::to_string(exp1.second)
                                   : "t" + std::to_string(exp1.second);
      std::string exp2_value = (exp2.first == ExpType::Num)
                                   ? std::to_string(exp2.second)
                                   : "t" + std::to_string(exp2.second);
      emit(op, TacOperand(exp1_value), TacOperand(exp2_value),
           TacOperand("t" + std::to_string(result)));
      exp1 = {ExpType::Temp, result};
    }
  }
  return exp1;
}

std::pair<ExpType, int> Parser::UnaryExp() {
  if (match(TokenType::PLUS) || match(TokenType::MINU) ||
      match(TokenType::NOT)) {
    std::string op_str = currentToken().first;
    consume(currentToken().second);
    auto exp = UnaryExp();
    int result_temp_idx = 0;
    if (exp.first == ExpType::Num) {
      int num_val = 0;
      if (op_str == "+")
        num_val = exp.second;
      else if (op_str == "-")
        num_val = -exp.second;
      else // NOT
        num_val = !exp.second;
      return {ExpType::Num, num_val};
    } else { // exp.first == ExpType::Temp
      if (op_str == "+")
        return exp; // No TAC needed for unary +

      std::string actual_op_for_tac = op_str;
      if (op_str == "-")
        actual_op_for_tac = "neg";
      // For "!", actual_op_for_tac remains "!"

      result_temp_idx = getTempVar();
      std::string exp_operand_str = "t" + std::to_string(exp.second);
      emit(actual_op_for_tac, TacOperand(exp_operand_str), TacOperand(),
           TacOperand("t" + std::to_string(result_temp_idx)));
      return {ExpType::Temp, result_temp_idx};
    }
  } else if (match(TokenType::IDENFR) && match(TokenType::LPARENT, 1)) {
    std::string func_name_str = currentToken().first;
    // auto symbol = currentTable->lookup(func_name_str); // Symbol for function
    // itself, not used in emit directly by name now Instead, func_name_str
    // (string) is used for call TAC.

    consume(TokenType::IDENFR);
    consume(TokenType::LPARENT);
    std::vector<TacOperand> params_for_tac;

    if (!match(TokenType::RPARENT)) {
      auto first_param_exp = AddExp();
      std::string first_param_val_str =
          (first_param_exp.first == ExpType::Num)
              ? std::to_string(first_param_exp.second)
              : "t" + std::to_string(first_param_exp.second);
      params_for_tac.push_back(TacOperand(first_param_val_str));

      while (match(TokenType::COMMA)) {
        consume(TokenType::COMMA);
        auto next_param_exp = AddExp();
        std::string next_param_val_str =
            (next_param_exp.first == ExpType::Num)
                ? std::to_string(next_param_exp.second)
                : "t" + std::to_string(next_param_exp.second);
        params_for_tac.push_back(TacOperand(next_param_val_str));
      }
    }
    consume(TokenType::RPARENT);

    // Before emitting call, emit all params
    for (const auto &param_op : params_for_tac) {
      emit("param", param_op); // Updated
    }

    auto func_sym_check =
        root->lookup(func_name_str); // Check from global scope for func def
    if (!func_sym_check || func_sym_check->getType() != SymbolType::FUNC) {
      throw std::runtime_error("Function " + func_name_str +
                               " not found or not a function.");
    }

    if (func_sym_check->getRetType() == RetType::VOID) {
      // emit("call", func_name_str, std::to_string(params_for_tac.size())); //
      // OLD
      emit("call", TacOperand(func_name_str),
           TacOperand(std::to_string(params_for_tac.size()))); // Updated
      return {ExpType::Void, 0};
    } else {
      int result_temp_idx = getTempVar();
      // emit("call", func_name_str, std::to_string(params_for_tac.size()), "t"
      // + std::to_string(result_temp_idx)); // OLD
      emit("call", TacOperand(func_name_str),
           TacOperand(std::to_string(params_for_tac.size())),
           TacOperand("t" + std::to_string(result_temp_idx))); // Updated
      return {ExpType::Temp, result_temp_idx};
    }

  } else
    return PrimaryExp();
  // return {ExpType::Void, 0}; // Should be unreachable if PrimaryExp always
  // returns non-void or throws
}

std::pair<ExpType, int> Parser::PrimaryExp() {
  if (match(TokenType::LPARENT)) {
    consume(TokenType::LPARENT);
    auto exp = AddExp();
    consume(TokenType::RPARENT);
    return exp;
  } else if (match(TokenType::IDENFR)) {
    std::string name_str = currentToken().first;
    consume(TokenType::IDENFR);

    auto sym = currentTable->lookup(name_str);
    if (!sym) {
      throw std::runtime_error("Undeclared identifier in PrimaryExp: " +
                               name_str);
    }

    // 收集所有的索引表达式
    std::vector<std::pair<ExpType, int>> indices;

    while (match(TokenType::LBRACK)) {
      consume(TokenType::LBRACK);
      auto index_exp = AddExp();
      consume(TokenType::RBRACK);
      indices.push_back(index_exp);
    }

    if (!indices.empty()) {
      // 有索引，是数组访问
      if (sym->getType() != SymbolType::ARRAY) {
        throw std::runtime_error("Non-array variable used with index: " +
                                 name_str);
      }

      const auto &dimensions = sym->getDimensions();

      if (indices.size() == 1) {
        // 一维数组访问
        std::string index_value_str =
            (indices[0].first == ExpType::Num)
                ? std::to_string(indices[0].second)
                : "t" + std::to_string(indices[0].second);
        int tempVar = getTempVar();
        emit("=[]", TacOperand(sym), TacOperand(index_value_str),
             TacOperand("t" + std::to_string(tempVar)));
        return {ExpType::Temp, tempVar};
      } else if (indices.size() == 2 && dimensions.size() >= 2) {
        // 二维数组访问
        // 计算偏移量: row_index * col_size + col_index
        int row_temp = getTempVar();
        int col_temp = getTempVar();
        int col_size_temp = getTempVar();
        int final_index_temp = getTempVar();
        int result_temp = getTempVar();

        // 获取行索引和列索引
        std::string row_index_str =
            (indices[0].first == ExpType::Num)
                ? std::to_string(indices[0].second)
                : "t" + std::to_string(indices[0].second);

        std::string col_index_str =
            (indices[1].first == ExpType::Num)
                ? std::to_string(indices[1].second)
                : "t" + std::to_string(indices[1].second);

        // 将行索引加载到临时变量
        emit("=", TacOperand(row_index_str), TacOperand(),
             TacOperand("t" + std::to_string(row_temp)));

        // 将列索引加载到临时变量
        emit("=", TacOperand(col_index_str), TacOperand(),
             TacOperand("t" + std::to_string(col_temp)));

        // 设置列大小（第二维的大小），确保安全访问
        int col_size = dimensions.size() > 1 ? dimensions[1] : 1;
        emit("=", TacOperand(std::to_string(col_size)), TacOperand(),
             TacOperand("t" + std::to_string(col_size_temp)));

        // 计算：row_index * col_size
        emit("*", TacOperand("t" + std::to_string(row_temp)),
             TacOperand("t" + std::to_string(col_size_temp)),
             TacOperand("t" + std::to_string(final_index_temp)));

        // 计算：(row_index * col_size) + col_index
        emit("+", TacOperand("t" + std::to_string(final_index_temp)),
             TacOperand("t" + std::to_string(col_temp)),
             TacOperand("t" + std::to_string(final_index_temp)));

        // 使用计算出的索引访问数组
        emit("=[]", TacOperand(sym),
             TacOperand("t" + std::to_string(final_index_temp)),
             TacOperand("t" + std::to_string(result_temp)));

        return {ExpType::Temp, result_temp};
      } else {
        throw std::runtime_error("Invalid array access dimensions for: " +
                                 name_str);
      }
    } else { // Simple variable access
      int tempVar = getTempVar();
      // emit("=", name_str, "", "t" + std::to_string(tempVar)); // OLD
      // For "=", arg1 is source symbol, result is temp (string)
      emit("=", TacOperand(sym), TacOperand(),
           TacOperand("t" + std::to_string(tempVar))); // Updated
      return {ExpType::Temp, tempVar};
    }
  } else if (match(TokenType::INTCON)) {
    int value = std::stoi(currentToken().first);
    consume(TokenType::INTCON);
    return {ExpType::Num, value};
  }
  // Removed redundant INTCON match, the one above has more debug info.
  // else if (match(TokenType::INTCON)) {
  // return {ExpType::Num, std::stoi(currentToken().first)};
  // }
  return {ExpType::Void,
          0}; // Should ideally not be reached if grammar is complete
}

std::pair<ExpType, int> Parser::LOrExp() {
  auto exp1 = LAndExp(); // Evaluate the left-hand side

  while (match(TokenType::OR)) {
    consume(TokenType::OR); // Consume "||"

    int result_temp = getTempVar(); // Temporary for the final result of "||"
    std::string result_temp_str = "t" + std::to_string(result_temp);
    // int label_true = getLabel(); // Label to jump to if exp1 is true
    // (short-circuit)
    int label_eval_exp2 = getLabel(); // Label to jump to if exp1 is false
    int label_end = getLabel();       // Label to jump to after evaluation

    std::string exp1_val_str;
    if (exp1.first == ExpType::Num) {
      exp1_val_str = std::to_string(exp1.second);
    } else { // ExpType::Temp
      exp1_val_str = "t" + std::to_string(exp1.second);
    }

    // Short-circuit: if exp1 is false, jump to label_eval_exp2.
    // Otherwise (exp1 is true), result is 1, then jump to label_end.
    emit("if_false", TacOperand(exp1_val_str),
         TacOperand("L" + std::to_string(label_eval_exp2)));
    // Exp1 was true path:
    emit("=", TacOperand("1"), TacOperand(), TacOperand(result_temp_str));
    emit("goto", TacOperand("L" + std::to_string(label_end)));

    // Exp1 was false path: evaluate exp2
    emit("L" + std::to_string(label_eval_exp2), TacOperand(""));
    auto exp2 =
        LAndExp(); // Note: LOrExp calls LAndExp, which calls EqExp, etc.
    std::string exp2_val_str;
    if (exp2.first == ExpType::Num) {
      exp2_val_str = std::to_string(exp2.second);
    } else { // ExpType::Temp
      exp2_val_str = "t" + std::to_string(exp2.second);
    }
    // The result of "||" is the result of exp2 if exp1 was false
    emit("=", TacOperand(exp2_val_str), TacOperand(),
         TacOperand(result_temp_str));
    // Control flows naturally to label_end after this.

    // End path
    emit("L" + std::to_string(label_end), TacOperand(""));

    exp1 = {ExpType::Temp,
            result_temp}; // Update exp1 to be the result of the "||" operation
  }
  return exp1;
}

std::pair<ExpType, int> Parser::LAndExp() {
  auto exp1 = EqExp(); // Evaluate the left-hand side

  while (match(TokenType::AND)) {
    consume(TokenType::AND); // Consume "&&"

    int result_temp = getTempVar(); // Temporary for the final result of "&&"
    std::string result_temp_str = "t" + std::to_string(result_temp);
    int label_false =
        getLabel(); // Label to jump to if exp1 is false (short-circuit)
    int label_end = getLabel(); // Label to jump to after evaluating exp2 or
                                // short-circuiting

    std::string exp1_val_str;
    if (exp1.first == ExpType::Num) {
      exp1_val_str = std::to_string(exp1.second);
    } else { // ExpType::Temp
      exp1_val_str = "t" + std::to_string(exp1.second);
    }

    // Short-circuit: if exp1 is false, jump to label_false and set result to 0
    emit("if_false", TacOperand(exp1_val_str),
         TacOperand("L" + std::to_string(label_false)));

    // If exp1 was true, evaluate exp2
    auto exp2 = EqExp();
    std::string exp2_val_str;
    if (exp2.first == ExpType::Num) {
      exp2_val_str = std::to_string(exp2.second);
    } else { // ExpType::Temp
      exp2_val_str = "t" + std::to_string(exp2.second);
    }
    // The result of "&&" is the result of exp2 if exp1 was true
    emit("=", TacOperand(exp2_val_str), TacOperand(),
         TacOperand(result_temp_str));
    emit("goto", TacOperand("L" + std::to_string(label_end)));

    // False path for exp1
    emit("L" + std::to_string(label_false), TacOperand(""));
    emit("=", TacOperand("0"), TacOperand(),
         TacOperand(result_temp_str)); // Result is 0

    // End path
    emit("L" + std::to_string(label_end), TacOperand(""));

    exp1 = {ExpType::Temp,
            result_temp}; // Update exp1 to be the result of the "&&" operation
  }
  return exp1;
}

std::pair<ExpType, int> Parser::EqExp() {
  auto exp1 = RelExp();
  while (match(TokenType::EQL) || match(TokenType::NEQ)) {
    std::string op = currentToken().first;
    consume(currentToken().second);
    auto exp2 = RelExp();
    int result = 0;
    if (exp1.first == ExpType::Num && exp2.first == ExpType::Num) {
      if (op == "==")
        result = exp1.second == exp2.second;
      else
        result = exp1.second != exp2.second;
      exp1 = {ExpType::Num, result};
    } else {
      result = getTempVar();
      std::string exp1_value = (exp1.first == ExpType::Num)
                                   ? std::to_string(exp1.second)
                                   : "t" + std::to_string(exp1.second);
      std::string exp2_value = (exp2.first == ExpType::Num)
                                   ? std::to_string(exp2.second)
                                   : "t" + std::to_string(exp2.second);
      // emit(op, exp1_value, exp2_value, "t" + std::to_string(result)); // OLD
      emit(op, TacOperand(exp1_value), TacOperand(exp2_value),
           TacOperand("t" + std::to_string(result))); // Updated
      exp1 = {ExpType::Temp, result};
    }
  }
  return exp1;
}

std::pair<ExpType, int> Parser::RelExp() {
  auto exp1 = AddExp();
  while (match(TokenType::LSS) || match(TokenType::LEQ) ||
         match(TokenType::GRE) || match(TokenType::GEQ)) {
    std::string op = currentToken().first;
    consume(currentToken().second);
    auto exp2 = AddExp();
    int result = 0;
    if (exp1.first == ExpType::Num && exp2.first == ExpType::Num) {
      if (op == "<")
        result = exp1.second < exp2.second;
      else if (op == "<=")
        result = exp1.second <= exp2.second;
      else if (op == ">")
        result = exp1.second > exp2.second;
      else
        result = exp1.second >= exp2.second;
      exp1 = {ExpType::Num, result};
    } else {
      result = getTempVar();
      std::string exp1_value = (exp1.first == ExpType::Num)
                                   ? std::to_string(exp1.second)
                                   : "t" + std::to_string(exp1.second);
      std::string exp2_value = (exp2.first == ExpType::Num)
                                   ? std::to_string(exp2.second)
                                   : "t" + std::to_string(exp2.second);
      // emit(op, exp1_value, exp2_value, "t" + std::to_string(result)); // OLD
      emit(op, TacOperand(exp1_value), TacOperand(exp2_value),
           TacOperand("t" + std::to_string(result))); // Updated
      exp1 = {ExpType::Temp, result};
    }
  }
  return exp1;
}

void Parser::preprocess() {
  std::unordered_map<std::string, int> const_vars;
  std::unordered_map<std::string, std::vector<int>> const_array_vars;
  while (position < tokens.size()) {
    if (match(TokenType::CONSTTK) && !match(TokenType::LBRACK, 3)) {
      consume(TokenType::CONSTTK);
      consume(TokenType::INTTK);
      while (match(TokenType::IDENFR)) {
        std::string name = currentToken().first;
        consume(TokenType::IDENFR);
        consume(TokenType::ASSIGN);
        auto exp = AddExp();
        if (exp.first == ExpType::Num) {
          const_vars[name] = exp.second;
        }
        if (match(TokenType::COMMA))
          consume(TokenType::COMMA);
      }
      consume(TokenType::SEMICN);
    } else if (match(TokenType::IDENFR) && !match(TokenType::LBRACK, 1) &&
               const_vars.find(currentToken().first) != const_vars.end()) {
      Token &token = tokens[position];
      token.first = std::to_string(const_vars[currentToken().first]);
      consume(TokenType::IDENFR);
      token.second = TokenType::INTCON;
    } else
      position++;
  }
  position = 0; // Reset position for parsing
  tempCount = 0;
}
