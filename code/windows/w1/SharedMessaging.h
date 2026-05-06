#pragma once

// SharedMessaging ??Windows ?ㅼ씠?곕툕 IPC: 1?④퀎 broadcast roll call.
//
// ?뺥????몄뒪?댁뒪 媛?/ ?몃? ?ы띁媛 *?댁븘?덈뒗 ?뺥???紐낅떒* ??諛쒓껄?섎뒗 ?먮━.
// 蹂?1?④퀎??broadcast (roll call) only ??1:1 inbox ? 紐낅졊 ?먮━???ㅼ쓬 ?④퀎.
//
// 援ъ“:
//   broadcast mapping = `Local\OrangeCode_Broadcast` (ring buffer, 32 ?щ’)
//       ?ㅻ뜑: writeIdx (atomic)
//       ?щ’: msgId, senderPid, type, respChannel(wchar[64])
//   ?묐떟 mapping     = `Local\OrangeCode_Resp_<respChannel>` (per-rollcall, 諛쒖떊?먭? 留뚮벀)
//       ?ㅻ뜑: count (atomic)
//       ?щ’: pid, id(wchar[64]), intent(wchar[128])
//   ?묐떟 event       = `Local\OrangeCode_Resp_<respChannel>_Evt` (?섏떊??源⑥? ???묐떟 諛뺥옄 ?뚮쭏??
//
// ?섎챸:
//   broadcast mapping = 泥?OpenFileMapping ??留뚮뱾怨? 紐⑤뱺 ?몃뱾 close ??kernel object ?먯뿰 ?댁젣.
//   ?묐떟 mapping/event = 諛쒖떊??lifetime ??(rollcall ???ъ씠??.

#include <windows.h>
#include <stdio.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <functional>

namespace orange {
namespace shm {

// ?? ?곸닔 ??????????????????????????????????????????????????????????????????????

constexpr int     kBroadcastCapacity = 32;          // ring buffer ?щ’ ??constexpr int     kRespCapacity      = 32;          // ?묐떟 mapping ?щ’ ??(?뺥???N紐??숈떆)
constexpr DWORD   kRollcallType      = 1;           // type 1 = roll call (1?④퀎 only)
constexpr DWORD   kDefaultTimeoutMs  = 3000;
constexpr wchar_t kBroadcastName[]   = L"Local\\OrangeCode_Broadcast";

// ?? 硫붿떆吏 援ъ“ (mapping ??raw) ?????????????????????????????????????????????

struct BroadcastSlot {
    DWORD   msgId;
    DWORD   senderPid;
    DWORD   type;
    DWORD   _pad;
    wchar_t respChannel[64];   // ?묐떟 mapping ??*吏㏃?* ?대쫫 (Local\\OrangeCode_Resp_<...>)
};

struct BroadcastHeader {
    LONG    writeIdx;          // InterlockedIncrement 濡??쒗솚
    LONG    version;
    LONG    _reserved[2];
};

struct RollcallRespSlot {
    DWORD   pid;
    DWORD   _pad;
    wchar_t id[64];
    wchar_t intent[128];
};

struct RollcallRespHeader {
    LONG    count;             // InterlockedIncrement 濡??먮━ ?뺣낫
    LONG    capacity;
    LONG    _reserved[2];
};

// ?? ?좏떥 ?????????????????????????????????????????????????????????????????????

// 怨좎쑀 ?몄뒪?댁뒪 ID = `<pid>_<starttime_filetime_low>`. PID ?ъ궗???덉쟾.
inline std::wstring InstanceId() {
    DWORD pid = GetCurrentProcessId();
    FILETIME ftCreate{}, ftExit{}, ftKernel{}, ftUser{};
    GetProcessTimes(GetCurrentProcess(), &ftCreate, &ftExit, &ftKernel, &ftUser);
    wchar_t buf[64];
    swprintf_s(buf, L"%lu_%lu", pid, (unsigned long)ftCreate.dwLowDateTime);
    return std::wstring(buf);
}

// ?묐떟 梨꾨꼸 ?꾩떆 ?대쫫 = `tmp_<pid>_<tick>`.
inline std::wstring NewRespChannel() {
    wchar_t buf[64];
    swprintf_s(buf, L"tmp_%lu_%llu", GetCurrentProcessId(),
               (unsigned long long)GetTickCount64());
    return std::wstring(buf);
}

// ?? 吏꾨떒 濡쒓렇 ?먮━ ??
// Windows GUI subsystem 寃곕줈 stdout ?먮━ ?먯꽭 紐⑦샇 ???붾쾭洹?遺꾧컙 ?꾪븳 file log.
// `%APPDATA%\OrangeCode\shm_debug.log` ???먮━???꾩쟻 append.
inline void DebugLog(const wchar_t* fmt, ...) {
    wchar_t appdata[MAX_PATH] = L"";
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) return;
    std::wstring path = std::wstring(appdata) + L"\\OrangeCode\\shm_debug.log";
    FILE* fp = nullptr;
    _wfopen_s(&fp, path.c_str(), L"a, ccs=UTF-8");
    if (!fp) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fwprintf(fp, L"[%04d-%02d-%02d %02d:%02d:%02d.%03d] [pid=%lu] ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
             GetCurrentProcessId());
    va_list args; va_start(args, fmt);
    vfwprintf(fp, fmt, args);
    va_end(args);
    fwprintf(fp, L"\n");
    fclose(fp);
}

// 諛쒖떊??(--rollcall) ???묐떟 ??stdout ?쇰줈 異쒕젰?섍린 ?꾪븳 ??
struct RollcallResult {
    DWORD        pid;
    std::wstring id;
    std::wstring intent;
};

// ?? broadcast mapping ?닿린/留뚮뱾湲????????????????????????????????????????????

inline HANDLE OpenOrCreateBroadcast(SIZE_T sizeBytes) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    sa.lpSecurityDescriptor = nullptr;
    return CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0,
                              (DWORD)sizeBytes, kBroadcastName);
}

inline SIZE_T BroadcastSize() {
    return sizeof(BroadcastHeader) + sizeof(BroadcastSlot) * kBroadcastCapacity;
}

// ?? 諛쒖떊???먯꽭 (rollcall ?ы띁) ?????????????????????????????????????????????

// broadcast 梨꾨꼸??roll call 諛뺢퀬 timeoutMs ???묐떟 紐⑥쓬.
// 鍮?紐낅떒???뺤긽 (?댁븘?덈뒗 ?뺥???0).
inline std::vector<RollcallResult> Rollcall(DWORD timeoutMs = kDefaultTimeoutMs) {
    std::vector<RollcallResult> out;
    DebugLog(L"Rollcall: start timeoutMs=%lu", timeoutMs);

    // 1) ?묐떟 梨꾨꼸 ?대쫫 + mapping/event ?앹꽦.
    std::wstring respChannel = NewRespChannel();
    std::wstring respMapName = std::wstring(L"Local\\OrangeCode_Resp_") + respChannel;
    std::wstring respEvtName = respMapName + L"_Evt";

    SIZE_T respSize = sizeof(RollcallRespHeader) + sizeof(RollcallRespSlot) * kRespCapacity;
    HANDLE hRespMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                         (DWORD)respSize, respMapName.c_str());
    if (!hRespMap) { DebugLog(L"Rollcall: respMap CreateFileMapping fail err=%lu", GetLastError()); return out; }
    auto* respView = (BYTE*)MapViewOfFile(hRespMap, FILE_MAP_ALL_ACCESS, 0, 0, respSize);
    if (!respView) { DebugLog(L"Rollcall: respView fail err=%lu", GetLastError()); CloseHandle(hRespMap); return out; }
    DebugLog(L"Rollcall: respMap created name=%s", respMapName.c_str());

    auto* respHdr = (RollcallRespHeader*)respView;
    auto* respSlots = (RollcallRespSlot*)(respView + sizeof(RollcallRespHeader));
    respHdr->count = 0;
    respHdr->capacity = kRespCapacity;

    HANDLE hRespEvt = CreateEventW(nullptr, FALSE, FALSE, respEvtName.c_str());

    // 2) broadcast mapping ?닿린/留뚮뱾湲?+ roll call ?щ’ 諛뺢린.
    HANDLE hBcast = OpenOrCreateBroadcast(BroadcastSize());
    if (!hBcast) { DebugLog(L"Rollcall: bcast CreateFileMapping fail err=%lu", GetLastError()); }
    if (hBcast) {
        auto* bcastView = (BYTE*)MapViewOfFile(hBcast, FILE_MAP_ALL_ACCESS, 0, 0, BroadcastSize());
        if (bcastView) {
            auto* hdr = (BroadcastHeader*)bcastView;
            auto* slots = (BroadcastSlot*)(bcastView + sizeof(BroadcastHeader));
            LONG idx = InterlockedIncrement(&hdr->writeIdx) - 1;
            int slotIdx = ((idx % kBroadcastCapacity) + kBroadcastCapacity) % kBroadcastCapacity;
            BroadcastSlot& s = slots[slotIdx];
            s.msgId = (DWORD)(idx + 1);
            s.senderPid = GetCurrentProcessId();
            s.type = kRollcallType;
            wcsncpy_s(s.respChannel, respChannel.c_str(), _TRUNCATE);
            DebugLog(L"Rollcall: ping idx=%ld slot=%d senderPid=%lu resp=%s",
                     idx, slotIdx, s.senderPid, s.respChannel);

            UnmapViewOfFile(bcastView);
        } else {
            DebugLog(L"Rollcall: bcast MapView fail err=%lu", GetLastError());
        }
        CloseHandle(hBcast);
    }

    // 3) timeoutMs ???묐떟 紐⑥쑝湲???留??묐떟留덈떎 event SetEvent ??wait ?由???read.
    DWORD start = GetTickCount();
    int collected = 0;
    while (true) {
        DWORD elapsed = GetTickCount() - start;
        if (elapsed >= timeoutMs) break;
        DWORD waitMs = timeoutMs - elapsed;

        DWORD wr = hRespEvt ? WaitForSingleObject(hRespEvt, waitMs) : WAIT_TIMEOUT;
        // event timeout ?먮뒗 ?좏샇 ???대뒓 履쎌씠??mapping ??踰?read.
        LONG count = InterlockedCompareExchange(&respHdr->count, 0, 0);
        for (int i = collected; i < count && i < kRespCapacity; ++i) {
            RollcallResult r;
            r.pid = respSlots[i].pid;
            r.id = respSlots[i].id;
            r.intent = respSlots[i].intent;
            out.push_back(r);
        }
        collected = (count < kRespCapacity) ? count : kRespCapacity;

        if (wr == WAIT_TIMEOUT) break;  // ?먯뿰 timeout ??醫낅즺
        // event ?좏샇 ???ㅼ쓬 ?묐떟 wait 怨꾩냽
    }

    UnmapViewOfFile(respView);
    CloseHandle(hRespMap);
    if (hRespEvt) CloseHandle(hRespEvt);
    DebugLog(L"Rollcall: end collected=%zu", out.size());
    return out;
}

// ?? ?섏떊???먯꽭 (?뺥???listener) ??????????????????????????????????????????

// ?뺥??μ씠 ?먭린 startup ??listener ?쒖옉. 蹂?1?④퀎??polling 寃?(200ms).
//   - broadcast mapping ?닿퀬 writeIdx 蹂??媛먯?.
//   - ???щ’ 諛쒓껄 ??(type == kRollcallType) ???묐떟 mapping ?댁뼱 ?먭린 ?뺣낫 諛뺢퀬 event SetEvent.
//   - intent ???몄텧?먭? 肄쒕갚?쇰줈 ?쒓났 ??Coordination ?먭린 entry ??currentIntent ?먮뒗 怨좎젙 ?쇰꺼.
class CBroadcastListener {
public:
    using IntentGetter = std::function<std::wstring()>;

    bool Start(const std::wstring& selfId, IntentGetter intentGetter) {
        if (m_thread) { DebugLog(L"listener Start: already started"); return true; }
        m_selfId = selfId;
        m_intentGetter = intentGetter;
        m_stopEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_stopEvt) { DebugLog(L"listener Start: CreateEvent fail err=%lu", GetLastError()); return false; }
        m_thread = CreateThread(nullptr, 0, &CBroadcastListener::ThreadProc, this, 0, nullptr);
        DebugLog(L"listener Start: thread=%p selfId=%s", m_thread, m_selfId.c_str());
        return m_thread != nullptr;
    }

    void Stop() {
        if (m_stopEvt) SetEvent(m_stopEvt);
        if (m_thread) {
            WaitForSingleObject(m_thread, 2000);
            CloseHandle(m_thread);
            m_thread = nullptr;
        }
        if (m_stopEvt) {
            CloseHandle(m_stopEvt);
            m_stopEvt = nullptr;
        }
    }

    ~CBroadcastListener() { Stop(); }

private:
    HANDLE       m_thread = nullptr;
    HANDLE       m_stopEvt = nullptr;
    std::wstring m_selfId;
    IntentGetter m_intentGetter;

    static DWORD WINAPI ThreadProc(LPVOID p) {
        ((CBroadcastListener*)p)->Run();
        return 0;
    }

    void Run() {
        HANDLE hBcast = OpenOrCreateBroadcast(BroadcastSize());
        if (!hBcast) { DebugLog(L"listener Run: bcast CreateFileMapping fail err=%lu", GetLastError()); return; }
        auto* view = (BYTE*)MapViewOfFile(hBcast, FILE_MAP_ALL_ACCESS, 0, 0, BroadcastSize());
        if (!view) { DebugLog(L"listener Run: MapViewOfFile fail err=%lu", GetLastError()); CloseHandle(hBcast); return; }

        auto* hdr = (BroadcastHeader*)view;
        auto* slots = (BroadcastSlot*)(view + sizeof(BroadcastHeader));

        // listener ?쒖옉 ?쒖젏??writeIdx 瑜?湲곗? ???댄썑 ???щ’留?泥섎━.
        LONG lastSeenIdx = InterlockedCompareExchange(&hdr->writeIdx, 0, 0);
        DebugLog(L"listener Run: started lastSeenIdx=%ld", lastSeenIdx);

        while (WaitForSingleObject(m_stopEvt, 200) == WAIT_TIMEOUT) {
            LONG curIdx = InterlockedCompareExchange(&hdr->writeIdx, 0, 0);
            if (curIdx != lastSeenIdx) {
                DebugLog(L"listener Run: curIdx=%ld lastSeenIdx=%ld", curIdx, lastSeenIdx);
            }
            while (lastSeenIdx < curIdx) {
                int slotIdx = ((lastSeenIdx % kBroadcastCapacity) + kBroadcastCapacity)
                              % kBroadcastCapacity;
                BroadcastSlot& s = slots[slotIdx];
                LONG msgId = (LONG)s.msgId;
                DWORD senderPid = s.senderPid;
                DWORD type = s.type;
                wchar_t respChannel[64] = L"";
                wcsncpy_s(respChannel, s.respChannel, _TRUNCATE);
                lastSeenIdx++;

                DebugLog(L"listener Run: slot[%d] msgId=%ld senderPid=%lu type=%lu resp=%s",
                         slotIdx, msgId, senderPid, type, respChannel);

                if (type != kRollcallType) continue;
                if (senderPid == GetCurrentProcessId()) { DebugLog(L"listener Run: self ping skipped"); continue; }

                Respond(respChannel);
            }
        }

        DebugLog(L"listener Run: stopping");
        UnmapViewOfFile(view);
        CloseHandle(hBcast);
    }

    void Respond(const wchar_t* respChannel) {
        if (!respChannel || !*respChannel) { DebugLog(L"Respond: empty channel"); return; }
        std::wstring respMapName = std::wstring(L"Local\\OrangeCode_Resp_") + respChannel;
        std::wstring respEvtName = respMapName + L"_Evt";

        HANDLE hMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, respMapName.c_str());
        if (!hMap) { DebugLog(L"Respond: OpenFileMapping fail name=%s err=%lu", respMapName.c_str(), GetLastError()); return; }
        SIZE_T respSize = sizeof(RollcallRespHeader) + sizeof(RollcallRespSlot) * kRespCapacity;
        auto* view = (BYTE*)MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, respSize);
        if (!view) { DebugLog(L"Respond: MapViewOfFile fail err=%lu", GetLastError()); CloseHandle(hMap); return; }

        auto* hdr = (RollcallRespHeader*)view;
        auto* respSlots = (RollcallRespSlot*)(view + sizeof(RollcallRespHeader));

        LONG idx = InterlockedIncrement(&hdr->count) - 1;
        if (idx < kRespCapacity) {
            RollcallRespSlot& r = respSlots[idx];
            r.pid = GetCurrentProcessId();
            wcsncpy_s(r.id, m_selfId.c_str(), _TRUNCATE);
            std::wstring intent = m_intentGetter ? m_intentGetter() : std::wstring();
            wcsncpy_s(r.intent, intent.c_str(), _TRUNCATE);
            DebugLog(L"Respond: slot[%ld] pid=%lu id=%s intent=%s", idx, r.pid, r.id, r.intent);
        } else {
            DebugLog(L"Respond: slot full idx=%ld cap=%d", idx, kRespCapacity);
        }

        UnmapViewOfFile(view);
        CloseHandle(hMap);

        HANDLE hEvt = OpenEventW(EVENT_MODIFY_STATE, FALSE, respEvtName.c_str());
        if (hEvt) {
            SetEvent(hEvt);
            CloseHandle(hEvt);
            DebugLog(L"Respond: SetEvent done");
        } else {
            DebugLog(L"Respond: OpenEvent fail err=%lu", GetLastError());
        }
    }
};

}  // namespace shm
}  // namespace orange
