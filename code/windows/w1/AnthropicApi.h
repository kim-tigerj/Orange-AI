#pragma once

// AnthropicApi ??/v1/messages ?붾뱶?ъ씤???몄텧 + ?묐떟 ?뚯떛.
// Phase 1: non-streaming ?⑥씪 硫붿떆吏 ?뺣났. ????대젰? ?몄텧?먭? 愿由?

#include "HttpClient.h"
#include <json/json.h>
#include <string>
#include <vector>
#include <sstream>

namespace orange {

class AnthropicApi {
public:
    struct Message {
        std::string role;     // "user" | "assistant"
        std::string content;  // UTF-8 ?띿뒪??    };

    struct Result {
        bool        ok = false;
        std::string text;     // assistant ?묐떟 ?띿뒪??(?깃났 ??
        std::string error;    // ?ㅻ쪟 (?ㅽ뙣 ??
    };

    // 紐⑤뜽 ID ???꾩슂 ???몄텧?먭? ?ㅻⅨ 媛?吏??媛??
    // (TODO: 異쒖떆 ?쒖젏??理쒖떊 stable 紐⑤뜽濡?媛깆떊)
    static constexpr const char* kDefaultModel = "claude-sonnet-4-5-20250929";

    explicit AnthropicApi(const std::string& apiKey)
        : m_apiKey(apiKey) {}

    Result SendMessage(const std::vector<Message>& history,
                       const std::string&          model     = kDefaultModel,
                       int                         maxTokens = 4096) const
    {
        Result result;

        // ?붿껌 JSON 援ъ꽦
        Json::Value root;
        root["model"]      = model;
        root["max_tokens"] = maxTokens;

        Json::Value messages(Json::arrayValue);
        for (const auto& msg : history) {
            Json::Value m;
            m["role"]    = msg.role;
            m["content"] = msg.content;
            messages.append(m);
        }
        root["messages"] = messages;

        Json::StreamWriterBuilder w;
        w["indentation"] = "";
        std::string body = Json::writeString(w, root);

        // ?ㅻ뜑 (wide string, CRLF 援щ텇, 留덉?留?CRLF ?놁쓬)
        std::wstringstream hdrs;
        hdrs << L"Content-Type: application/json\r\n";
        hdrs << L"x-api-key: " << Utf8ToWide(m_apiKey) << L"\r\n";
        hdrs << L"anthropic-version: 2023-06-01";

        // POST
        auto resp = HttpClient::PostJson(
            L"api.anthropic.com",
            L"/v1/messages",
            body,
            hdrs.str(),
            L"OrangeCode/0.1");

        if (!resp.ok) {
            result.error = "HTTP ?ㅻ쪟: " + resp.error +
                           " (?곹깭 " + std::to_string(resp.status) + ")";
            if (!resp.body.empty()) {
                result.error += "\n?묐떟 諛붾뵒: " + resp.body;
            }
            return result;
        }

        // ?묐떟 JSON ?뚯떛
        Json::Value             respJson;
        Json::CharReaderBuilder rb;
        std::string             parseErr;
        std::istringstream      iss(resp.body);
        if (!Json::parseFromStream(rb, iss, &respJson, &parseErr)) {
            result.error = "JSON ?뚯떛 ?ㅻ쪟: " + parseErr;
            return result;
        }

        // content: [ { type: "text", text: "..." }, ... ] ?먯꽌 text 遺遺꾨쭔 ?⑹묠
        if (!respJson.isMember("content") || !respJson["content"].isArray()) {
            result.error = "?묐떟 ?뺤떇 ?댁긽 (content 諛곗뿴 ?놁쓬). ?먮낯: " + resp.body;
            return result;
        }

        std::string text;
        for (const auto& part : respJson["content"]) {
            if (part.get("type", "").asString() == "text") {
                text += part.get("text", "").asString();
            }
        }

        result.ok   = true;
        result.text = text;
        return result;
    }

private:
    std::string m_apiKey;

    static std::wstring Utf8ToWide(const std::string& s) {
        if (s.empty()) return L"";
        int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                       nullptr, 0);
        std::wstring out(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(),
                            out.data(), wlen);
        return out;
    }
};

}  // namespace orange
