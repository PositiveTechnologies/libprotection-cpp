#include "HTMLLexer.h"

#include "protection/Languages/Html.h"
#include "protection/Languages/JavaScript.h"
#include "protection/Languages/Url.h"
#include "support/WebUtils.h"

#include <cctype>

namespace protection {
namespace injections {

struct Html::IslandDto {
  LanguageProvider *languageProvider;
  size_t offset;
  std::string text;
};

TokenType Html::convertAntlrTokenType(size_t antlrTokenType) const { return antlrTokenType; }

Token Html::createToken(TokenType type, size_t lowerBound, size_t upperBound, const std::string &text) const {
  return Token(this, type, lowerBound, upperBound, text, isTrivial(type, text));
}

std::unique_ptr<antlr4::Lexer> Html::createLexer(const std::string &text, antlr4::CharStream *charStream) const {
  return std::unique_ptr<html::HTMLLexer>(new html::HTMLLexer(charStream));
}

std::vector<Token> Html::tokenize(const std::string &text, size_t offset) const {
  static const std::set<std::string> htmlUrlAttributes = {"href",     "src",        "manifest",   "poster",   "code",
                                                          "codebase", "data",       "xlink:href", "xml:base", "from",
                                                          "to",       "formaction", "action",     "dynsrc",   "lowsrc"};
  auto state = HtmlTokenizerState::Insignificant;
  auto insideScriptTag = false;

  std::vector<Token> tokens;

  for (const auto &token : AntlrLanguageProvider::tokenize(text, offset)) {
    if (dynamic_cast<decltype(this)>(token.languageProvider) != nullptr) {
      auto htmlTokenType = static_cast<HtmlTokenType>(token.tokenType);
      switch (state) {
      case HtmlTokenizerState::EventName:
        state = (htmlTokenType == HtmlTokenType::TagEquals) ? HtmlTokenizerState::EventEqualSign
                                                            : HtmlTokenizerState::Insignificant;
        break;
      case HtmlTokenizerState::EventEqualSign:
        state = (htmlTokenType == HtmlTokenType::AttributeValue) ? HtmlTokenizerState::EventValue
                                                                 : HtmlTokenizerState::Insignificant;
        break;

      case HtmlTokenizerState::ResourceName:
        state = (htmlTokenType == HtmlTokenType::TagEquals) ? HtmlTokenizerState::ResourceEqualSign
                                                            : HtmlTokenizerState::Insignificant;
        break;

      case HtmlTokenizerState::ResourceEqualSign:
        state = (htmlTokenType == HtmlTokenType::AttributeValue) ? HtmlTokenizerState::ResourceValue
                                                                 : HtmlTokenizerState::Insignificant;
        break;

      default:
        auto ignoreCaseEqual = [](unsigned char a, unsigned char b) { return (std::toupper(a) == std::toupper(b)); };

        state = HtmlTokenizerState::Insignificant;
        switch (htmlTokenType) {
        case HtmlTokenType::AttributeName: {

          auto startsWithIgnoreCase = [&ignoreCaseEqual](const std::string &text, const std::string &substr) {
            if (text.length() < substr.length()) {
              return false;
            }

            return std::equal(substr.begin(), substr.end(), text.begin(), ignoreCaseEqual);
          };

          if (startsWithIgnoreCase(token.text, "on")) {
            state = HtmlTokenizerState::EventName;
          } else if (htmlUrlAttributes.find(token.text) != htmlUrlAttributes.end()) {
            state = HtmlTokenizerState::ResourceName;
          }
        } break;

        case HtmlTokenType::TagOpen:
          if (std::equal(token.text.begin(), token.text.end(), std::string{"<script"}.begin(), ignoreCaseEqual)) {
            insideScriptTag = true;
          } else if (std::equal(token.text.begin(), token.text.end(), std::string{"</script"}.begin(),
                                ignoreCaseEqual)) {
            insideScriptTag = false;
          }
          break;

        default:
          break;
        }
        break;
      }

      auto islandData = isContextChanged(token, state, insideScriptTag);
      if (islandData) {
        auto islandTokens = islandData->languageProvider->tokenize(islandData->text, islandData->offset);
        for (const auto &islandToken : islandTokens) {
          tokens.push_back(islandToken);
        }
      } else {
        tokens.push_back(token);
      }
    }
  }

  return tokens;
}

std::unique_ptr<Html::IslandDto> Html::isContextChanged(const Token &htmlToken, HtmlTokenizerState context,
                                                        bool insideScriptTag) const {
  std::unique_ptr<IslandDto> islandData;
  switch (context) {
  case HtmlTokenizerState::EventValue: {
    size_t offset{0};
    auto islandText = trimQuotes(htmlToken, offset);

    if (!islandText.empty()) {
      islandData = std::unique_ptr<IslandDto>{new IslandDto{&Single<JavaScript>::instance(), offset, islandText}};
    }

    break;
  }

  case HtmlTokenizerState::ResourceValue: {
    size_t offset{0};
    auto islandText = trimQuotes(htmlToken, offset);
    if (!islandText.empty()) {
      islandData = std::unique_ptr<IslandDto>{new IslandDto{&Single<Url>::instance(), offset, islandText}};
    }

    break;
  }

  default:

    if (insideScriptTag) {
      switch (static_cast<HtmlTokenType>(htmlToken.tokenType)) {
      case HtmlTokenType::HtmlText:
        islandData = std::unique_ptr<IslandDto>{
            new IslandDto{&Single<JavaScript>::instance(), htmlToken.range.lowerBound, htmlToken.text}};
        break;

      case HtmlTokenType::HtmlComment:
        // TODO: implement
        break;
      default:
        break;
      }
    }

    break;
  }

  return islandData;
}

std::string Html::trimQuotes(const Token &token, size_t &offset) const {
  auto tokenText = token.text;
  offset = token.range.lowerBound;

  if (tokenText.length() == 0) {
    return {};
  }

  if ((tokenText[0] == '\'' && tokenText[tokenText.length() - 1] == '\'') ||
      (tokenText[0] == '"' && tokenText[tokenText.length() - 1] == '"')) {
    if (tokenText.length() > 2) {
      tokenText = tokenText.substr(1, tokenText.length() - 2);
      ++offset;
    }
  }

  return tokenText;
}

std::pair<std::string, bool> Html::trySanitize(const std::string &text, const Token &context) const {
  if (dynamic_cast<decltype(this)>(context.languageProvider) != nullptr) {
    return {htmlEncode(text, static_cast<HtmlTokenType>(context.tokenType)), true};
  } else if (dynamic_cast<const Url *>(context.languageProvider) != nullptr) {
    auto urlSanitized = Single<Url>::instance().trySanitize(text, context);
    if (urlSanitized.second) {
      return {htmlEncode(urlSanitized.first, HtmlTokenType::AttributeValue), true};
    }
  } else if (dynamic_cast<const JavaScript *>(context.languageProvider) != nullptr) {
    auto ecmaScriptSanitized = Single<JavaScript>::instance().trySanitize(text, context);
    if (ecmaScriptSanitized.second) {
      return {htmlEncode(ecmaScriptSanitized.first, HtmlTokenType::HtmlText), true};
    }
  } else {
    throw std::runtime_error{"Unsupported HTML island: " + context.toString()};
  }

  return {{}, false};
}

bool Html::isTrivial(TokenType type, const std::string & /*unused*/) const {
  switch (static_cast<HtmlTokenType>(type)) {
  case HtmlTokenType::HtmlComment:
  case HtmlTokenType::HtmlConditionalComment:
  case HtmlTokenType::HtmlText:
  case HtmlTokenType::Cdata:
  case HtmlTokenType::AttributeWhiteSpace:
  case HtmlTokenType::AttributeValue:
    return true;
  default:
    break;
  }

  return false;
}

std::string Html::htmlEncode(const std::string &text, HtmlTokenType type) const {
  switch (type) {
  case HtmlTokenType::AttributeValue:
    return utility::HtmlAttributeEncode(text);
  default:
    return utility::HtmlEncode(text);
  }
}

std::string Html::tokenTypeToString(TokenType type) const {
  static const std::map<HtmlTokenType, std::string> tokenTypeToStringMap = {
      {HtmlTokenType::HtmlComment, "HtmlComment"},
      {HtmlTokenType::HtmlConditionalComment, "HtmlConditionalComment"},
      {HtmlTokenType::XmlDeclaration, "XmlDeclaration"},
      {HtmlTokenType::Cdata, "Cdata"},
      {HtmlTokenType::Dtd, "Dtd"},
      {HtmlTokenType::SpecialTag, "SpecialTag"},
      {HtmlTokenType::TagOpen, "TagOpen"},
      {HtmlTokenType::HtmlText, "HtmlText"},
      {HtmlTokenType::ErrorText, "ErrorText"},
      {HtmlTokenType::TagClose, "TagClose"},
      {HtmlTokenType::TagSlashClose, "TagSlashClose"},
      {HtmlTokenType::TagSlash, "TagSlash"},
      {HtmlTokenType::TagEquals, "TagEquals"},
      {HtmlTokenType::TagWhiteSpace, "TagWhiteSpace"},
      {HtmlTokenType::AttributeName, "AttributeName"},
      {HtmlTokenType::ErrorTag, "ErrorTag"},
      {HtmlTokenType::AttributeWhiteSpace, "AttributeWhiteSpace"},
      {HtmlTokenType::AttributeSlash, "AttributeSlash"},
      {HtmlTokenType::AttributeValue, "AttributeValue"},
      {HtmlTokenType::ErrorAttribute, "ErrorAttribute"}};

  auto search = tokenTypeToStringMap.find(static_cast<HtmlTokenType>(type));
  if (search == tokenTypeToStringMap.end()) {
    throw std::runtime_error{"Undefined Html Token type" + std::to_string(type)};
  }
  return search->second;
}

} // namespace injections
} // namespace protection
