#pragma once

// MarkdownParser — md4c 기반 마크다운 → StyledText 변환.
// wide 입력 → UTF-8 변환 → md4c 콜백으로 StyledText 누적 → 반환.
//
// 1차 지원 요소:
//   - 인라인: **굵게**, *기울임*, `인라인 코드`, [링크](url), ~~취소선~~ (GFM), ![이미지](src)
//   - 블록: # 헤더(H1~H6), ``` 코드블록 ```, > 인용, --- 수평선, | 표
//   - 리스트: - * + 불릿(중첩 OK), 1. 번호 매김 (start 번호 존중)
//   - 이미지는 `[image: alt]` 형태 placeholder (실제 이미지 그리기는 미지원)

#include <md4c.h>
#include <string>

#include "StyledText.h"
#include "Utils.h"
#include "CodeHighlighter.h"

namespace orange {

class MarkdownParser {
public:
    static StyledText Parse(const std::wstring& wideInput) {
        State state;
        std::string utf8 = WideToUtf8(wideInput);

        MD_PARSER parser{};
        parser.abi_version = 0;
        parser.flags       = MD_DIALECT_GITHUB;
        parser.enter_block = EnterBlock;
        parser.leave_block = LeaveBlock;
        parser.enter_span  = EnterSpan;
        parser.leave_span  = LeaveSpan;
        parser.text        = TextCb;

        md_parse(utf8.data(), (MD_SIZE)utf8.size(), &parser, &state);

        // 양 끝 newline 정리
        while (!state.styled.text.empty() &&
               state.styled.text.back() == L'\n')
        {
            state.styled.text.pop_back();
        }
        size_t lead = 0;
        while (lead < state.styled.text.size() &&
               state.styled.text[lead] == L'\n')
        {
            ++lead;
        }
        if (lead > 0) {
            state.styled.text.erase(0, lead);
            // span 시작점도 lead 만큼 당김 (단, 0보다 작아지면 0 으로)
            for (auto& sp : state.styled.spans) {
                if (sp.start >= (uint32_t)lead) {
                    sp.start -= (uint32_t)lead;
                } else {
                    uint32_t shrink = (uint32_t)lead - sp.start;
                    sp.start = 0;
                    sp.length = (sp.length > shrink) ? sp.length - shrink : 0;
                }
            }
        }

        return std::move(state.styled);
    }

private:
    struct OpenStyle {
        TextSpan::Style style;
        uint32_t        start;
        std::wstring    url;  // Link 일 때만
    };

    struct ListLevel {
        bool     ordered;
        unsigned counter;  // OL 의 다음 번호. UL 은 무시.
    };

    struct State {
        StyledText               styled;
        std::vector<OpenStyle>   openSpans;
        bool                     hasBlockStyle = false;
        OpenStyle                blockStyle{};
        std::wstring             currentCodeLang;       // 현재 처리 중인 코드 블록 언어
        uint32_t                 codeContentStart = 0;  // lang 라벨 이후 실제 코드 시작 위치
        bool                     firstTopBlock = true;  // 최상위 sibling 첫 등장 여부
        int                      depth         = 0;     // DOC 진입 시 1, 그 자식(top-level) 진입 시 2
        int                      imageDepth    = 0;
        std::vector<ListLevel>   listStack;             // 중첩 리스트 추적

        // 표 빌드 상태
        bool                     inTable = false;
        uint16_t                 currentRow = 0;
        uint16_t                 currentCol = 0;
        bool                     inHeader = false;
    };

    static int EnterBlock(MD_BLOCKTYPE type, void* detail, void* user) {
        auto& s = *static_cast<State*>(user);
        s.depth++;

        // depth == 2 = 최상위 sibling 콘텐츠 블록 (DOC 의 직접 자식). 그 사이에서만 빈 줄 삽입.
        if (s.depth == 2) {
            if (!s.firstTopBlock) s.styled.text += L"\n\n";
            s.firstTopBlock = false;
        }

        switch (type) {
        case MD_BLOCK_TABLE:
            s.inTable = true;
            s.currentRow = 0;
            s.styled.tables.emplace_back();
            break;
        case MD_BLOCK_THEAD:
            s.inHeader = true;
            break;
        case MD_BLOCK_TR:
            s.currentCol = 0;
            break;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD: {
            TextSpan::Style style = (type == MD_BLOCK_TH) ? TextSpan::Style::TableHeader : TextSpan::Style::TableCell;
            s.openSpans.push_back({ style, (uint32_t)s.styled.text.size() });
            break;
        }
        case MD_BLOCK_H: {
            auto* d = (MD_BLOCK_H_DETAIL*)detail;
            TextSpan::Style hs;
            switch (d->level) {
            case 1:  hs = TextSpan::Style::Heading1; break;
            case 2:  hs = TextSpan::Style::Heading2; break;
            case 3:  hs = TextSpan::Style::Heading3; break;
            case 4:  hs = TextSpan::Style::Heading4; break;
            case 5:  hs = TextSpan::Style::Heading5; break;
            default: hs = TextSpan::Style::Heading6; break;
            }
            s.blockStyle    = { hs, (uint32_t)s.styled.text.size() };
            s.hasBlockStyle = true;
            break;
        }
        case MD_BLOCK_CODE: {
            auto* d = (MD_BLOCK_CODE_DETAIL*)detail;
            s.currentCodeLang.clear();
            if (d && d->lang.text && d->lang.size > 0) {
                s.currentCodeLang = Utf8ToWide(d->lang.text, (int)d->lang.size);
            }
            uint32_t blockStart = (uint32_t)s.styled.text.size();
            if (!s.currentCodeLang.empty()) {
                // 언어 라벨을 코드 블록 첫 줄로 삽입 — CodeBlockLang span 으로 dim 스타일.
                s.styled.spans.push_back(TextSpan{
                    TextSpan::Style::CodeBlockLang, blockStart,
                    (uint32_t)s.currentCodeLang.size() + 1  // +1: '\n'
                });
                s.styled.text += s.currentCodeLang + L"\n";
            }
            s.codeContentStart = (uint32_t)s.styled.text.size();  // 실제 코드 시작
            s.blockStyle    = { TextSpan::Style::CodeBlock, blockStart };
            s.hasBlockStyle = true;
            break;
        }
        case MD_BLOCK_QUOTE:
            s.blockStyle    = { TextSpan::Style::Quote,
                                (uint32_t)s.styled.text.size() };
            s.hasBlockStyle = true;
            break;
        case MD_BLOCK_HR: {
            // 수평선 — md4c 가 텍스트 콜백 발급 안 하므로 EnterBlock 에서 직접 라인 문자 삽입.
            // 60자 정도면 일반 폭에서 시각적으로 라인. ApplySpans 가 HorizontalRule 케이스에서
            // 폰트 크기를 줄여 *가는 라인* 시각 결로 — 색 분기는 brush 인프라 정리(owner-draw
            // 사이클) 시 같이.
            static const wchar_t kHr[] =
                L"────────────────────────────────────────────────────────────";
            uint32_t start  = (uint32_t)s.styled.text.size();
            uint32_t length = (uint32_t)wcslen(kHr);
            s.styled.text += kHr;
            s.styled.spans.push_back(TextSpan{
                TextSpan::Style::HorizontalRule, start, length
            });
            break;
        }
        case MD_BLOCK_UL:
            s.listStack.push_back({ false, 0 });
            break;
        case MD_BLOCK_OL: {
            auto* d = (MD_BLOCK_OL_DETAIL*)detail;
            s.listStack.push_back({ true, d ? d->start : 1u });
            break;
        }
        case MD_BLOCK_LI: {
            if (s.listStack.empty()) break;
            auto& lvl = s.listStack.back();
            // 직전 컨텐츠가 줄바꿈으로 끝나지 않으면 보강 (UL의 첫 항목은 보통 "\n\n" 뒤라 자연스럽게 통과)
            if (!s.styled.text.empty() && s.styled.text.back() != L'\n') {
                s.styled.text += L'\n';
            }
            // 들여쓰기 — 중첩 깊이 -1 만큼 두 칸씩 (최상위 0칸)
            size_t indent = (s.listStack.size() - 1) * 2;
            s.styled.text.append(indent, L' ');
            // 마커
            if (lvl.ordered) {
                wchar_t buf[16];
                swprintf(buf, 16, L"%u. ", lvl.counter++);
                s.styled.text += buf;
            } else {
                s.styled.text += L"• ";
            }
            break;
        }
        default:
            break;
        }
        return 0;
    }

    static int LeaveBlock(MD_BLOCKTYPE type, void* /*detail*/, void* user) {
        auto& s = *static_cast<State*>(user);
        if (s.hasBlockStyle) {
            uint32_t start = s.blockStyle.start;
            uint32_t end   = (uint32_t)s.styled.text.size();
            if (end > start) {
                s.styled.spans.push_back(TextSpan{
                    s.blockStyle.style, start, end - start
                });

                // 코드 블록: lang 라벨 이후의 실제 코드만 하이라이팅.
                if (s.blockStyle.style == TextSpan::Style::CodeBlock) {
                    uint32_t codeStart = s.codeContentStart;
                    if (end > codeStart) {
                        std::wstring code = s.styled.text.substr(codeStart, end - codeStart);
                        CodeHighlighter::Highlight(code, s.currentCodeLang, codeStart, s.styled.spans);
                    }
                }
            }
            s.hasBlockStyle = false;
        }
        if ((type == MD_BLOCK_UL || type == MD_BLOCK_OL) && !s.listStack.empty()) {
            s.listStack.pop_back();
        }

        // 표 빌드 마무리
        if (type == MD_BLOCK_TABLE) {
            if (!s.styled.tables.empty()) {
                auto& tbl = s.styled.tables.back();
                tbl.rowCount = s.currentRow;
                // colCount 는 TR 에서 currentCol 로 계속 갱신되므로 최종값은 보존됨
            }
            s.inTable = false;
        } else if (type == MD_BLOCK_THEAD) {
            s.inHeader = false;
        } else if (type == MD_BLOCK_TH || type == MD_BLOCK_TD) {
            if (!s.openSpans.empty()) {
                OpenStyle os = std::move(s.openSpans.back());
                s.openSpans.pop_back();
                uint32_t end = (uint32_t)s.styled.text.size();
                if (end > os.start) {
                    uint32_t spanIdx = (uint32_t)s.styled.spans.size();
                    s.styled.spans.push_back({ os.style, os.start, end - os.start });
                    
                    if (s.inTable && !s.styled.tables.empty()) {
                        auto& tbl = s.styled.tables.back();
                        tbl.cells.push_back({ spanIdx, s.currentRow, s.currentCol });
                        if (s.currentCol + 1 > tbl.colCount) tbl.colCount = s.currentCol + 1;
                    }
                }
            }
            s.styled.text += L"    ";
            s.currentCol++;
        } else if (type == MD_BLOCK_TR) {
            while (!s.styled.text.empty() && s.styled.text.back() == L' ') {
                s.styled.text.pop_back();
            }
            s.styled.text += L"\n";
            s.currentRow++;
        }
        s.depth--;
        return 0;
    }

    static int EnterSpan(MD_SPANTYPE type, void* detail, void* user) {
        auto& s = *static_cast<State*>(user);
        TextSpan::Style style;
        std::wstring url;
        bool imageSpan = false;
        uint32_t imageStart = 0;
        switch (type) {
        case MD_SPAN_STRONG: style = TextSpan::Style::Bold;          break;
        case MD_SPAN_EM:     style = TextSpan::Style::Italic;        break;
        case MD_SPAN_CODE:   style = TextSpan::Style::InlineCode;    break;
        case MD_SPAN_DEL:    style = TextSpan::Style::Strikethrough; break;
        case MD_SPAN_A: {
            style = TextSpan::Style::Link;
            auto* d = (MD_SPAN_A_DETAIL*)detail;
            if (d && d->href.text && d->href.size > 0) {
                url = Utf8ToWide(d->href.text, (int)d->href.size);
            }
            break;
        }
        case MD_SPAN_IMG:
            style = TextSpan::Style::ImagePlaceholder;
            s.imageDepth++;
            imageSpan = true;
            imageStart = (uint32_t)s.styled.text.size();
            {
                auto* d = (MD_SPAN_IMG_DETAIL*)detail;
                if (d && d->src.text && d->src.size > 0) {
                    url = Utf8ToWide(d->src.text, (int)d->src.size);
                }
            }
            s.styled.text += L" ";
            break;
        default: return 0;  // 나머지 미지원
        }
        OpenStyle os;
        os.style = style;
        os.start = imageSpan ? imageStart : (uint32_t)s.styled.text.size();
        os.url   = std::move(url);
        s.openSpans.push_back(std::move(os));
        return 0;
    }

    static int LeaveSpan(MD_SPANTYPE type, void* /*detail*/, void* user) {
        auto& s = *static_cast<State*>(user);
        if (s.openSpans.empty()) return 0;
        // 이미지 placeholder 는 레이아웃 좌표만 확보하고 실제 화면에는 비트맵만 그린다.
        OpenStyle os = std::move(s.openSpans.back());
        s.openSpans.pop_back();

        uint32_t end = (uint32_t)s.styled.text.size();
        if (end > os.start) {
            TextSpan ts;
            ts.style  = os.style;
            ts.start  = os.start;
            ts.length = end - os.start;
            ts.url    = std::move(os.url);
            s.styled.spans.push_back(std::move(ts));
        }
        if (type == MD_SPAN_IMG) {
            if (s.imageDepth > 0) s.imageDepth--;
            s.styled.text += L"\n\n\n\n\n\n\n\n\n";
        }
        return 0;
    }

    static int TextCb(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* user) {
        auto& s = *static_cast<State*>(user);
        if (s.imageDepth > 0) return 0;
        switch (type) {
        case MD_TEXT_NORMAL:
        case MD_TEXT_CODE:
        case MD_TEXT_HTML:
        case MD_TEXT_LATEXMATH: {
            std::wstring wide = Utf8ToWide(text, (int)size);
            s.styled.text += wide;
            break;
        }
        case MD_TEXT_ENTITY: {
            // HTML entity (e.g., &amp;) — 단순화: raw 그대로 추가
            std::wstring wide = Utf8ToWide(text, (int)size);
            s.styled.text += wide;
            break;
        }
        case MD_TEXT_BR:
        case MD_TEXT_SOFTBR:
            s.styled.text += L'\n';
            break;
        case MD_TEXT_NULLCHAR:
            s.styled.text += L'�';  // U+FFFD replacement
            break;
        default:
            break;
        }
        return 0;
    }
};

}  // namespace orange
