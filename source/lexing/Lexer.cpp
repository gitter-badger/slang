//------------------------------------------------------------------------------
// Lexer.cpp
// Source file lexer.
//
// File is under the MIT license; see LICENSE for details
//------------------------------------------------------------------------------
#include "Lexer.h"

#include <algorithm>
#include <cmath>

#include "parsing/SyntaxNode.h"
#include "text/CharInfo.h"
#include "text/SourceManager.h"
#include "util/BumpAllocator.h"

namespace slang {

static const int MaxMantissaDigits = 18;

inline bool composeDouble(double fraction, int exp, double& result) {
    static const double powersOf10[] = {
        10.0,
        100.0,
        1.0e4,
        1.0e8,
        1.0e16,
        1.0e32,
        1.0e64,
        1.0e128,
        1.0e256
    };

    bool neg = false;
    if (exp < 0) {
        neg = true;
        exp = -exp;
    }

    static const int MaxExponent = 511;
    if (exp > MaxExponent)
        exp = MaxExponent;

    double dblExp = 1.0;
    for (auto d = powersOf10; exp != 0; exp >>= 1, d++) {
        if (exp & 0x1)
            dblExp *= *d;
    }

    if (neg)
        fraction /= dblExp;
    else
        fraction *= dblExp;

    result = fraction;
    return std::isfinite(result);
}

inline double computeRealValue(uint64_t value, int decPoint, int digits, uint64_t expValue, bool negative) {
    int fracExp = decPoint - std::min(digits, MaxMantissaDigits);
    int exp;
    if (negative)
        exp = fracExp - int(expValue);
    else
        exp = fracExp + int(expValue);

    double result;
    composeDouble(double(value), exp, result);

    return result;
}

SyntaxKind getDirectiveKind(string_view directive);

Lexer::Lexer(SourceBuffer buffer, BumpAllocator& alloc, Diagnostics& diagnostics, LexerOptions options) :
    Lexer(buffer.id, buffer.data, alloc, diagnostics, options)
{
}

Lexer::Lexer(BufferID bufferId, string_view source, BumpAllocator& alloc,
             Diagnostics& diagnostics, LexerOptions options) :
    alloc(alloc),
    diagnostics(diagnostics),
    options(options),
    bufferId(bufferId),
    originalBegin(source.data()),
    sourceBuffer(source.data()),
    sourceEnd(source.data() + source.length()),
    marker(nullptr)
{
    ptrdiff_t count = sourceEnd - sourceBuffer;
    ASSERT(count);
    ASSERT(sourceEnd[-1] == '\0');

    // detect BOMs so we can give nice errors for invaild encoding
    if (count >= 2) {
        const unsigned char* ubuf = reinterpret_cast<const unsigned char*>(sourceBuffer);
        if ((ubuf[0] == 0xFF && ubuf[1] == 0xFE) ||
            (ubuf[0] == 0xFE && ubuf[1] == 0xFF)) {
            addError(DiagCode::UnicodeBOM, 0);
            advance(2);
        }
        else if (count >= 3) {
            if (ubuf[0] == 0xEF &&
                ubuf[1] == 0xBB &&
                ubuf[2] == 0xBF) {
                addError(DiagCode::UnicodeBOM, 0);
                advance(3);
            }
        }
    }
}

Token Lexer::concatenateTokens(BumpAllocator& alloc, Token left, Token right) {
    auto location = left.location();
    auto trivia = left.trivia();

    // if either side is empty, we have an error; the user tried to concatenate some weird kind of token
    auto leftText = left.rawText();
    auto rightText = right.rawText();
    if (leftText.empty() || rightText.empty())
        return Token();

    // combine the text for both sides; make sure to include room for a null
    uint32_t newLength = (uint32_t)(leftText.length() + rightText.length() + 1);
    char* mem = (char*)alloc.allocate(newLength, 1);
    leftText.copy(mem, leftText.length());
    rightText.copy(mem + leftText.length(), rightText.length());
    mem[newLength - 1] = '\0';
    string_view combined { mem, newLength };

    Diagnostics unused;
    Lexer lexer { BufferID(), combined, alloc, unused, LexerOptions{} };

    auto token = lexer.lex();
    if (token.kind == TokenKind::Unknown || token.rawText().empty())
        return Token();

    // make sure the next token is an EoF, otherwise the tokens were unable to
    // be combined and should be left alone
    if (lexer.lex().kind != TokenKind::EndOfFile)
        return Token();

    auto info = alloc.emplace<Token::Info>(*token.getInfo());
    info->location = location;
    info->trivia = trivia;
    return Token(token.kind, info);
}

Token Lexer::stringify(BumpAllocator& alloc, SourceLocation location, span<Trivia const> trivia,
                       Token* begin, Token* end, bool noWhitespace) {
    SmallVectorSized<char, 64> text;
    text.append('"');

    while (begin != end) {
        Token cur = *begin;

        if (!noWhitespace) {
            for (const Trivia& t : cur.trivia()) {
                if (t.kind == TriviaKind::Whitespace)
                    text.appendRange(t.getRawText());
            }
        }

        if (cur.kind == TokenKind::MacroEscapedQuote) {
            text.append('\\');
            text.append('"');
        }
        else if (cur.kind != TokenKind::EmptyMacroArgument) {
            text.appendRange(cur.rawText());
        }
        begin++;
    }
    text.append('"');
    text.append('\0');

    string_view raw = to_string_view(text.copy(alloc));

    Diagnostics unused;
    Lexer lexer { BufferID(), raw, alloc, unused, LexerOptions{} };

    auto token = lexer.lex();
    ASSERT(token.kind == TokenKind::StringLiteral);
    ASSERT(lexer.lex().kind == TokenKind::EndOfFile);

    auto info = alloc.emplace<Token::Info>(*token.getInfo());
    info->location = location;
    info->trivia = trivia;
    info->rawText = raw.substr(0, raw.length() - 1);
    return Token(token.kind, info);
}

Token Lexer::lex(LexerMode mode, KeywordVersion keywordVersion) {
    if (mode == LexerMode::IncludeFileName)
        return lexIncludeFileName();

    auto info = alloc.emplace<Token::Info>();
    SmallVectorSized<Trivia, 32> triviaBuffer;
    bool directiveMode = mode == LexerMode::Directive;

    // Lex any leading trivia; if we're in directive mode this might require
    // us to return an EndOfDirective token right away.
    if (lexTrivia(triviaBuffer, directiveMode)) {
        info->trivia = triviaBuffer.copy(alloc);
        return Token(TokenKind::EndOfDirective, info);
    }

    // lex the next token
    mark();
    TokenKind kind = lexToken(info, directiveMode, keywordVersion);
    onNewLine = false;
    info->rawText = lexeme();

    if (kind != TokenKind::EndOfFile && diagnostics.size() > options.maxErrors) {
        // Stop any further lexing by claiming to be at the end of the buffer.
        addError(DiagCode::TooManyLexerErrors, currentOffset());
        sourceBuffer = sourceEnd - 1;
        triviaBuffer.append(Trivia(TriviaKind::DisabledText, lexeme()));
        kind = TokenKind::EndOfFile;
    }
    info->trivia = triviaBuffer.copy(alloc);
    return Token(kind, info);
}

TokenKind Lexer::lexToken(Token::Info* info, bool directiveMode, KeywordVersion keywordVersion) {
    uint32_t offset = currentOffset();
    info->location = SourceLocation(getBufferID(), offset);

    char c = peek();
    advance();
    switch (c) {
        case '\0':
            // check if we're not really at the end
            // we back up one character here so that if the user calls lex() again and again,
            // he'll just keep getting back EndOfFile tokens over and over
            sourceBuffer--;
            if (!reallyAtEnd()) {
                advance();
                addError(DiagCode::EmbeddedNull, offset);
                return TokenKind::Unknown;
            }

            // if we're lexing a directive, issue an EndOfDirective before the EndOfFile
            if (directiveMode)
                return TokenKind::EndOfDirective;

            // otherwise, end of file
            return TokenKind::EndOfFile;
        case '!':
            if (consume('=')) {
                switch (peek()) {
                    case '=': advance(); return TokenKind::ExclamationDoubleEquals;
                    case '?': advance(); return TokenKind::ExclamationEqualsQuestion;
                    default: return TokenKind::ExclamationEquals;
                }
            }
            return TokenKind::Exclamation;
        case '"':
            lexStringLiteral(info);
            return TokenKind::StringLiteral;
        case '#':
            switch (peek()) {
                case '#': advance(); return TokenKind::DoubleHash;
                case '-':
                    if (peek(1) == '#') {
                        advance(2);
                        return TokenKind::HashMinusHash;
                    }
                    // #- isn't a token, so just return a hash
                    return TokenKind::Hash;
                case '=':
                    if (peek(1) == '#') {
                        advance(2);
                        return TokenKind::HashEqualsHash;
                    }
                    // #= isn't a token, so just return a hash
                    return TokenKind::Hash;
            }
            return TokenKind::Hash;
        case '$': return lexDollarSign(info);
        case '%':
            if (consume('='))
                return TokenKind::PercentEqual;
            return TokenKind::Percent;
        case '&':
            switch (peek()) {
                case '&':
                    advance();
                    if (consume('&'))
                        return TokenKind::TripleAnd;
                    else
                        return TokenKind::DoubleAnd;
                case '=': advance(); return TokenKind::AndEqual;
            }
            return TokenKind::And;
        case '\'':
            if (consume('{'))
                return TokenKind::ApostropheOpenBrace;
            else
                return lexApostrophe(info);
        case '(':
            if (!consume('*'))
                return TokenKind::OpenParenthesis;
            else if (consume(')'))
                return TokenKind::OpenParenthesisStarCloseParenthesis;
            else
                return TokenKind::OpenParenthesisStar;
        case ')': return TokenKind::CloseParenthesis;
        case '*':
            switch (peek()) {
                case '*': advance(); return TokenKind::DoubleStar;
                case '=': advance(); return TokenKind::StarEqual;
                case '>': advance(); return TokenKind::StarArrow;
                case ')': advance(); return TokenKind::StarCloseParenthesis;
                case ':':
                    if (peek(1) == ':' && peek(2) == '*') {
                        advance(3);
                        return TokenKind::StarDoubleColonStar;
                    }
                    return TokenKind::Star;
            }
            return TokenKind::Star;
        case '+':
            switch (peek()) {
                case '+': advance(); return TokenKind::DoublePlus;
                case '=': advance(); return TokenKind::PlusEqual;
                case ':': advance(); return TokenKind::PlusColon;
            }
            return TokenKind::Plus;
        case ',': return TokenKind::Comma;
        case '-':
            switch (peek()) {
                case '-': advance(); return TokenKind::DoubleMinus;
                case '=': advance(); return TokenKind::MinusEqual;
                case ':': advance(); return TokenKind::MinusColon;
                case '>':
                    advance();
                    if (consume('>'))
                        return TokenKind::MinusDoubleArrow;
                    else
                        return TokenKind::MinusArrow;
            }
            return TokenKind::Minus;
        case '.':
            if (consume('*'))
                return TokenKind::DotStar;
            else
                return TokenKind::Dot;
        case '/':
            if (consume('='))
                return TokenKind::SlashEqual;
            else
                return TokenKind::Slash;
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7':
        case '8': case '9':
            // back up so that lexNumericLiteral can look at this digit again
            sourceBuffer--;
            return lexNumericLiteral(info);
        case ':':
            switch (peek()) {
                case '=': advance(); return TokenKind::ColonEquals;
                case '/': advance(); return TokenKind::ColonSlash;
                case ':': advance(); return TokenKind::DoubleColon;
            }
            return TokenKind::Colon;
        case ';': return TokenKind::Semicolon;
        case '<':
            switch (peek()) {
                case '=': advance(); return TokenKind::LessThanEquals;
                case '-':
                    if (peek(1) == '>') {
                        advance(2);
                        return TokenKind::LessThanMinusArrow;
                    }
                    return TokenKind::LessThan;
                case '<':
                    advance();
                    switch (peek()) {
                        case '<':
                            if (peek(1) == '=') {
                                advance(2);
                                return TokenKind::TripleLeftShiftEqual;
                            }
                            else {
                                advance();
                                return TokenKind::TripleLeftShift;
                            }
                        case '=': advance(); return TokenKind::LeftShiftEqual;
                    }
                    return TokenKind::LeftShift;
            }
            return TokenKind::LessThan;
        case '=':
            switch (peek()) {
                case '=':
                    advance();
                    switch (peek()) {
                        case '=': advance(); return TokenKind::TripleEquals;
                        case '?': advance(); return TokenKind::DoubleEqualsQuestion;
                    }
                    return TokenKind::DoubleEquals;
                case '>': advance(); return TokenKind::EqualsArrow;
            }
            return TokenKind::Equals;
        case '>':
            switch (peek()) {
                case '=': advance(); return TokenKind::GreaterThanEquals;
                case '>':
                    advance();
                    switch (peek()) {
                        case '>':
                            if (peek(1) == '=') {
                                advance(2);
                                return TokenKind::TripleRightShiftEqual;
                            }
                            else {
                                advance();
                                return TokenKind::TripleRightShift;
                            }
                        case '=': advance(); return TokenKind::RightShiftEqual;
                    }
                    return TokenKind::RightShift;
            }
            return TokenKind::GreaterThan;
        case '?': return TokenKind::Question;
        case '@':
            switch (peek()) {
                case '@': advance(); return TokenKind::DoubleAt;
                case '*': advance(); return TokenKind::AtStar;
                default: return TokenKind::At;
            }
        case 'A': case 'B': case 'C': case 'D':
        case 'E': case 'F': case 'G': case 'H':
        case 'I': case 'J': case 'L': case 'K':
        case 'M': case 'N': case 'O': case 'P':
        case 'Q': case 'R': case 'S': case 'T':
        case 'U': case 'V': case 'W': case 'X':
        case 'Y': case 'Z':
        case 'a': case 'b': case 'c': case 'd':
        case 'e': case 'f': case 'g': case 'h':
        case 'i': case 'j': case 'k': case 'l':
        case 'm': case 'n': case 'o': case 'p':
        case 'q': case 'r': case 's': case 't':
        case 'u': case 'v': case 'w': case 'x':
        case 'y': case 'z':
        case '_': {
            scanIdentifier();

            // might be a keyword
            TokenKind kind;
            if (getKeywordTable(keywordVersion)->lookup(lexeme(), kind))
                return kind;

            info->extra = IdentifierType::Normal;
            return TokenKind::Identifier;
        }
        case '[': return TokenKind::OpenBracket;
        case '\\': return lexEscapeSequence(info);
        case ']': return TokenKind::CloseBracket;
        case '^':
            switch (peek()) {
                case '~': advance(); return TokenKind::XorTilde;
                case '=': advance(); return TokenKind::XorEqual;
            }
            return TokenKind::Xor;
        case '`':
            switch (peek()) {
                case '"': advance(); return TokenKind::MacroQuote;
                case '`': advance(); return TokenKind::MacroPaste;
                case '\\':
                    if (peek(1) == '`' && peek(2) == '"') {
                        advance(3);
                        return TokenKind::MacroEscapedQuote;
                    }
                    return lexDirective(info);
            }
            return lexDirective(info);
        case '{': return TokenKind::OpenBrace;
        case '|':
            switch (peek()) {
                case '|': advance(); return TokenKind::DoubleOr;
                case '-':
                    if (peek(1) == '>') {
                        advance(2);
                        if (consume('>'))
                            return TokenKind::OrMinusDoubleArrow;
                        else
                            return TokenKind::OrMinusArrow;
                    }
                    return TokenKind::Or;
                case '=':
                    if (peek(1) == '>') {
                        advance(2);
                        return TokenKind::OrEqualsArrow;
                    }
                    else {
                        advance();
                        return TokenKind::OrEqual;
                    }
            }
            return TokenKind::Or;
        case '}': return TokenKind::CloseBrace;
        case '~':
            switch (peek()) {
                case '&': advance(); return TokenKind::TildeAnd;
                case '|': advance(); return TokenKind::TildeOr;
                case '^': advance(); return TokenKind::TildeXor;
            }
            return TokenKind::Tilde;
        default:
            if (isASCII(c))
                addError(DiagCode::NonPrintableChar, offset);
            else {
                // skip over UTF-8 sequences
                advance(utf8SeqBytes(c));
                addError(DiagCode::UTF8Char, offset);
            }
            return TokenKind::Unknown;
    }
}

void Lexer::lexStringLiteral(Token::Info* info) {
    SmallVectorSized<char, 128> stringBuffer;
    while (true) {
        uint32_t offset = currentOffset();
        char c = peek();

        if (c == '\\') {
            advance();
            c = peek();
            advance();

            uint32_t charCode;
            switch (c) {
                case 'n': stringBuffer.append('\n'); break;
                case 't': stringBuffer.append('\t'); break;
                case '\\': stringBuffer.append('\\'); break;
                case '"': stringBuffer.append('"'); break;
                case 'v': stringBuffer.append('\v'); break;
                case 'f': stringBuffer.append('\f'); break;
                case 'a': stringBuffer.append('\a'); break;
                case '\n': break;
                case '\r': consume('\n'); break;
                case '0': case '1': case '2': case '3':
                case '4': case '5': case '6': case '7':
                    // octal character code
                    charCode = getDigitValue(c);
                    if (isOctalDigit(c = peek())) {
                        advance();
                        charCode = (charCode * 8) + getDigitValue(c);
                        if (isOctalDigit(c = peek())) {
                            advance();
                            charCode = (charCode * 8) + getDigitValue(c);
                            if (charCode > 255) {
                                addError(DiagCode::OctalEscapeCodeTooBig, offset);
                                break;
                            }
                        }
                    }
                    stringBuffer.append((char)charCode);
                    break;
                case 'x':
                    c = peek();
                    advance();
                    if (!isHexDigit(c)) {
                        addError(DiagCode::InvalidHexEscapeCode, offset);
                        stringBuffer.append(c);
                    }
                    else {
                        charCode = getHexDigitValue(c);
                        if (isHexDigit(c = peek())) {
                            advance();
                            charCode = (charCode * 16) + getHexDigitValue(c);
                        }
                        stringBuffer.append((char)charCode);
                    }
                    break;
                default:
                    addError(DiagCode::UnknownEscapeCode, offset);
                    stringBuffer.append(c);
                    break;
            }
        }
        else if (c == '"') {
            advance();
            break;
        }
        else if (isNewline(c)) {
            addError(DiagCode::ExpectedClosingQuote, offset);
            break;
        }
        else if (c == '\0') {
            if (reallyAtEnd()) {
                addError(DiagCode::ExpectedClosingQuote, offset);
                break;
            }

            // otherwise just error and ignore
            addError(DiagCode::EmbeddedNull, offset);
            advance();
        }
        else {
            advance();
            stringBuffer.append(c);
        }
    }

    info->extra = to_string_view(stringBuffer.copy(alloc));
}

TokenKind Lexer::lexEscapeSequence(Token::Info* info) {
    char c = peek();
    if (isWhitespace(c) || c == '\0') {
        addError(DiagCode::EscapedWhitespace, currentOffset());
        return TokenKind::Unknown;
    }

    while (isPrintable(c)) {
        advance();
        c = peek();
        if (isWhitespace(c))
            break;
    }

    info->extra = IdentifierType::Escaped;
    return TokenKind::Identifier;
}

TokenKind Lexer::lexDollarSign(Token::Info* info) {
    scanIdentifier();

    // if length is 1, we just have a dollar sign operator
    if (lexemeLength() == 1)
        return TokenKind::Dollar;

    // otherwise, we have a system identifier
    // check for system keywords
    TokenKind kind = getSystemKeywordKind(lexeme());
    if (kind != TokenKind::Unknown)
        return kind;

    info->extra = IdentifierType::System;
    return TokenKind::Identifier;
}

TokenKind Lexer::lexDirective(Token::Info* info) {
    // store the offset before scanning in order to easily report error locations
    uint32_t startingOffset = currentOffset();
    scanIdentifier();

    // if length is 1, we just have a grave character on its own, which is an error
    if (lexemeLength() == 1) {
        addError(DiagCode::MisplacedDirectiveChar, startingOffset);
        info->extra = SyntaxKind::Unknown;
        return TokenKind::Directive;
    }

    info->extra = getDirectiveKind(lexeme().substr(1));
    if (!onNewLine && std::get<SyntaxKind>(info->extra) == SyntaxKind::IncludeDirective)
        addError(DiagCode::IncludeNotFirstOnLine, startingOffset);

    return TokenKind::Directive;
}

Token Lexer::lexIncludeFileName() {
    // leading whitespace should lex into trivia
    SmallVectorSized<Trivia, 8> triviaBuffer;
    if (isHorizontalWhitespace(peek())) {
        mark();
        scanWhitespace(triviaBuffer);
    }

    span<Trivia const> trivia = triviaBuffer.copy(alloc);
    uint32_t offset = currentOffset();
    auto location = SourceLocation(getBufferID(), offset);

    mark();
    char delim = peek();
    if (delim == '`') {
        advance();
        // macro file name
        auto info = alloc.emplace<Token::Info>();
        auto token = Token(lexDirective(info), info);
        info->trivia = trivia;
        info->rawText = lexeme();
        info->location = location;
        return token;
    }
    else if (delim != '"' && delim != '<') {
        addError(DiagCode::ExpectedIncludeFileName, offset);
        return Token(TokenKind::IncludeFileName, alloc.emplace<Token::Info>(trivia, "", location, TokenFlags::Missing));
    }
    else if (delim == '<') {
        delim = '>';
    }

    advance();
    char c;
    do {
        c = peek();
        if (c == '\0' || isNewline(c)) {
            addError(DiagCode::ExpectedIncludeFileName, offset);
            break;
        }
        advance();
    } while (c != delim);

    string_view rawText = lexeme();
    auto info = alloc.emplace<Token::Info>(trivia, rawText, location, TokenFlags::None);
    info->extra = rawText;

    return Token(TokenKind::IncludeFileName, info);
}

TokenKind Lexer::lexNumericLiteral(Token::Info* info) {
    // have to check for the "1step" magic keyword
    static const char OneStepText[] = "1step";
    for (int i = 0; i < (int)sizeof(OneStepText) - 1; i++) {
        if (peek(i) != OneStepText[i])
            break;
        if (i == sizeof(OneStepText) - 2) {
            advance(sizeof(OneStepText) - 1);
            return TokenKind::OneStep;
        }
    }

    // scan past leading zeros
    while (peek() == '0')
        advance();

    // scan past decimal digits; we know we have at least one if we got here
    uint64_t value = 0;
    int digits = 0;
    scanUnsignedNumber(value, digits);

    // check if we have a fractional number here
    switch (peek()) {
        case '.': {
            // fractional digits
            int decPoint = digits;
            advance();
            if (!isDecimalDigit(peek()))
                addError(DiagCode::MissingFractionalDigits, currentOffset());
            scanUnsignedNumber(value, digits);

            TokenKind result = TokenKind::RealLiteral;
            uint64_t exp = 0;
            bool neg = false;

            char c = peek();
            if (c == 'e' || c == 'E') {
                uint32_t startOfExponent = currentOffset() + 1;
                if (!scanExponent(exp, neg))
                    addError(DiagCode::MissingExponentDigits, startOfExponent);
            }
            else if (lexTimeLiteral(info))
                result = TokenKind::TimeLiteral;

            info->setReal(computeRealValue(value, decPoint, digits, exp, neg));
            return result;
        }
        case 'e':
        case 'E': {
            // Check if this is an exponent or just something like a hex digit.
            // We disambiguate by always choosing a real if possible; someone
            // downstream might need to fix it up later.
            uint64_t exp;
            bool neg;
            if (scanExponent(exp, neg)) {
                info->setReal(computeRealValue(value, digits, digits, exp, neg));
                return TokenKind::RealLiteral;
            }
            break;
        }
    }

    if (lexTimeLiteral(info)) {
        // TODO: overflow?
        info->setReal((double)value);
        return TokenKind::TimeLiteral;
    }

    // normal signed numeric literal
    info->setInt(alloc, SVInt(32, value, true));
    return TokenKind::IntegerLiteral;
}

bool Lexer::scanExponent(uint64_t& value, bool& negative) {
    value = 0;
    negative = false;

    // skip over leading sign
    int index = 1;
    char c = peek(index);
    if (c == '+' || c == '-') {
        negative = c == '-';
        index++;
        c = peek(index);
    }

    // need at least one decimal digit
    if (!isDecimalDigit(c))
        return false;

    // otherwise, we have a real exponent, so skip remaining digits
    int unused = 0;
    advance(index);
    scanUnsignedNumber(value, unused);
    return true;
}

TokenKind Lexer::lexApostrophe(Token::Info* info) {
    char c = peek();
    switch (c) {
        case '0':
        case '1':
            advance();
            info->setBit((logic_t)getDigitValue(c));
            return TokenKind::UnbasedUnsizedLiteral;
        case 'x':
        case 'X':
            advance();
            info->setBit(logic_t::x);
            return TokenKind::UnbasedUnsizedLiteral;
        case 'Z':
        case 'z':
        case '?':
            advance();
            info->setBit(logic_t::z);
            return TokenKind::UnbasedUnsizedLiteral;

        case 's':
        case 'S':
            advance();
            if (!lexIntegerBase(info, true))
                addError(DiagCode::ExpectedIntegerBaseAfterSigned, currentOffset());

            return TokenKind::IntegerBase;

        default:
            if (lexIntegerBase(info, false))
                return TokenKind::IntegerBase;

            // otherwise just an apostrophe token
            return TokenKind::Apostrophe;
    }
}

bool Lexer::lexIntegerBase(Token::Info* info, bool isSigned) {
    LiteralBase base;
    if (literalBaseFromChar(peek(), base)) {
        advance();
        info->setNumFlags(base, isSigned);
        return true;
    }
    return false;
}

bool Lexer::lexTimeLiteral(Token::Info* info) {
#define CASE(c, flag) \
    case c: if (peek(1) == 's') { \
        advance(2); \
        info->setTimeUnit(TimeUnit::flag); \
        return true; \
    } break;

    switch (peek()) {
        case 's':
            advance();
            info->setTimeUnit(TimeUnit::Seconds);
            return true;
        CASE('m', Milliseconds);
        CASE('u', Microseconds);
        CASE('n', Nanoseconds);
        CASE('p', Picoseconds);
        CASE('f', Femtoseconds);
        default:
            break;
    }
#undef CASE
    return false;
}

bool Lexer::lexTrivia(SmallVector<Trivia>& triviaBuffer, bool directiveMode) {
    while (true) {
        mark();

        switch (peek()) {
            case ' ':
            case '\t':
            case '\v':
            case '\f':
                advance();
                scanWhitespace(triviaBuffer);
                break;
            case '/':
                switch (peek(1)) {
                    case '/':
                        advance(2);
                        scanLineComment(triviaBuffer, directiveMode);
                        break;
                    case '*': {
                        advance(2);
                        if (scanBlockComment(triviaBuffer, directiveMode))
                            return true;
                        break;
                    }
                    default:
                        return false;
                }
                break;
            case '\r':
                advance();
                consume('\n');
                onNewLine = true;
                addTrivia(TriviaKind::EndOfLine, triviaBuffer);
                if (directiveMode)
                    return true;
                break;
            case '\n':
                advance();
                onNewLine = true;
                addTrivia(TriviaKind::EndOfLine, triviaBuffer);
                if (directiveMode)
                    return true;
                break;
            case '\\': {
                // if we're lexing a directive, this might escape a newline
                char n = peek(1);
                if (!directiveMode || !isNewline(n))
                    return false;

                advance(2);
                if (n == '\r')
                    consume('\n');

                onNewLine = true;
                addTrivia(TriviaKind::LineContinuation, triviaBuffer);
                break;
            }
            case '\0':
                // in directive mode, return an EOD first to wrap up any directive processing
                return directiveMode;
            default:
                return false;
        }
    }
}

void Lexer::scanIdentifier() {
    while (true) {
        char c = peek();
        if (isAlphaNumeric(c) || c == '_' || c == '$')
            advance();
        else
            return;
    }
}

void Lexer::scanUnsignedNumber(uint64_t& value, int& digits) {
    while (true) {
        char c = peek();
        if (c == '_')
            advance();
        else if (!isDecimalDigit(c))
            return;
        else {
            // After 18 digits stop caring. For normal integers we're going to truncate
            // to 32-bits anyway. For reals, later digits won't have any effect on the result.
            if (digits < MaxMantissaDigits)
                value = (value * 10) + getDigitValue(c);
            digits++;
            advance();
        }
    }
}

void Lexer::scanWhitespace(SmallVector<Trivia>& triviaBuffer) {
    bool done = false;
    while (!done) {
        switch (peek()) {
            case ' ':
            case '\t':
            case '\v':
            case '\f':
                advance();
                break;
            default:
                done = true;
                break;
        }
    }
    addTrivia(TriviaKind::Whitespace, triviaBuffer);
}

void Lexer::scanLineComment(SmallVector<Trivia>& triviaBuffer, bool directiveMode) {
    while (true) {
        char c = peek();
        if (isNewline(c))
            break;

        // If we're in a directive, we want the continuation character to continue
        // the directive on to the next line, so don't consume it as part of the comment.
        if (c == '\\' && directiveMode) {
            if (isNewline(peek(1)))
                break;
        }

        if (c == '\0') {
            if (reallyAtEnd())
                break;

            // otherwise just error and ignore
            addError(DiagCode::EmbeddedNull, currentOffset());
        }
        advance();
    }
    addTrivia(TriviaKind::LineComment, triviaBuffer);
}

bool Lexer::scanBlockComment(SmallVector<Trivia>& triviaBuffer, bool directiveMode) {
    bool eod = false;
    while (true) {
        char c = peek();
        if (c == '\0') {
            if (reallyAtEnd()) {
                addError(DiagCode::UnterminatedBlockComment, currentOffset());
                break;
            }

            // otherwise just error and ignore
            addError(DiagCode::EmbeddedNull, currentOffset());
            advance();
        }
        else if (c == '*' && peek(1) == '/') {
            advance(2);
            break;
        }
        else if (c == '/' && peek(1) == '*') {
            // nested block comments disallowed by the standard; ignore and continue
            addError(DiagCode::NestedBlockComment, currentOffset());
            advance(2);
        }
        else {
            if (directiveMode && isNewline(c)) {
                // found a newline in a block comment inside a directive; this is not allowed
                // we need to stop lexing trivia and issue an EndOfDirective token after this comment
                addError(DiagCode::SplitBlockCommentInDirective, currentOffset());
                eod = true;
            }
            advance();
        }
    }

    addTrivia(TriviaKind::BlockComment, triviaBuffer);
    return eod;
}

void Lexer::addTrivia(TriviaKind kind, SmallVector<Trivia>& triviaBuffer) {
    triviaBuffer.emplace(kind, lexeme());
}

void Lexer::addError(DiagCode code, uint32_t offset) {
    diagnostics.emplace(code, SourceLocation(getBufferID(), offset));
    errorCount++;
}

uint32_t Lexer::currentOffset() {
    return (uint32_t)(sourceBuffer - originalBegin);
}

BufferID Lexer::getBufferID() const {
    return bufferId;
}

}
