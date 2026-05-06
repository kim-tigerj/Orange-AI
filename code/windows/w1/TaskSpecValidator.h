#pragma once

// TaskSpecValidator ?????spawn ???묒뾽 紐낆꽭 媛뺤젣.
//
// ?뺥??μ쓽 ?듭떖 ??븷? *踰덉뿭* (CLAUDE.md 짠2). ??먯뿉寃??묒뾽??遺꾨같?????ъ슜???섎룄瑜?// ??먯씠 ?댄빐?????덇쾶 紐낆꽭 ?뚯씪??*?곷뒗* ?④퀎媛 怨?踰덉뿭. ??寃利앷린??洹?踰덉뿭 ?④퀎瑜?// *?쒖뒪?쒖뿉??媛뺤젣* ?⑸땲????紐낆꽭媛 誘몃떖?대㈃ spawn ?먯껜媛 嫄곕??⑸땲??
//
// 洹쒖튃:
//   - ?ㅼ쓬 ???뱀뀡 ?ㅻ뜑媛 紐⑤몢 ?덉뼱???⑸땲??
//       `## 紐⑹쟻` ?먮뒗 `## 諛곌꼍`
//       `## 踰붿쐞`
//       `## 寃利?
//   - 蹂몃Ц(怨듬갚쨌?ㅻ뜑 ?쇱씤 ?쒖쇅) ?⑷퀎媛 200???댁긽?댁뼱???⑸땲??
//
// 媛숈? 洹쒖튃??bash 痢?(`tools/spawn-member.sh`) ?먮룄 諛뺥??덉뼱, ??吏꾩엯?먯씠 媛숈? 寃곗쓣 怨듭쑀?⑸땲??

#include <windows.h>
#include <fstream>
#include <sstream>
#include <string>

namespace orange {

class CTaskSpecValidator {
public:
    struct Result {
        bool         ok = false;
        std::wstring errorMessage;  // ?쒓뎅??移쒖젅 ?덈궡 (?ㅽ뙣 ??.
    };

    // ?뚯씪???쎌뼱 寃利? ?뚯씪 ?놁쓬쨌?쎄린 ?ㅽ뙣??ok=false 濡?諛섑솚.
    static Result ValidateFile(const std::wstring& path) {
        Result r;
        std::ifstream ifs(path);
        if (!ifs) {
            r.errorMessage = L"紐낆꽭 ?뚯씪???????놁뒿?덈떎: " + path;
            return r;
        }
        std::stringstream ss;
        ss << ifs.rdbuf();
        std::string utf8 = ss.str();
        return ValidateUtf8(utf8);
    }

    // UTF-8 蹂몃Ц 吏곸젒 寃利? ?⑥쐞 ?뚯뒪?몄? ?ы띁 ?몄텧??
    static Result ValidateUtf8(const std::string& utf8) {
        Result r;

        bool hasPurpose = false;   // 紐⑹쟻 ?먮뒗 諛곌꼍
        bool hasScope   = false;   // 踰붿쐞
        bool hasVerify  = false;
        size_t bodyChars = 0;

        std::stringstream lineStream(utf8);
        std::string line;
        while (std::getline(lineStream, line)) {
            // CR ?쒓굅 (CRLF ???.
            if (!line.empty() && line.back() == '\r') line.pop_back();

            const std::string trimmed = TrimLeft(line);

            // ?ㅻ뜑 ?쇱씤 寃?? `## 紐⑹쟻` / `## 諛곌꼍` / `## 踰붿쐞` / `## 寃利?.
            // ?쒓뎅??湲???욎뿉 怨듬갚??0媛??댁긽, `##` ?ㅼ쓬 怨듬갚 ?????댁긽 ?뺤떇.
            if (StartsWith(trimmed, "## ")) {
                std::string title = TrimLeft(trimmed.substr(3));
                title = TrimRight(title);
                // title ??泥??좏겙留?蹂대㈃ 留ㅼ묶 ???⑥닚. 洹몃?濡??곕㈃ *"## 紐⑹쟻 (?????묒뾽)"* ?뺤떇???듦낵.
                if (StartsWithToken(title, "\xEB\xAA\xA9\xEC\xA0\x81") ||  // 紐⑹쟻
                    StartsWithToken(title, "\xEB\xB0\xB0\xEA\xB2\xBD")) {  // 諛곌꼍
                    hasPurpose = true;
                } else if (StartsWithToken(title, "\xEB\xB2\x94\xEC\x9C\x84")) {  // 踰붿쐞
                    hasScope = true;
                } else if (StartsWithToken(title, "\xEA\xB2\x80\xEC\xA6\x9D")) {
                    hasVerify = true;
                }
                continue;  // ?ㅻ뜑 ?먯껜??蹂몃Ц 湲???섏뿉???쒖쇅.
            }
            // ?ㅻ뜑(#) ?쇱씤 ?먯껜??蹂몃Ц?먯꽌 ?쒖쇅.
            if (StartsWith(trimmed, "#")) continue;

            // 蹂몃Ц 湲??????UTF-8 肄붾뱶?ъ씤???⑥쐞 (?곸뼱???쒓뎅?대뱺 湲????媛쒕줈 ??.
            bodyChars += CountUtf8CodePoints(trimmed);
        }

        std::wstring missing;
        if (!hasPurpose) missing += L"`## 紐⑹쟻` ?먮뒗 `## 諛곌꼍`, ";
        if (!hasScope)   missing += L"`## 踰붿쐞`, ";
        if (!hasVerify)  missing += L"`## 寃利?, ";
        if (!missing.empty()) {
            // ?앹쓽 ", " ?쒓굅.
            if (missing.size() >= 2) missing.resize(missing.size() - 2);
            r.errorMessage = L"紐낆꽭???ㅼ쓬 ?뱀뀡??鍮좎죱?듬땲?? " + missing +
                             L".\n?묒뾽 紐낆꽭?먮뒗 `## 紐⑹쟻` (?먮뒗 `## 諛곌꼍`) 쨌 `## 踰붿쐞` 쨌 `## 寃利? ???뱀뀡??紐⑤몢 ?덉뼱???⑸땲?? tasks/_template.md 瑜?李멸퀬?섏꽭??";
            return r;
        }

        constexpr size_t kMinBodyChars = 200;
        if (bodyChars < kMinBodyChars) {
            wchar_t buf[256];
            swprintf_s(buf,
                       L"紐낆꽭 蹂몃Ц???덈Т 吏㏃뒿?덈떎 (%zu?? 理쒖냼 %zu???꾩슂).\n??먯씠 ?묒뾽???댄빐?섎젮硫?紐⑹쟻쨌踰붿쐞쨌寃利앹쓣 異⑸텇??????곸뼱 二쇱꽭?? tasks/_template.md 瑜?李멸퀬?섏꽭??",
                       bodyChars, kMinBodyChars);
            r.errorMessage = buf;
            return r;
        }

        r.ok = true;
        return r;
    }

private:
    static bool StartsWith(const std::string& s, const std::string& prefix) {
        if (s.size() < prefix.size()) return false;
        return std::equal(prefix.begin(), prefix.end(), s.begin());
    }

    static bool StartsWithToken(const std::string& s, const std::string& token) {
        // s 媛 token ?쇰줈 ?쒖옉?섍퀬, 洹??ㅼ쓬 湲?먭? ??怨듬갚/臾몄옣遺?몃㈃ true.
        if (!StartsWith(s, token)) return false;
        if (s.size() == token.size()) return true;
        const unsigned char next = (unsigned char)s[token.size()];
        // ?쒓뎅?닿? ?꾨땶 ASCII 怨듬갚쨌臾몄옣遺?몃㈃ ?좏겙 寃쎄퀎濡??몄젙.
        return (next == ' ' || next == '\t' || next == '(' || next == '[' ||
                next == '-' || next == ':' || next == ',' || next == '.' ||
                next == '/' || next == '\xE2');  // U+2014 媛숈? ?쒓? dash ?쒖옉 諛붿씠??
    }

    static std::string TrimLeft(const std::string& s) {
        size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        return s.substr(i);
    }

    static std::string TrimRight(const std::string& s) {
        size_t n = s.size();
        while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) --n;
        return s.substr(0, n);
    }

    static size_t CountUtf8CodePoints(const std::string& s) {
        size_t count = 0;
        for (size_t i = 0; i < s.size(); ) {
            const unsigned char c = (unsigned char)s[i];
            // continuation byte (10xxxxxx) ????肄붾뱶?ъ씤???쒖옉???꾨떂.
            if (c < 0x80)        i += 1;
            else if (c < 0xC0)   i += 1;  // ?먯긽???쒗??????諛붿씠?몄뵫 吏꾪뻾.
            else if (c < 0xE0)   i += 2;
            else if (c < 0xF0)   i += 3;
            else                 i += 4;
            ++count;
        }
        return count;
    }
};

}  // namespace orange
