#ifndef PROTECTION_LANGUAGEPROVIDER_H
#define PROTECTION_LANGUAGEPROVIDER_H

#include <vector>
#include <utility>

#include "Token.h"
#include "Range.h"

namespace protection {

class Token;
using TokenType = size_t;
#define TOKEN_TYPE(type) static_cast<TokenType>(type)

enum class LanguageProviderType { Url, FilePath, Sql };

class LanguageProvider {
public:
  virtual ~LanguageProvider() = default;
  virtual std::vector<Token> tokenize(const std::string &text, size_t offset = 0) = 0;
  virtual std::pair<std::string, bool> trySanitize(const std::string &text, Token context) = 0;

protected:
  virtual Token createToken(TokenType type, size_t lowerBound, size_t upperBound, const std::string &text) const = 0;

  virtual bool isTrivial(TokenType type, const std::string &text) const = 0;
};
} // namespace protection

#endif // PROTECTION_LANGUAGEPROVIDER_H