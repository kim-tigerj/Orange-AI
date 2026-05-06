#pragma once

#include <string>
#include <vector>
#include <unordered_set>
#include "StyledText.h"

namespace orange {

class CodeHighlighter {
public:
    static void Highlight(const std::wstring& text, const std::wstring& lang,
                          uint32_t offset, std::vector<TextSpan>& outSpans)
    {
        if (text.empty()) return;

        // ?뚮Ц?먮줈 蹂?섑븯??鍮꾧탳
        std::wstring l = lang;
        for (auto& c : l) c = towlower(c);

        const std::unordered_set<std::wstring>* keywords = &m_cppKeywords;
        const std::unordered_set<std::wstring>* types = &m_cppTypes;

        if (l == L"python" || l == L"py") {
            keywords = &m_pythonKeywords;
            types = &m_pythonTypes;
        } else if (l == L"javascript" || l == L"js" || l == L"typescript" || l == L"ts") {
            keywords = &m_jsKeywords;
            types = &m_jsTypes;
        } else if (l == L"rust" || l == L"rs") {
            keywords = &m_rustKeywords;
            types = &m_rustTypes;
        }

        // ?꾩＜ ?⑥닚???곹깭 癒몄떊 湲곕컲 ?섏씠?쇱씠??
        enum class State { Normal, String, CommentLine, CommentBlock };
        State state = State::Normal;
        size_t tokenStart = 0;

        auto isAlpha = [](wchar_t c) {
            return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'_';
        };
        auto isDigit = [](wchar_t c) {
            return (c >= L'0' && c <= L'9');
        };
        auto isAlNum = [&](wchar_t c) {
            return isAlpha(c) || isDigit(c);
        };

        for (size_t i = 0; i < text.size(); ++i) {
            wchar_t c = text[i];
            wchar_t next = (i + 1 < text.size()) ? text[i + 1] : L'\0';

            switch (state) {
            case State::Normal:
                if (c == L'/' && next == L'/') {
                    state = State::CommentLine;
                    tokenStart = i;
                    i++;
                } else if (c == L'/' && next == L'*') {
                    state = State::CommentBlock;
                    tokenStart = i;
                    i++;
                } else if (c == L'#' && (l == L"python" || l == L"py" || l == L"sh" || l == L"bash")) {
                    state = State::CommentLine;
                    tokenStart = i;
                } else if (c == L'"' || c == L'\'') {
                    state = State::String;
                    tokenStart = i;
                    m_quoteChar = c;
                } else if (isAlpha(c)) {
                    size_t start = i;
                    while (i + 1 < text.size() && isAlNum(text[i + 1])) i++;
                    std::wstring word = text.substr(start, i - start + 1);
                    TextSpan::Style style = TextSpan::Style::CodeBlock; // Default
                    if (keywords->count(word)) style = TextSpan::Style::CodeKeyword;
                    else if (types->count(word)) style = TextSpan::Style::CodeType;
                    else if (i + 1 < text.size() && text[i + 1] == L'(') style = TextSpan::Style::CodeFunction;

                    if (style != TextSpan::Style::CodeBlock) {
                        outSpans.push_back({ style, (uint32_t)(offset + start), (uint32_t)word.size() });
                    }
                } else if (isDigit(c)) {
                    size_t start = i;
                    while (i + 1 < text.size() && (isAlNum(text[i + 1]) || text[i + 1] == L'.')) i++;
                    outSpans.push_back({ TextSpan::Style::CodeNumber, (uint32_t)(offset + start), (uint32_t)(i - start + 1) });
                } else if (c == L'#' && l != L"python" && l != L"py") {
                    size_t start = i;
                    while (i + 1 < text.size() && isAlpha(text[i + 1])) i++;
                    outSpans.push_back({ TextSpan::Style::CodePreprocessor, (uint32_t)(offset + start), (uint32_t)(i - start + 1) });
                }
                break;

            case State::String:
                if (c == m_quoteChar && (i == 0 || text[i - 1] != L'\\')) {
                    outSpans.push_back({ TextSpan::Style::CodeString, (uint32_t)(offset + tokenStart), (uint32_t)(i - tokenStart + 1) });
                    state = State::Normal;
                }
                break;

            case State::CommentLine:
                if (c == L'\n') {
                    outSpans.push_back({ TextSpan::Style::CodeComment, (uint32_t)(offset + tokenStart), (uint32_t)(i - tokenStart) });
                    state = State::Normal;
                }
                break;

            case State::CommentBlock:
                if (c == L'*' && next == L'/') {
                    outSpans.push_back({ TextSpan::Style::CodeComment, (uint32_t)(offset + tokenStart), (uint32_t)(i - tokenStart + 2) });
                    state = State::Normal;
                    i++;
                }
                break;
            }
        }

        if (state == State::CommentLine || state == State::CommentBlock || state == State::String) {
            outSpans.push_back({ 
                (state == State::String ? TextSpan::Style::CodeString : TextSpan::Style::CodeComment),
                (uint32_t)(offset + tokenStart),
                (uint32_t)(text.size() - tokenStart)
            });
        }
    }

private:
    static inline wchar_t m_quoteChar = L'"';

    static inline const std::unordered_set<std::wstring> m_cppKeywords = {
        L"if", L"else", L"for", L"while", L"do", L"switch", L"case", L"default",
        L"break", L"continue", L"return", L"class", L"struct", L"public", L"private",
        L"protected", L"static", L"inline", L"virtual", L"override", L"const", L"volatile",
        L"template", L"typename", L"using", L"namespace", L"new", L"delete", L"try", L"catch", L"throw",
        L"constexpr", L"nullptr", L"explicit", L"noexcept", L"static_cast", L"dynamic_cast", L"const_cast",
        L"reinterpret_cast", L"friend", L"operator", L"this", L"decltype", L"auto", L"enum"
    };
    static inline const std::unordered_set<std::wstring> m_cppTypes = {
        L"int", L"float", L"double", L"char", L"bool", L"void", L"size_t", L"uint32_t", L"uint64_t",
        L"int32_t", L"int64_t", L"std", L"string", L"wstring", L"vector", L"map", L"set", L"unique_ptr", L"shared_ptr"
    };

    static inline const std::unordered_set<std::wstring> m_pythonKeywords = {
        L"if", L"else", L"elif", L"for", L"while", L"break", L"continue", L"return",
        L"def", L"class", L"import", L"from", L"as", L"try", L"except", L"finally", L"raise",
        L"with", L"yield", L"pass", L"lambda", L"async", L"await", L"assert", L"global", L"nonlocal",
        L"and", L"or", L"not", L"is", L"in", L"del"
    };
    static inline const std::unordered_set<std::wstring> m_pythonTypes = {
        L"self", L"None", L"True", L"False", L"int", L"float", L"str", L"list", L"dict", L"set", L"tuple", L"bool"
    };

    static inline const std::unordered_set<std::wstring> m_jsKeywords = {
        L"if", L"else", L"for", L"while", L"do", L"switch", L"case", L"default",
        L"break", L"continue", L"return", L"class", L"function", L"const", L"let", L"var",
        L"new", L"delete", L"try", L"catch", L"finally", L"throw", L"import", L"export", L"from", L"as",
        L"async", L"await", L"yield", L"this", L"super", L"extends", L"static", L"get", L"set",
        L"type", L"interface", L"enum", L"namespace", L"implements", L"readonly"
    };
    static inline const std::unordered_set<std::wstring> m_jsTypes = {
        L"number", L"string", L"boolean", L"object", L"any", L"void", L"null", L"undefined", L"true", L"false"
    };

    static inline const std::unordered_set<std::wstring> m_rustKeywords = {
        L"if", L"else", L"for", L"while", L"loop", L"match", L"return", L"break", L"continue",
        L"fn", L"pub", L"mut", L"let", L"const", L"static", L"impl", L"trait", L"struct", L"enum", L"type",
        L"use", L"mod", L"crate", L"self", L"Self", L"super", L"where", L"unsafe", L"async", L"await",
        L"move", L"dyn", L"as", L"in", L"ref"
    };
    static inline const std::unordered_set<std::wstring> m_rustTypes = {
        L"u8", L"u16", L"u32", L"u64", L"u128", L"usize", L"i8", L"i16", L"i32", L"i64", L"i128", L"isize",
        L"f32", L"f64", L"bool", L"char", L"str", L"String", L"Vec", L"Option", L"Result", L"Box", L"Arc", L"true", L"false"
    };
};

} // namespace orange
