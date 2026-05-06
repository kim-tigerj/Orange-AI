#pragma once

// BuildHistory ??蹂?OrangeCode.exe 媛 *?덈줈 鍮뚮뱶?섏뼱 泥섏쓬 ?좎삤瑜? ?꾩쟻 ?잛닔?
// 理쒖큹쨌理쒓렐 鍮뚮뱶 ?쒓컖???곸냽?뷀빀?덈떎.
//
// ?ъ슜???섎룄: *蹂??깆씠 ?쇰쭏???먯＜ 援대윭媛붾뒗吏* 瑜?留?spawn 留덈떎 ?쒕늿?? ?먭린-寃⑸컻 猷⑦봽??// ?꾩쟻???쒓컖?곸쑝濡??몄??섏뼱??*諛섎났 횞 ?쒓컙* 鍮꾩쟾???붾㈃??諛뺥옓?덈떎.
//
// ?뺤쓽: *?щ퉴?? = exe ??LastWriteTime ???붿뒪??湲곕줉怨??ㅻⅨ 寃쎌슦. 媛숈? exe ???⑥닚
// ?ъ떎?됱? 移댁슫?멸? ?щ씪媛吏 ?딆뒿?덈떎.
//
// ????꾩튂: %APPDATA%\OrangeCode\build_history.json (atomic .tmp ??rename).

#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include <string>

#include <json/json.h>

#include "Utils.h"

namespace orange {

struct BuildHistoryRecord {
    int          count = 0;
    std::wstring firstBuildIso;
    std::wstring lastBuildIso;
};

class CBuildHistory {
public:
    // 蹂?exe ??LastWriteTime ???쎄퀬 ?붿뒪??湲곕줉怨?鍮꾧탳.
    // ?ㅻⅤ硫?count++, last 媛깆떊 (first 鍮꾩뼱?덉쑝硫?媛숈씠 ?ㅼ젙), ?붿뒪??atomic write.
    // 媛숈쑝硫?read-only (?⑥닚 ?ъ떎????移댁슫??蹂??0).
    static BuildHistoryRecord Tick() {
        BuildHistoryRecord rec = Load();

        std::wstring exeLastIso = ExeLastWriteIso();
        if (exeLastIso.empty()) return rec;  // 紐??쎌쑝硫?洹몃?濡?

        if (exeLastIso != rec.lastBuildIso) {
            // ??鍮뚮뱶. count + 1, last 媛깆떊.
            rec.count += 1;
            rec.lastBuildIso = exeLastIso;
            if (rec.firstBuildIso.empty()) {
                rec.firstBuildIso = exeLastIso;
            }
            SaveAtomic(rec);
        }
        return rec;
    }

    // ?붿뒪??read 留????붾㈃ 媛깆떊??
    static BuildHistoryRecord Load() {
        BuildHistoryRecord rec;
        std::wstring path = HistoryPath();
        if (path.empty()) return rec;

        std::ifstream ifs(path);
        if (!ifs) return rec;

        std::stringstream ss;
        ss << ifs.rdbuf();

        Json::CharReaderBuilder rb;
        Json::Value root;
        std::string err;
        std::istringstream iss(ss.str());
        if (!Json::parseFromStream(rb, iss, &root, &err)) return rec;

        rec.count         = root.get("count", 0).asInt();
        std::string fb    = root.get("first_build_iso", "").asString();
        std::string lb    = root.get("last_build_iso",  "").asString();
        rec.firstBuildIso = std::wstring(fb.begin(), fb.end());
        rec.lastBuildIso  = std::wstring(lb.begin(), lb.end());
        if (rec.count < 0) rec.count = 0;
        return rec;
    }

private:
    static std::wstring HistoryPath() {
        wchar_t appdata[MAX_PATH] = L"";
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
            return L"";
        }
        std::wstring dir = std::wstring(appdata) + L"\\OrangeCode";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\build_history.json";
    }

    static std::wstring ExeLastWriteIso() {
        wchar_t exePath[MAX_PATH] = L"";
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return L"";
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(exePath, GetFileExInfoStandard, &fad)) return L"";
        return FileTimeToIso(fad.ftLastWriteTime);
    }

    static std::wstring FileTimeToIso(const FILETIME& ft) {
        SYSTEMTIME st{};
        if (!FileTimeToSystemTime(&ft, &st)) return L"";
        wchar_t buf[32];
        swprintf_s(buf, L"%04d-%02d-%02dT%02d:%02d:%02dZ",
                   st.wYear, st.wMonth, st.wDay,
                   st.wHour, st.wMinute, st.wSecond);
        return buf;
    }

    static bool SaveAtomic(const BuildHistoryRecord& rec) {
        std::wstring path = HistoryPath();
        if (path.empty()) return false;

        Json::Value root;
        root["version"]         = 1;
        root["count"]           = rec.count;
        // ISO ?쒓컙 臾몄옄?댁? ASCII 留뚯씠??wide ??narrow 蹂?섏씠 ?덉쟾?섏?留? 而댄뙆?쇰윭??wchar_t ??char
        // 蹂??寃쎄퀬 (C4244) 瑜??쇳븯湲??꾪빐 紐낆떆??UTF-8 蹂?섏쓣 ?ъ슜?⑸땲??
        root["first_build_iso"] = WideToUtf8(rec.firstBuildIso);
        root["last_build_iso"]  = WideToUtf8(rec.lastBuildIso);

        Json::StreamWriterBuilder w;
        w["indentation"] = "  ";
        std::string content = Json::writeString(w, root);

        std::wstring tmpPath = path + L".tmp";
        {
            std::ofstream ofs(tmpPath, std::ios::binary | std::ios::trunc);
            if (!ofs) return false;
            ofs.write(content.data(), (std::streamsize)content.size());
            if (!ofs) {
                ofs.close();
                DeleteFileW(tmpPath.c_str());
                return false;
            }
        }
        if (!MoveFileExW(tmpPath.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(tmpPath.c_str());
            return false;
        }
        return true;
    }
};

}  // namespace orange
