#pragma once
#include <string>
#include <unordered_map>
#include <vector>

enum class TokenType {
  // Identifiers and constants
  IDENFR, // Identifier
  INTCON, // Integer constant
  STRCON, // Format string

  // Keywords
  MAINTK,     // main
  CONSTTK,    // const
  INTTK,      // int
  BREAKTK,    // break
  CONTINUETK, // continue
  IFTK,       // if
  ELSETK,     // else
  WHILETK,    // while
  GETINTTK,   // getint
  PRINTFTK,   // printf
  RETURNTK,   // return
  VOIDTK,     // void

  // Operators
  NOT,    // !
  AND,    // &&
  OR,     // ||
  PLUS,   // +
  MINU,   // -
  MULT,   // *
  DIV,    // /
  MOD,    // %
  LSS,    // <
  LEQ,    // <=
  GRE,    // >
  GEQ,    // >=
  EQL,    // ==
  NEQ,    // !=
  ASSIGN, // =

  // Delimiters
  SEMICN,  // ;
  COMMA,   // ,
  LPARENT, // (
  RPARENT, // )
  LBRACK,  // [
  RBRACK,  // ]
  LBRACE,  // {
  RBRACE,  // }

  // Error
  ERROR, // Error
};

using Token = std::pair<std::string, TokenType>; // Token type and lexeme

const std::unordered_map<std::string, TokenType> keywords = {
    {"main", TokenType::MAINTK},         {"const", TokenType::CONSTTK},
    {"int", TokenType::INTTK},           {"break", TokenType::BREAKTK},
    {"continue", TokenType::CONTINUETK}, {"if", TokenType::IFTK},
    {"else", TokenType::ELSETK},         {"while", TokenType::WHILETK},
    {"getint", TokenType::GETINTTK},     {"printf", TokenType::PRINTFTK},
    {"return", TokenType::RETURNTK},     {"void", TokenType::VOIDTK}};
const std::unordered_map<std::string, TokenType> operators = {
    {"!", TokenType::NOT},     {"&&", TokenType::AND},
    {"||", TokenType::OR},     {"+", TokenType::PLUS},
    {"-", TokenType::MINU},    {"*", TokenType::MULT},
    {"/", TokenType::DIV},     {"%", TokenType::MOD},
    {"<", TokenType::LSS},     {"<=", TokenType::LEQ},
    {">", TokenType::GRE},     {">=", TokenType::GEQ},
    {"==", TokenType::EQL},    {"!=", TokenType::NEQ},
    {"=", TokenType::ASSIGN},  {";", TokenType::SEMICN},
    {",", TokenType::COMMA},   {"(", TokenType::LPARENT},
    {")", TokenType::RPARENT}, {"[", TokenType::LBRACK},
    {"]", TokenType::RBRACK},  {"{", TokenType::LBRACE},
    {"}", TokenType::RBRACE}};

class Lexer {
public:
  Lexer(std::string &source) : source(std::move(source)), position(0) {}
  std::vector<Token> tokenize();               // Tokenize the source code
  const std::vector<Token> &getTokens() const; // Get the list of tokens

private:
  std::string source;        // Source code
  size_t position;           // Current position in the source code
  std::vector<Token> tokens; // List of tokens

  // Helper functions
  char currentChar() const;           // Get the current character
  char peekChar(size_t offset) const; // Peek at a character
  void advance();                     // Advance to the next character
  void skipWhitespace();              // Skip whitespace characters
  void skipComment();                 // Skip comments
  Token lexIdentifier();              // Lex an identifier
  Token lexNumber();                  // Lex a number
  Token lexString();                  // Lex a string
  Token lexOperator();                // Lex an operator
};
