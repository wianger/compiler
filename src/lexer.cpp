#include "lexer.h"
#include <cctype>

const std::vector<Token> &Lexer::getTokens() const { return tokens; }

char Lexer::currentChar() const {
  return position < source.size() ? source[position] : '\0';
}

char Lexer::peekChar(size_t offset) const {
  return position + offset < source.size() ? source[position + offset] : '\0';
}

void Lexer::advance() {
  if (position < source.size())
    position++;
}

void Lexer::skipWhitespace() {
  while (position < source.size() && isspace(currentChar()))
    advance();
}

void Lexer::skipComment() {
  if (currentChar() == '/') {
    if (peekChar(1) == '/') {
      while (position < source.size() && currentChar() != '\n')
        advance();
    } else if (peekChar(1) == '*') {
      advance(); // Skip the first '*'
      advance(); // Skip the second '/'
      while (position < source.size()) {
        if (currentChar() == '*' && peekChar(1) == '/') {
          advance(); // Skip the first '*'
          advance(); // Skip the second '/'
          break;
        }
        advance();
      }
    }
  }
}

Token Lexer::lexIdentifier() {
  std::string identifier;
  while (position < source.size() &&
         (isalnum(currentChar()) || currentChar() == '_')) {
    identifier += currentChar();
    advance();
  }
  auto it = keywords.find(identifier);
  if (it != keywords.end())
    return {identifier, it->second};
  return {identifier, TokenType::IDENFR};
}

Token Lexer::lexNumber() {
  std::string number;
  while (position < source.size() && isdigit(currentChar())) {
    number += currentChar();
    advance();
  }
  return {number, TokenType::INTCON};
}

Token Lexer::lexString() {
  std::string str;
  advance(); // Skip the opening '"'
  while (position < source.size() && currentChar() != '"') {
    if (currentChar() == '\\') {
      advance(); // Skip the '\'
      if (currentChar() == 'n')
        str += "\\n";
      else if (currentChar() == 't')
        str += "\\t";
      else
        str += currentChar();
    } else {
      str += currentChar();
    }
    advance();
  }
  advance(); // Skip the closing '"'
  return {str, TokenType::STRCON};
}

Token Lexer::lexOperator() {
  std::string op(1, currentChar());
  advance();
  if (currentChar() == '=' || currentChar() == '&' || currentChar() == '|') {
    op += currentChar();
    advance();
  }
  auto it = operators.find(op);
  if (it != operators.end())
    return {op, it->second};
  return {op, TokenType::ERROR};
}

std::vector<Token> Lexer::tokenize() {
  while (position < source.size()) {
    skipWhitespace();
    while (position < source.size() && currentChar() == '/' &&
           (peekChar(1) == '/' || peekChar(1) == '*')) {
      skipComment();
      skipWhitespace();
    }
    if (isalpha(currentChar()) || currentChar() == '_') {
      tokens.push_back(lexIdentifier());
    } else if (isdigit(currentChar())) {
      tokens.push_back(lexNumber());
    } else if (currentChar() == '"') {
      tokens.push_back(lexString());
    } else if (operators.find(std::string(1, currentChar())) !=
                   operators.end() ||
               currentChar() != '&' || currentChar() != '|') {
      tokens.push_back(lexOperator());
    } else if (currentChar() != '\0') {
      tokens.push_back({std::string(1, currentChar()), TokenType::ERROR});
      advance();
    }
  }
  return tokens;
}
