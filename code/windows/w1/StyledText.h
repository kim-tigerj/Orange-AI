#pragma once

// StyledText — 마크다운 파싱 결과의 *디스플레이 표현*.
// 마크다운 구문은 제거된 텍스트 + 스타일 적용 범위 목록.
// MarkdownParser 가 생성, OrangeView 가 IDWriteTextLayout 으로 그림.

#include <cstdint>
#include <string>
#include <vector>

namespace orange {

struct TextSpan {
    enum class Style : uint8_t {
        // 인라인
        Bold,
        Italic,
        InlineCode,
        Link,
        Strikethrough,
        // 블록 *간* 마커 — 자체 텍스트 없이 시각만 (수평선 등)
        HorizontalRule,
        // 코드블록 우상단 라벨 (` ```cpp ` 의 cpp). dim 색 + 작은 폰트.
        CodeBlockLang,
        // 이미지 placeholder — `[image: alt]` 형태. dim 색.
        ImagePlaceholder,
        // 블록 레벨 (전체 단락에 적용)
        CodeBlock,
        // 코드 내부 하이라이팅 (CodeBlock span 내부의 sub-spans)
        CodeKeyword,
        CodeComment,
        CodeString,
        CodeNumber,
        CodeType,       // int, float, std::string 등
        CodeFunction,   // 함수명
        CodePreprocessor, // #include, #define 등
        Heading1, Heading2, Heading3,
        Heading4, Heading5, Heading6,
        Quote,
        // 표 (Table) 지원
        TableCell,
        TableHeader,
    };
    Style        style;
    uint32_t     start;   // wide-char offset
    uint32_t     length;  // wide-char count
    std::wstring url;     // Style::Link 일 때만 — 클릭 시 ShellExecute 대상
};

struct TableCellInfo {
    uint32_t spanIdx;  // spans[] 에서의 인덱스
    uint16_t row;
    uint16_t col;
};

struct TableInfo {
    uint16_t rowCount;
    uint16_t colCount;
    std::vector<TableCellInfo> cells;
};

struct StyledText {
    std::wstring          text;
    std::vector<TextSpan> spans;
    std::vector<TableInfo> tables; // 블록 내 포함된 표 정보
};

}  // namespace orange
