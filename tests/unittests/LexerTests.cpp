#include "Test.h"

TEST_CASE("Invalid chars", "[lexer]") {
    auto& text = "\x04";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::Unknown);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    REQUIRE(!diagnostics.empty());
    CHECK(diagnostics.back().code == DiagCode::NonPrintableChar);
}

TEST_CASE("UTF8 chars", "[lexer]") {
    auto& text = u8"\U0001f34c";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::Unknown);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    REQUIRE(!diagnostics.empty());
    CHECK(diagnostics.back().code == DiagCode::UTF8Char);
}

TEST_CASE("Unicode BOMs", "[lexer]") {
    lexToken("\xEF\xBB\xBF ");
    REQUIRE(!diagnostics.empty());
    CHECK(diagnostics.back().code == DiagCode::UnicodeBOM);

    lexToken("\xFE\xFF ");
    REQUIRE(!diagnostics.empty());
    CHECK(diagnostics.back().code == DiagCode::UnicodeBOM);

    lexToken("\xFF\xFE ");
    REQUIRE(!diagnostics.empty());
    CHECK(diagnostics.back().code == DiagCode::UnicodeBOM);
}

TEST_CASE("Embedded null", "[lexer]") {
    const char text[] = "\0\0";
    auto str = std::string(text, text + sizeof(text) - 1);
    Token token = lexToken(string_view(str));

    CHECK(token.kind == TokenKind::Unknown);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == str.substr(0, str.length() - 1));
    REQUIRE(!diagnostics.empty());
    CHECK(diagnostics.back().code == DiagCode::EmbeddedNull);
}

TEST_CASE("Line Comment", "[lexer]") {
    auto& text = "// comment";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::EndOfFile);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(token.trivia().size() == 1);
    CHECK(token.trivia()[0].kind == TriviaKind::LineComment);
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("Line Comment (directive continuation)", "[lexer]") {
    auto& text = "`define FOO // comment\\\n  bar";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::EndOfFile);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia | SyntaxToStringFlags::IncludeDirectives) == text);
    CHECK(token.trivia().size() == 1);
    CHECK(token.trivia()[0].kind == TriviaKind::Directive);
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("Line Comment (embedded null)", "[lexer]") {
    const char text[] = "// foo \0 bar";
    auto str = std::string(text, text + sizeof(text) - 1);
    Token token = lexToken(string_view(str));

    CHECK(token.kind == TokenKind::EndOfFile);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == str);
    CHECK(token.trivia().size() == 1);
    CHECK(token.trivia()[0].kind == TriviaKind::LineComment);
    REQUIRE(!diagnostics.empty());
    CHECK(diagnostics.back().code == DiagCode::EmbeddedNull);
}

TEST_CASE("Block Comment (one line)", "[lexer]") {
    auto& text = "/* comment */";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::EndOfFile);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(token.trivia().size() == 1);
    CHECK(token.trivia()[0].kind == TriviaKind::BlockComment);
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("Block Comment (multiple lines)", "[lexer]") {
    auto& text =
        R"(/*
comment on
multiple lines
*/)";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::EndOfFile);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(token.trivia().size() == 1);
    CHECK(token.trivia()[0].kind == TriviaKind::BlockComment);
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("Block Comment (unterminated)", "[lexer]") {
    auto& text = "/* comment";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::EndOfFile);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(token.trivia().size() == 1);
    CHECK(token.trivia()[0].kind == TriviaKind::BlockComment);
    REQUIRE(!diagnostics.empty());
    CHECK(diagnostics.back().code == DiagCode::UnterminatedBlockComment);
}

TEST_CASE("Block comment (embedded null)", "[lexer]") {
    const char text[] = "/* foo\0 */";
    auto str = std::string(text, text + sizeof(text) - 1);
    Token token = lexToken(string_view(str));

    CHECK(token.kind == TokenKind::EndOfFile);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == str);
    CHECK(token.trivia().size() == 1);
    CHECK(token.trivia()[0].kind == TriviaKind::BlockComment);
    REQUIRE(!diagnostics.empty());
    CHECK(diagnostics.back().code == DiagCode::EmbeddedNull);
}

TEST_CASE("Block comment (directive with newline)", "[lexer]") {
    auto& text = "`resetall /* comment\n asdf */";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::EndOfFile);
    CHECK(token.trivia().size() == 1);
    CHECK(token.trivia()[0].kind == TriviaKind::Directive);
    REQUIRE(!diagnostics.empty());
    CHECK(diagnostics.back().code == DiagCode::SplitBlockCommentInDirective);
}

TEST_CASE("Block Comment (nested)", "[lexer]") {
    auto& text = "/* comment /* stuff */";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::EndOfFile);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(token.trivia().size() == 1);
    CHECK(token.trivia()[0].kind == TriviaKind::BlockComment);
    REQUIRE(!diagnostics.empty());
    CHECK(diagnostics.back().code == DiagCode::NestedBlockComment);
}

TEST_CASE("Whitespace", "[lexer]") {
    auto& text = " \t\v\f token";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::Identifier);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(token.trivia().size() == 1);
    CHECK(token.trivia()[0].kind == TriviaKind::Whitespace);
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("Newlines (CR)", "[lexer]") {
    auto& text = "\r";
    Token token = lexToken(text);
    CHECK(token.kind == TokenKind::EndOfFile);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(token.trivia().size() == 1);
    CHECK(token.trivia()[0].kind == TriviaKind::EndOfLine);
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("Newlines (CR/LF)", "[lexer]") {
    auto& text = "\r\n";
    Token token = lexToken(text);
    CHECK(token.kind == TokenKind::EndOfFile);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(token.trivia().size() == 1);
    CHECK(token.trivia()[0].kind == TriviaKind::EndOfLine);
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("Newlines (LF)", "[lexer]") {
    auto& text = "\n";
    Token token = lexToken(text);
    CHECK(token.kind == TokenKind::EndOfFile);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(token.trivia().size() == 1);
    CHECK(token.trivia()[0].kind == TriviaKind::EndOfLine);
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("Simple Identifiers", "[lexer]") {
    auto& text = "abc";
    Token token = lexToken(text);
    CHECK(token.kind == TokenKind::Identifier);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(token.valueText() == text);
    CHECK(token.identifierType() == IdentifierType::Normal);
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("Mixed Identifiers", "[lexer]") {
    auto& text = "a92837asdf358";
    Token token = lexToken(text);
    CHECK(token.kind == TokenKind::Identifier);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(token.valueText() == text);
    CHECK(token.identifierType() == IdentifierType::Normal);
    CHECK_DIAGNOSTICS_EMPTY;

    auto& text2 = "__a$$asdf213$";
    Token token2 = lexToken(text2);
    CHECK(token2.kind == TokenKind::Identifier);
    CHECK(token2.toString(SyntaxToStringFlags::IncludeTrivia) == text2);
    CHECK(token2.valueText() == text2);
    CHECK(token2.identifierType() == IdentifierType::Normal);
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("Escaped Identifiers", "[lexer]") {
    auto& text = "\\98\\#$%)(*lkjsd__09...asdf345";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::Identifier);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(token.valueText() == "98\\#$%)(*lkjsd__09...asdf345");
    CHECK(token.identifierType() == IdentifierType::Escaped);
    CHECK_DIAGNOSTICS_EMPTY;

    auto& text2 = "\\98\\#$%)(*lkjsd__09...a sdf345";
    token = lexToken(text2);

    CHECK(token.kind == TokenKind::Identifier);
    CHECK(token.valueText() == "98\\#$%)(*lkjsd__09...a");
    CHECK(token.identifierType() == IdentifierType::Escaped);
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("System Identifiers", "[lexer]") {
    auto& text = "$hello";
    Token token = lexToken(text);
    CHECK(token.kind == TokenKind::Identifier);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(token.valueText() == text);
    CHECK(token.identifierType() == IdentifierType::System);
    CHECK_DIAGNOSTICS_EMPTY;

    auto& text2 = "$45__hello";
    Token token2 = lexToken(text2);
    CHECK(token2.kind == TokenKind::Identifier);
    CHECK(token2.toString(SyntaxToStringFlags::IncludeTrivia) == text2);
    CHECK(token2.valueText() == text2);
    CHECK(token2.identifierType() == IdentifierType::System);
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("Invalid escapes", "[lexer]") {
    auto& text = "\\";
    Token token = lexToken(text);
    CHECK(token.kind == TokenKind::Unknown);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    REQUIRE(!diagnostics.empty());
    CHECK(diagnostics.back().code == DiagCode::EscapedWhitespace);

    Token token2 = lexToken("\\  ");
    CHECK(token2.kind == TokenKind::Unknown);
    CHECK(token2.toString() == "\\");
    REQUIRE(!diagnostics.empty());
    CHECK(diagnostics.back().code == DiagCode::EscapedWhitespace);
}

TEST_CASE("String literal", "[lexer]") {
    auto& text = "\"literal  #@$asdf\"";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::StringLiteral);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(token.valueText() == "literal  #@$asdf");
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("String literal (newline)", "[lexer]") {
    auto& text = "\"literal\r\nwith new line\"";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::StringLiteral);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) != text);
    CHECK(token.valueText() == "literal");

    REQUIRE(!diagnostics.empty());
    CHECK(diagnostics.back().code == DiagCode::ExpectedClosingQuote);
}

TEST_CASE("String literal (escaped newline)", "[lexer]") {
    auto& text = "\"literal\\\r\nwith new line\"";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::StringLiteral);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(token.valueText() == "literalwith new line");
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("String literal (unterminated)", "[lexer]") {
    auto& text = "\"literal";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::StringLiteral);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(token.valueText() == "literal");

    REQUIRE(!diagnostics.empty());
    CHECK(diagnostics.back().code == DiagCode::ExpectedClosingQuote);
}

TEST_CASE("String literal (escapes)", "[lexer]") {
    auto& text = "\"literal\\n\\t\\v\\f\\a \\\\ \\\" \"";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::StringLiteral);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(token.valueText() == "literal\n\t\v\f\a \\ \" ");
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("String literal (octal escape)", "[lexer]") {
    auto& text = "\"literal\\377\"";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::StringLiteral);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(token.valueText() == "literal\377");
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("String literal (bad octal escape)", "[lexer]") {
    auto& text = "\"literal\\400\"";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::StringLiteral);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(token.valueText() == "literal");
    REQUIRE(!diagnostics.empty());
    CHECK(diagnostics.back().code == DiagCode::OctalEscapeCodeTooBig);
}

TEST_CASE("String literal with hex escape", "[lexer]") {
    auto& text = "\"literal\\xFa\"";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::StringLiteral);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(token.valueText() == "literal\xFa");
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("String literal (bad hex escape)", "[lexer]") {
    auto& text = "\"literal\\xz\"";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::StringLiteral);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(token.valueText() == "literalz");
    REQUIRE(!diagnostics.empty());
    CHECK(diagnostics.back().code == DiagCode::InvalidHexEscapeCode);
}

TEST_CASE("String literal (unknown escape)", "[lexer]") {
    auto& text = "\"literal\\i\"";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::StringLiteral);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(token.valueText() == "literali");
    REQUIRE(!diagnostics.empty());
    CHECK(diagnostics.back().code == DiagCode::UnknownEscapeCode);
}

TEST_CASE("Integer literal", "[lexer]") {
    auto& text = "19248";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::IntegerLiteral);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(token.intValue() == 19248);
    CHECK_DIAGNOSTICS_EMPTY;
}

void checkVectorBase(const std::string& s, LiteralBase base, bool isSigned) {
    Token token = lexToken(string_view(s));

    CHECK(token.kind == TokenKind::IntegerBase);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == s);
    CHECK(token.numericFlags().base() == base);
    CHECK(token.numericFlags().isSigned() == isSigned);
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("Vector bases", "[lexer]") {
    checkVectorBase("'d", LiteralBase::Decimal, false);
    checkVectorBase("'sD", LiteralBase::Decimal, true);
    checkVectorBase("'Sb", LiteralBase::Binary, true);
    checkVectorBase("'B", LiteralBase::Binary, false);
    checkVectorBase("'so", LiteralBase::Octal, true);
    checkVectorBase("'O", LiteralBase::Octal, false);
    checkVectorBase("'h", LiteralBase::Hex, false);
    checkVectorBase("'SH", LiteralBase::Hex, true);
}

TEST_CASE("Vector base (bad)", "[lexer]") {
    Token token = lexToken(string_view("'sf"));

    CHECK(token.kind == TokenKind::IntegerBase);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == "'s");
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics.back().code == DiagCode::ExpectedIntegerBaseAfterSigned);
}

TEST_CASE("Unbased unsized literal", "[lexer]") {
    auto& text = "'1";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::UnbasedUnsizedLiteral);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(token.bitValue().value == 1);
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("Real literal (fraction)", "[lexer]") {
    auto& text = "32.57";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::RealLiteral);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(withinUlp(token.realValue(), 32.57));
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("Real literal (missing fraction)", "[lexer]") {
    auto& text = "32.";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::RealLiteral);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    REQUIRE(!diagnostics.empty());
    CHECK(diagnostics.back().code == DiagCode::MissingFractionalDigits);
    CHECK(token.realValue() == 32);
}

TEST_CASE("Real literal (exponent)", "[lexer]") {
    auto& text = "32e57";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::RealLiteral);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(withinUlp(token.realValue(), 32e57));
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("Real literal (plus exponent)", "[lexer]") {
    auto& text = "0000032E+000__57";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::RealLiteral);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(withinUlp(token.realValue(), 32e57));
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("Real literal (minus exponent)", "[lexer]") {
    auto& text = "3_2e-5__7";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::RealLiteral);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(withinUlp(token.realValue(), 32e-57));
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("Real literal (fraction exponent)", "[lexer]") {
    auto& text = "32.3456e57";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::RealLiteral);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(withinUlp(token.realValue(), 32.3456e57));
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("Real literal (exponent overflow)", "[lexer]") {
    auto& text = "32e9000";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::RealLiteral);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(std::isinf(token.realValue()));
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("Real literal (bad exponent)", "[lexer]") {
    auto& text = "32.234e";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::RealLiteral);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == "32.234");
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics.back().code == DiagCode::MissingExponentDigits);
}

TEST_CASE("Real literal (digit overflow)", "[lexer]") {
    std::string text = std::string(400, '9') + ".0";
    Token token = lexToken(string_view(text));

    CHECK(token.kind == TokenKind::RealLiteral);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK_DIAGNOSTICS_EMPTY;

    CHECK(std::isinf(token.realValue()));
}

TEST_CASE("Integer literal (not an exponent)", "[lexer]") {
    auto& text = "32e_9";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::IntegerLiteral);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == "32");
    CHECK_DIAGNOSTICS_EMPTY;
}

void checkTimeLiteral(const std::string& s, TimeUnit flagCheck, double num) {
    Token token = lexToken(string_view(s));

    CHECK(token.kind == TokenKind::TimeLiteral);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == s);
    CHECK(token.numericFlags().unit() == flagCheck);
    CHECK(token.realValue() == num);
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("Time literals", "[lexer]") {
    checkTimeLiteral("3.4s", TimeUnit::Seconds, 3.4);
    checkTimeLiteral("9999ms", TimeUnit::Milliseconds, 9999);
    checkTimeLiteral("572.234us", TimeUnit::Microseconds, 572.234);
    checkTimeLiteral("97ns", TimeUnit::Nanoseconds, 97);
    checkTimeLiteral("42ps", TimeUnit::Picoseconds, 42);
    checkTimeLiteral("42fs", TimeUnit::Femtoseconds, 42);
}

TEST_CASE("Bad time literal", "[lexer]") {
    Token token = lexToken("10mX");
    CHECK(token.kind != TokenKind::TimeLiteral);
}

TEST_CASE("Misplaced directive char", "[lexer]") {
    auto& text = "`";
    Token token = lexRawToken(text);

    CHECK(token.kind == TokenKind::Directive);
    CHECK(token.directiveKind() == SyntaxKind::Unknown);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    REQUIRE(!diagnostics.empty());
    CHECK(diagnostics.back().code == DiagCode::MisplacedDirectiveChar);
}

TEST_CASE("Directive continuation", "[lexer]") {
    auto& text = "`define FOO asdf\\\nbar\\\r\nbaz";
    Token token = lexToken(text);

    CHECK(token.kind == TokenKind::EndOfFile);
    REQUIRE(token.trivia().size() == 1);

    Trivia t = token.trivia()[0];
    CHECK(t.kind == TriviaKind::Directive);
    REQUIRE(t.syntax()->kind == SyntaxKind::DefineDirective);

    const DefineDirectiveSyntax& define = t.syntax()->as<DefineDirectiveSyntax>();
    REQUIRE(define.body.count() == 3);
    CHECK(define.body[2].valueText() == "baz");

    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("Directive not on own line", "[lexer]") {
    auto& text = "foo `include <sdf.svh>";

    diagnostics.clear();
    auto buffer = getSourceManager().assignText(string_view(text));
    Lexer lexer(buffer, alloc, diagnostics);

    Token token = lexer.lex();
    CHECK(token.kind == TokenKind::Identifier);
    token = lexer.lex();
    CHECK(token.kind == TokenKind::Directive);

    REQUIRE(!diagnostics.empty());
    CHECK(diagnostics.back().code == DiagCode::IncludeNotFirstOnLine);
}

TEST_CASE("Escaped keyword identifiers", "[lexer]") {
    auto& text = "\\wire";

    auto token = lexToken(text);
    CHECK(token.kind == TokenKind::Identifier);
    CHECK(token.valueText() == "wire");
    CHECK(token.identifierType() == IdentifierType::Escaped);
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("Too many errors", "[lexer]") {
    std::vector<char> buf;
    for (int i = 0; i < 10; i++)
        buf.push_back('\x01');

    LexerOptions options;
    options.maxErrors = 9;

    diagnostics.clear();
    auto buffer = getSourceManager().assignText(string_view(buf.data(), buf.size()));
    Lexer lexer(buffer, alloc, diagnostics, options);

    for (size_t i = 0; i < buf.size() - 1; i++)
        CHECK(lexer.lex().kind == TokenKind::Unknown);

    CHECK(diagnostics.size() == buf.size() - 1);
    CHECK(lexer.lex().kind == TokenKind::EndOfFile);
    CHECK(diagnostics.back().code == DiagCode::TooManyLexerErrors);
}

void testKeyword(TokenKind kind) {
    auto text = getTokenKindText(kind);
    Token token = lexToken(text);

    CHECK(token.kind == kind);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(token.valueText() == text);
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("All Keywords", "[preprocessor]") {
    testKeyword(TokenKind::OneStep);
    testKeyword(TokenKind::AcceptOnKeyword);
    testKeyword(TokenKind::AliasKeyword);
    testKeyword(TokenKind::AlwaysKeyword);
    testKeyword(TokenKind::AlwaysCombKeyword);
    testKeyword(TokenKind::AlwaysFFKeyword);
    testKeyword(TokenKind::AlwaysLatchKeyword);
    testKeyword(TokenKind::AndKeyword);
    testKeyword(TokenKind::AssertKeyword);
    testKeyword(TokenKind::AssignKeyword);
    testKeyword(TokenKind::AssumeKeyword);
    testKeyword(TokenKind::AutomaticKeyword);
    testKeyword(TokenKind::BeforeKeyword);
    testKeyword(TokenKind::BeginKeyword);
    testKeyword(TokenKind::BindKeyword);
    testKeyword(TokenKind::BinsKeyword);
    testKeyword(TokenKind::BinsOfKeyword);
    testKeyword(TokenKind::BitKeyword);
    testKeyword(TokenKind::BreakKeyword);
    testKeyword(TokenKind::BufKeyword);
    testKeyword(TokenKind::BufIf0Keyword);
    testKeyword(TokenKind::BufIf1Keyword);
    testKeyword(TokenKind::ByteKeyword);
    testKeyword(TokenKind::CaseKeyword);
    testKeyword(TokenKind::CaseXKeyword);
    testKeyword(TokenKind::CaseZKeyword);
    testKeyword(TokenKind::CellKeyword);
    testKeyword(TokenKind::CHandleKeyword);
    testKeyword(TokenKind::CheckerKeyword);
    testKeyword(TokenKind::ClassKeyword);
    testKeyword(TokenKind::ClockingKeyword);
    testKeyword(TokenKind::CmosKeyword);
    testKeyword(TokenKind::ConfigKeyword);
    testKeyword(TokenKind::ConstKeyword);
    testKeyword(TokenKind::ConstraintKeyword);
    testKeyword(TokenKind::ContextKeyword);
    testKeyword(TokenKind::ContinueKeyword);
    testKeyword(TokenKind::CoverKeyword);
    testKeyword(TokenKind::CoverGroupKeyword);
    testKeyword(TokenKind::CoverPointKeyword);
    testKeyword(TokenKind::CrossKeyword);
    testKeyword(TokenKind::DeassignKeyword);
    testKeyword(TokenKind::DefaultKeyword);
    testKeyword(TokenKind::DefParamKeyword);
    testKeyword(TokenKind::DesignKeyword);
    testKeyword(TokenKind::DisableKeyword);
    testKeyword(TokenKind::DistKeyword);
    testKeyword(TokenKind::DoKeyword);
    testKeyword(TokenKind::EdgeKeyword);
    testKeyword(TokenKind::ElseKeyword);
    testKeyword(TokenKind::EndKeyword);
    testKeyword(TokenKind::EndCaseKeyword);
    testKeyword(TokenKind::EndCheckerKeyword);
    testKeyword(TokenKind::EndClassKeyword);
    testKeyword(TokenKind::EndClockingKeyword);
    testKeyword(TokenKind::EndConfigKeyword);
    testKeyword(TokenKind::EndFunctionKeyword);
    testKeyword(TokenKind::EndGenerateKeyword);
    testKeyword(TokenKind::EndGroupKeyword);
    testKeyword(TokenKind::EndInterfaceKeyword);
    testKeyword(TokenKind::EndModuleKeyword);
    testKeyword(TokenKind::EndPackageKeyword);
    testKeyword(TokenKind::EndPrimitiveKeyword);
    testKeyword(TokenKind::EndProgramKeyword);
    testKeyword(TokenKind::EndPropertyKeyword);
    testKeyword(TokenKind::EndSpecifyKeyword);
    testKeyword(TokenKind::EndSequenceKeyword);
    testKeyword(TokenKind::EndTableKeyword);
    testKeyword(TokenKind::EndTaskKeyword);
    testKeyword(TokenKind::EnumKeyword);
    testKeyword(TokenKind::EventKeyword);
    testKeyword(TokenKind::EventuallyKeyword);
    testKeyword(TokenKind::ExpectKeyword);
    testKeyword(TokenKind::ExportKeyword);
    testKeyword(TokenKind::ExtendsKeyword);
    testKeyword(TokenKind::ExternKeyword);
    testKeyword(TokenKind::FinalKeyword);
    testKeyword(TokenKind::FirstMatchKeyword);
    testKeyword(TokenKind::ForKeyword);
    testKeyword(TokenKind::ForceKeyword);
    testKeyword(TokenKind::ForeachKeyword);
    testKeyword(TokenKind::ForeverKeyword);
    testKeyword(TokenKind::ForkKeyword);
    testKeyword(TokenKind::ForkJoinKeyword);
    testKeyword(TokenKind::FunctionKeyword);
    testKeyword(TokenKind::GenerateKeyword);
    testKeyword(TokenKind::GenVarKeyword);
    testKeyword(TokenKind::GlobalKeyword);
    testKeyword(TokenKind::HighZ0Keyword);
    testKeyword(TokenKind::HighZ1Keyword);
    testKeyword(TokenKind::IfKeyword);
    testKeyword(TokenKind::IffKeyword);
    testKeyword(TokenKind::IfNoneKeyword);
    testKeyword(TokenKind::IgnoreBinsKeyword);
    testKeyword(TokenKind::IllegalBinsKeyword);
    testKeyword(TokenKind::ImplementsKeyword);
    testKeyword(TokenKind::ImpliesKeyword);
    testKeyword(TokenKind::ImportKeyword);
    testKeyword(TokenKind::IncDirKeyword);
    testKeyword(TokenKind::IncludeKeyword);
    testKeyword(TokenKind::InitialKeyword);
    testKeyword(TokenKind::InOutKeyword);
    testKeyword(TokenKind::InputKeyword);
    testKeyword(TokenKind::InsideKeyword);
    testKeyword(TokenKind::InstanceKeyword);
    testKeyword(TokenKind::IntKeyword);
    testKeyword(TokenKind::IntegerKeyword);
    testKeyword(TokenKind::InterconnectKeyword);
    testKeyword(TokenKind::InterfaceKeyword);
    testKeyword(TokenKind::IntersectKeyword);
    testKeyword(TokenKind::JoinKeyword);
    testKeyword(TokenKind::JoinAnyKeyword);
    testKeyword(TokenKind::JoinNoneKeyword);
    testKeyword(TokenKind::LargeKeyword);
    testKeyword(TokenKind::LetKeyword);
    testKeyword(TokenKind::LibListKeyword);
    testKeyword(TokenKind::LibraryKeyword);
    testKeyword(TokenKind::LocalKeyword);
    testKeyword(TokenKind::LocalParamKeyword);
    testKeyword(TokenKind::LogicKeyword);
    testKeyword(TokenKind::LongIntKeyword);
    testKeyword(TokenKind::MacromoduleKeyword);
    testKeyword(TokenKind::MatchesKeyword);
    testKeyword(TokenKind::MediumKeyword);
    testKeyword(TokenKind::ModPortKeyword);
    testKeyword(TokenKind::ModuleKeyword);
    testKeyword(TokenKind::NandKeyword);
    testKeyword(TokenKind::NegEdgeKeyword);
    testKeyword(TokenKind::NetTypeKeyword);
    testKeyword(TokenKind::NewKeyword);
    testKeyword(TokenKind::NextTimeKeyword);
    testKeyword(TokenKind::NmosKeyword);
    testKeyword(TokenKind::NorKeyword);
    testKeyword(TokenKind::NoShowCancelledKeyword);
    testKeyword(TokenKind::NotKeyword);
    testKeyword(TokenKind::NotIf0Keyword);
    testKeyword(TokenKind::NotIf1Keyword);
    testKeyword(TokenKind::NullKeyword);
    testKeyword(TokenKind::OrKeyword);
    testKeyword(TokenKind::OutputKeyword);
    testKeyword(TokenKind::PackageKeyword);
    testKeyword(TokenKind::PackedKeyword);
    testKeyword(TokenKind::ParameterKeyword);
    testKeyword(TokenKind::PmosKeyword);
    testKeyword(TokenKind::PosEdgeKeyword);
    testKeyword(TokenKind::PrimitiveKeyword);
    testKeyword(TokenKind::PriorityKeyword);
    testKeyword(TokenKind::ProgramKeyword);
    testKeyword(TokenKind::PropertyKeyword);
    testKeyword(TokenKind::ProtectedKeyword);
    testKeyword(TokenKind::Pull0Keyword);
    testKeyword(TokenKind::Pull1Keyword);
    testKeyword(TokenKind::PullDownKeyword);
    testKeyword(TokenKind::PullUpKeyword);
    testKeyword(TokenKind::PulseStyleOnDetectKeyword);
    testKeyword(TokenKind::PulseStyleOnEventKeyword);
    testKeyword(TokenKind::PureKeyword);
    testKeyword(TokenKind::RandKeyword);
    testKeyword(TokenKind::RandCKeyword);
    testKeyword(TokenKind::RandCaseKeyword);
    testKeyword(TokenKind::RandSequenceKeyword);
    testKeyword(TokenKind::RcmosKeyword);
    testKeyword(TokenKind::RealKeyword);
    testKeyword(TokenKind::RealTimeKeyword);
    testKeyword(TokenKind::RefKeyword);
    testKeyword(TokenKind::RegKeyword);
    testKeyword(TokenKind::RejectOnKeyword);
    testKeyword(TokenKind::ReleaseKeyword);
    testKeyword(TokenKind::RepeatKeyword);
    testKeyword(TokenKind::RestrictKeyword);
    testKeyword(TokenKind::ReturnKeyword);
    testKeyword(TokenKind::RnmosKeyword);
    testKeyword(TokenKind::RpmosKeyword);
    testKeyword(TokenKind::RtranKeyword);
    testKeyword(TokenKind::RtranIf0Keyword);
    testKeyword(TokenKind::RtranIf1Keyword);
    testKeyword(TokenKind::SAlwaysKeyword);
    testKeyword(TokenKind::SEventuallyKeyword);
    testKeyword(TokenKind::SNextTimeKeyword);
    testKeyword(TokenKind::SUntilKeyword);
    testKeyword(TokenKind::SUntilWithKeyword);
    testKeyword(TokenKind::ScalaredKeyword);
    testKeyword(TokenKind::SequenceKeyword);
    testKeyword(TokenKind::ShortIntKeyword);
    testKeyword(TokenKind::ShortRealKeyword);
    testKeyword(TokenKind::ShowCancelledKeyword);
    testKeyword(TokenKind::SignedKeyword);
    testKeyword(TokenKind::SmallKeyword);
    testKeyword(TokenKind::SoftKeyword);
    testKeyword(TokenKind::SolveKeyword);
    testKeyword(TokenKind::SpecifyKeyword);
    testKeyword(TokenKind::SpecParamKeyword);
    testKeyword(TokenKind::StaticKeyword);
    testKeyword(TokenKind::StringKeyword);
    testKeyword(TokenKind::StrongKeyword);
    testKeyword(TokenKind::Strong0Keyword);
    testKeyword(TokenKind::Strong1Keyword);
    testKeyword(TokenKind::StructKeyword);
    testKeyword(TokenKind::SuperKeyword);
    testKeyword(TokenKind::Supply0Keyword);
    testKeyword(TokenKind::Supply1Keyword);
    testKeyword(TokenKind::SyncAcceptOnKeyword);
    testKeyword(TokenKind::SyncRejectOnKeyword);
    testKeyword(TokenKind::TableKeyword);
    testKeyword(TokenKind::TaggedKeyword);
    testKeyword(TokenKind::TaskKeyword);
    testKeyword(TokenKind::ThisKeyword);
    testKeyword(TokenKind::ThroughoutKeyword);
    testKeyword(TokenKind::TimeKeyword);
    testKeyword(TokenKind::TimePrecisionKeyword);
    testKeyword(TokenKind::TimeUnitKeyword);
    testKeyword(TokenKind::TranKeyword);
    testKeyword(TokenKind::TranIf0Keyword);
    testKeyword(TokenKind::TranIf1Keyword);
    testKeyword(TokenKind::TriKeyword);
    testKeyword(TokenKind::Tri0Keyword);
    testKeyword(TokenKind::Tri1Keyword);
    testKeyword(TokenKind::TriAndKeyword);
    testKeyword(TokenKind::TriOrKeyword);
    testKeyword(TokenKind::TriRegKeyword);
    testKeyword(TokenKind::TypeKeyword);
    testKeyword(TokenKind::TypedefKeyword);
    testKeyword(TokenKind::UnionKeyword);
    testKeyword(TokenKind::UniqueKeyword);
    testKeyword(TokenKind::Unique0Keyword);
    testKeyword(TokenKind::UnsignedKeyword);
    testKeyword(TokenKind::UntilKeyword);
    testKeyword(TokenKind::UntilWithKeyword);
    testKeyword(TokenKind::UntypedKeyword);
    testKeyword(TokenKind::UseKeyword);
    testKeyword(TokenKind::UWireKeyword);
    testKeyword(TokenKind::VarKeyword);
    testKeyword(TokenKind::VectoredKeyword);
    testKeyword(TokenKind::VirtualKeyword);
    testKeyword(TokenKind::VoidKeyword);
    testKeyword(TokenKind::WaitKeyword);
    testKeyword(TokenKind::WaitOrderKeyword);
    testKeyword(TokenKind::WAndKeyword);
    testKeyword(TokenKind::WeakKeyword);
    testKeyword(TokenKind::Weak0Keyword);
    testKeyword(TokenKind::Weak1Keyword);
    testKeyword(TokenKind::WhileKeyword);
    testKeyword(TokenKind::WildcardKeyword);
    testKeyword(TokenKind::WireKeyword);
    testKeyword(TokenKind::WithKeyword);
    testKeyword(TokenKind::WithinKeyword);
    testKeyword(TokenKind::WOrKeyword);
    testKeyword(TokenKind::XnorKeyword);
    testKeyword(TokenKind::XorKeyword);
}

void testPunctuation(TokenKind kind) {
    string_view text = getTokenKindText(kind);
    Token token = lexToken(text);

    CHECK(token.kind == kind);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(token.valueText() == text);
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("All Punctuation", "[lexer]") {
    testPunctuation(TokenKind::ApostropheOpenBrace);
    testPunctuation(TokenKind::OpenBrace);
    testPunctuation(TokenKind::CloseBrace);
    testPunctuation(TokenKind::OpenBracket);
    testPunctuation(TokenKind::CloseBracket);
    testPunctuation(TokenKind::OpenParenthesis);
    testPunctuation(TokenKind::OpenParenthesisStar);
    testPunctuation(TokenKind::OpenParenthesisStarCloseParenthesis);
    testPunctuation(TokenKind::CloseParenthesis);
    testPunctuation(TokenKind::StarCloseParenthesis);
    testPunctuation(TokenKind::Semicolon);
    testPunctuation(TokenKind::Colon);
    testPunctuation(TokenKind::ColonEquals);
    testPunctuation(TokenKind::ColonSlash);
    testPunctuation(TokenKind::DoubleColon);
    testPunctuation(TokenKind::StarDoubleColonStar);
    testPunctuation(TokenKind::Comma);
    testPunctuation(TokenKind::DotStar);
    testPunctuation(TokenKind::Dot);
    testPunctuation(TokenKind::Slash);
    testPunctuation(TokenKind::Star);
    testPunctuation(TokenKind::DoubleStar);
    testPunctuation(TokenKind::StarArrow);
    testPunctuation(TokenKind::Plus);
    testPunctuation(TokenKind::DoublePlus);
    testPunctuation(TokenKind::PlusColon);
    testPunctuation(TokenKind::Minus);
    testPunctuation(TokenKind::DoubleMinus);
    testPunctuation(TokenKind::MinusColon);
    testPunctuation(TokenKind::MinusArrow);
    testPunctuation(TokenKind::MinusDoubleArrow);
    testPunctuation(TokenKind::Tilde);
    testPunctuation(TokenKind::TildeAnd);
    testPunctuation(TokenKind::TildeOr);
    testPunctuation(TokenKind::TildeXor);
    testPunctuation(TokenKind::Dollar);
    testPunctuation(TokenKind::Question);
    testPunctuation(TokenKind::Hash);
    testPunctuation(TokenKind::DoubleHash);
    testPunctuation(TokenKind::HashMinusHash);
    testPunctuation(TokenKind::HashEqualsHash);
    testPunctuation(TokenKind::Xor);
    testPunctuation(TokenKind::XorTilde);
    testPunctuation(TokenKind::Equals);
    testPunctuation(TokenKind::DoubleEquals);
    testPunctuation(TokenKind::DoubleEqualsQuestion);
    testPunctuation(TokenKind::TripleEquals);
    testPunctuation(TokenKind::EqualsArrow);
    testPunctuation(TokenKind::PlusEqual);
    testPunctuation(TokenKind::MinusEqual);
    testPunctuation(TokenKind::SlashEqual);
    testPunctuation(TokenKind::StarEqual);
    testPunctuation(TokenKind::AndEqual);
    testPunctuation(TokenKind::OrEqual);
    testPunctuation(TokenKind::PercentEqual);
    testPunctuation(TokenKind::XorEqual);
    testPunctuation(TokenKind::LeftShiftEqual);
    testPunctuation(TokenKind::TripleLeftShiftEqual);
    testPunctuation(TokenKind::RightShiftEqual);
    testPunctuation(TokenKind::TripleRightShiftEqual);
    testPunctuation(TokenKind::LeftShift);
    testPunctuation(TokenKind::RightShift);
    testPunctuation(TokenKind::TripleLeftShift);
    testPunctuation(TokenKind::TripleRightShift);
    testPunctuation(TokenKind::Exclamation);
    testPunctuation(TokenKind::ExclamationEquals);
    testPunctuation(TokenKind::ExclamationEqualsQuestion);
    testPunctuation(TokenKind::ExclamationDoubleEquals);
    testPunctuation(TokenKind::Percent);
    testPunctuation(TokenKind::LessThan);
    testPunctuation(TokenKind::LessThanEquals);
    testPunctuation(TokenKind::LessThanMinusArrow);
    testPunctuation(TokenKind::GreaterThan);
    testPunctuation(TokenKind::GreaterThanEquals);
    testPunctuation(TokenKind::Or);
    testPunctuation(TokenKind::DoubleOr);
    testPunctuation(TokenKind::OrMinusArrow);
    testPunctuation(TokenKind::OrMinusDoubleArrow);
    testPunctuation(TokenKind::OrEqualsArrow);
    testPunctuation(TokenKind::At);
    testPunctuation(TokenKind::AtStar);
    testPunctuation(TokenKind::DoubleAt);
    testPunctuation(TokenKind::And);
    testPunctuation(TokenKind::DoubleAnd);
    testPunctuation(TokenKind::TripleAnd);
}

void testDirectivePunctuation(TokenKind kind) {
    string_view text = getTokenKindText(kind);

    diagnostics.clear();
    auto buffer = getSourceManager().assignText(text);
    Lexer lexer(buffer, alloc, diagnostics);

    Token token = lexer.lex(LexerMode::Directive);

    CHECK(token.kind == kind);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(token.valueText() == text);
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("Directive Punctuation", "[lexer]") {
    testDirectivePunctuation(TokenKind::MacroQuote);
    testDirectivePunctuation(TokenKind::MacroEscapedQuote);
    testDirectivePunctuation(TokenKind::MacroPaste);
}

TEST_CASE("Punctuation corner cases", "[lexer]") {
    // These look like the start of a longer token but are not, so the
    // lexer needs to properly fallback to the original character.
    Token token = lexToken("#-");
    CHECK(token.kind == TokenKind::Hash);
    CHECK_DIAGNOSTICS_EMPTY;

    token = lexToken("#=");
    CHECK(token.kind == TokenKind::Hash);
    CHECK_DIAGNOSTICS_EMPTY;

    token = lexToken("*::");
    CHECK(token.kind == TokenKind::Star);
    CHECK_DIAGNOSTICS_EMPTY;

    token = lexToken("<-");
    CHECK(token.kind == TokenKind::LessThan);
    CHECK_DIAGNOSTICS_EMPTY;

    token = lexToken("|-");
    CHECK(token.kind == TokenKind::Or);
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("Include file name", "[lexer]") {
    auto& text = "  <asdf>";
    Token token = lexRawToken(text, LexerMode::IncludeFileName);

    CHECK(token.kind == TokenKind::IncludeFileName);
    CHECK(token.toString(SyntaxToStringFlags::IncludeTrivia) == text);
    CHECK(token.valueText() == "<asdf>");
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("Include file name (bad)", "[lexer]") {
    auto& text = "  asdf";
    Token token = lexRawToken(text, LexerMode::IncludeFileName);

    CHECK(token.kind == TokenKind::IncludeFileName);
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics.back().code == DiagCode::ExpectedIncludeFileName);
}

TEST_CASE("Include file name (unterminated)", "[lexer]") {
    auto& text = "  \"asdf";
    Token token = lexRawToken(text, LexerMode::IncludeFileName);

    CHECK(token.kind == TokenKind::IncludeFileName);
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics.back().code == DiagCode::ExpectedIncludeFileName);
}
