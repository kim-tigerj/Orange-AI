#include "MainWindow.h"

#include "Utils.h"

#include <commdlg.h>
#include <shellapi.h>
#include <wincodec.h>

namespace orange {

namespace {

#ifndef BI_ALPHABITFIELDS
constexpr DWORD BI_ALPHABITFIELDS = 6;
#endif

std::wstring CurrentTimestampForFile() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[32] = L"";
    swprintf_s(buf, L"%04u%02u%02u-%02u%02u%02u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::wstring TempClipboardImagePath(const std::wstring& ext = L".png") {
    wchar_t temp[MAX_PATH] = L"";
    if (GetTempPathW(MAX_PATH, temp) == 0) return L"";
    return std::wstring(temp) + L"OrangeCode-clipboard-" + CurrentTimestampForFile() + ext;
}

std::wstring FolderOf(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L".";
    return path.substr(0, pos);
}

std::wstring FileUriForFolder(const std::wstring& path) {
    std::wstring folder = FolderOf(path);
    std::wstring uri = L"file:///";
    for (wchar_t ch : folder) {
        if (ch == L'\\') uri += L'/';
        else if (ch == L' ') uri += L"%20";
        else uri += ch;
    }
    return uri;
}

std::wstring FileUriForPath(const std::wstring& path) {
    std::wstring uri = L"file:///";
    for (wchar_t ch : path) {
        if (ch == L'\\') uri += L'/';
        else if (ch == L' ') uri += L"%20";
        else uri += ch;
    }
    return uri;
}

size_t DibPixelOffset(const BITMAPINFOHEADER* bih, SIZE_T dataSize) {
    if (!bih || bih->biSize < sizeof(BITMAPINFOHEADER) || bih->biSize > dataSize) return 0;

    DWORD colors = bih->biClrUsed;
    if (colors == 0 && bih->biBitCount <= 8) colors = 1u << bih->biBitCount;

    size_t offset = bih->biSize + (size_t)colors * sizeof(RGBQUAD);
    if (bih->biSize == sizeof(BITMAPINFOHEADER)) {
        if (bih->biCompression == BI_BITFIELDS) offset += sizeof(DWORD) * 3;
        else if (bih->biCompression == BI_ALPHABITFIELDS) offset += sizeof(DWORD) * 4;
    }

    return offset <= dataSize ? offset : 0;
}

bool SaveHBitmapToPng(HBITMAP bitmap, const std::wstring& outPath) {
    if (!bitmap || outPath.empty()) return false;

    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool needUninit = SUCCEEDED(coHr);

    IWICImagingFactory* factory = nullptr;
    IWICBitmap* wicBitmap = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateBitmapFromHBITMAP(
        bitmap, nullptr, WICBitmapUseAlpha, &wicBitmap);
    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(outPath.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &props);
    if (SUCCEEDED(hr)) hr = frame->Initialize(props);

    UINT w = 0, h = 0;
    if (SUCCEEDED(hr)) hr = wicBitmap->GetSize(&w, &h);
    if (SUCCEEDED(hr)) hr = frame->SetSize(w, h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);
    if (SUCCEEDED(hr)) hr = frame->WriteSource(wicBitmap, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();

    if (props) props->Release();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (wicBitmap) wicBitmap->Release();
    if (factory) factory->Release();
    if (needUninit) CoUninitialize();
    return SUCCEEDED(hr);
}

bool SaveDibToBmp(HANDLE handle, const std::wstring& outPath) {
    if (!handle || outPath.empty()) return false;
    SIZE_T size = GlobalSize(handle);
    if (size < sizeof(BITMAPINFOHEADER)) return false;

    void* data = GlobalLock(handle);
    if (!data) return false;

    const auto* bih = static_cast<const BITMAPINFOHEADER*>(data);
    size_t pixelOffset = DibPixelOffset(bih, size);
    if (pixelOffset == 0) {
        GlobalUnlock(handle);
        return false;
    }

    BITMAPFILEHEADER bfh{};
    bfh.bfType = 0x4D42;
    bfh.bfOffBits = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + pixelOffset);
    bfh.bfSize = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + size);

    HANDLE file = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        GlobalUnlock(handle);
        return false;
    }

    DWORD written = 0;
    bool ok = WriteFile(file, &bfh, sizeof(bfh), &written, nullptr) &&
              written == sizeof(bfh);
    if (ok) {
        written = 0;
        ok = WriteFile(file, data, (DWORD)size, &written, nullptr) &&
             written == (DWORD)size;
    }

    CloseHandle(file);
    GlobalUnlock(handle);
    if (!ok) DeleteFileW(outPath.c_str());
    return ok;
}

bool SaveGlobalMemoryToFile(HANDLE handle, const std::wstring& outPath) {
    if (!handle || outPath.empty()) return false;
    SIZE_T size = GlobalSize(handle);
    if (size == 0) return false;

    void* data = GlobalLock(handle);
    if (!data) return false;

    HANDLE file = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        GlobalUnlock(handle);
        return false;
    }

    const BYTE* cursor = static_cast<const BYTE*>(data);
    SIZE_T remaining = size;
    bool ok = true;
    while (remaining > 0) {
        DWORD chunk = (remaining > 1024 * 1024) ? 1024 * 1024 : (DWORD)remaining;
        DWORD written = 0;
        if (!WriteFile(file, cursor, chunk, &written, nullptr) || written != chunk) {
            ok = false;
            break;
        }
        cursor += written;
        remaining -= written;
    }

    CloseHandle(file);
    GlobalUnlock(handle);
    if (!ok) DeleteFileW(outPath.c_str());
    return ok;
}

} // namespace

void CMainWindow::HandleDroppedFiles(HDROP drop) {
    if (!drop) return;
    UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    std::vector<std::wstring> paths;
    for (UINT i = 0; i < count; ++i) {
        UINT len = DragQueryFileW(drop, i, nullptr, 0);
        if (len == 0) continue;
        std::wstring path(len + 1, L'\0');
        DragQueryFileW(drop, i, path.data(), len + 1);
        path.resize(len);
        paths.push_back(std::move(path));
    }
    DragFinish(drop);
    AddAttachmentPaths(paths);
}

void CMainWindow::AddAttachmentPaths(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    EnsureCurrentGlobalChat();

    std::vector<AttachmentRecord> added;
    std::wstring sessionKey = Utf8ToWide(m_currentChatKey.c_str(), (int)m_currentChatKey.size());
    for (const auto& path : paths) {
        AttachmentRecord rec;
        if (AttachmentStore::AddFile(sessionKey, path, &rec)) {
            Persistence::SaveAttachmentRecord(m_currentChatKey, rec);
            added.push_back(std::move(rec));
        }
    }

    if (!added.empty()) {
        AppendAttachmentCard(added);
    } else if (m_view) {
        m_view->NewBlock(L"error");
        m_view->AppendText(L"첨부 실패\n\n- 파일을 저장소에 복사하지 못했습니다.");
        m_view->ScrollLatestIntoView();
    }
}

void CMainWindow::SelectAttachmentFiles() {
    std::vector<wchar_t> buffer(65536, L'\0');

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hwnd;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = (DWORD)buffer.size();
    ofn.lpstrFilter = L"All files\0*.*\0Images\0*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp\0Text\0*.txt;*.md;*.log;*.json\0\0";
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = L"첨부할 파일 선택";

    if (!GetOpenFileNameW(&ofn)) return;

    std::vector<std::wstring> paths;
    const wchar_t* cursor = buffer.data();
    std::wstring first = cursor;
    cursor += first.size() + 1;
    if (*cursor == L'\0') {
        paths.push_back(first);
    } else {
        std::wstring dir = first;
        while (*cursor != L'\0') {
            std::wstring name = cursor;
            cursor += name.size() + 1;
            paths.push_back(dir + L"\\" + name);
        }
    }
    AddAttachmentPaths(paths);
}

void CMainWindow::AppendAttachmentCard(const std::vector<AttachmentRecord>& records) {
    if (!m_view || records.empty()) return;

    EnsureCurrentGlobalChat();
    std::wstring sessionKey = Utf8ToWide(m_currentChatKey.c_str(), (int)m_currentChatKey.size());
    std::wstring manifest = AttachmentStore::ManifestPath(sessionKey);
    OrangeView::AttachmentViewBlock block;
    block.manifestPath = manifest;
    block.manifestFileUrl = FileUriForPath(manifest);
    block.manifestFolderUrl = FileUriForFolder(manifest);

    for (const auto& rec : records) {
        OrangeView::AttachmentViewItem item;
        item.kind = rec.kind;
        item.name = rec.name;
        item.sizeLabel = std::to_wstring(rec.sizeBytes / 1024) + L" KB";
        item.originalPath = rec.originalPath;
        item.storedPath = rec.storedPath;
        item.thumbnailUrl = rec.thumbnailPath.empty() ? L"" : FileUriForPath(rec.thumbnailPath);
        item.fileUrl = FileUriForPath(rec.storedPath);
        item.folderUrl = FileUriForFolder(rec.storedPath);
        block.items.push_back(std::move(item));
    }
    m_view->AddAttachmentBlock(block);
    SaveChatBlock(L"attachment", sessionKey);
    m_view->ScrollLatestIntoView();
}

bool CMainWindow::PasteClipboardImage() {
    if (!m_view) return false;
    EnsureCurrentGlobalChat();
    if (!OpenClipboard(m_hwnd)) return false;

    std::vector<std::wstring> sourcePaths;
    std::wstring tempPath;
    bool deleteTemp = false;

    if (IsClipboardFormatAvailable(CF_HDROP)) {
        HDROP drop = reinterpret_cast<HDROP>(GetClipboardData(CF_HDROP));
        if (drop) {
            UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                UINT len = DragQueryFileW(drop, i, nullptr, 0);
                if (len == 0) continue;
                std::wstring path(len + 1, L'\0');
                DragQueryFileW(drop, i, path.data(), len + 1);
                path.resize(len);
                sourcePaths.push_back(std::move(path));
            }
        }
    }

    if (sourcePaths.empty()) {
        UINT pngFormat = RegisterClipboardFormatW(L"PNG");
        if (pngFormat != 0 && IsClipboardFormatAvailable(pngFormat)) {
            HANDLE png = GetClipboardData(pngFormat);
            tempPath = TempClipboardImagePath(L".png");
            if (SaveGlobalMemoryToFile(png, tempPath)) {
                sourcePaths.push_back(tempPath);
                deleteTemp = true;
            }
        }
    }

    if (sourcePaths.empty() && IsClipboardFormatAvailable(CF_DIBV5)) {
        HANDLE dib = GetClipboardData(CF_DIBV5);
        tempPath = TempClipboardImagePath(L".bmp");
        if (SaveDibToBmp(dib, tempPath)) {
            sourcePaths.push_back(tempPath);
            deleteTemp = true;
        }
    }

    if (sourcePaths.empty() && IsClipboardFormatAvailable(CF_DIB)) {
        HANDLE dib = GetClipboardData(CF_DIB);
        tempPath = TempClipboardImagePath(L".bmp");
        if (SaveDibToBmp(dib, tempPath)) {
            sourcePaths.push_back(tempPath);
            deleteTemp = true;
        }
    }

    if (sourcePaths.empty()) {
        HBITMAP bitmap = nullptr;
        HANDLE h = GetClipboardData(CF_BITMAP);
        if (h) bitmap = reinterpret_cast<HBITMAP>(h);
        if (bitmap) {
            tempPath = TempClipboardImagePath(L".png");
            if (SaveHBitmapToPng(bitmap, tempPath)) {
                sourcePaths.push_back(tempPath);
                deleteTemp = true;
            }
        }
    }

    CloseClipboard();
    if (sourcePaths.empty()) return false;

    std::vector<AttachmentRecord> records;
    std::wstring sessionKey = Utf8ToWide(m_currentChatKey.c_str(), (int)m_currentChatKey.size());
    for (const auto& path : sourcePaths) {
        AttachmentRecord rec;
        if (AttachmentStore::AddFile(sessionKey, path, &rec)) {
            if (deleteTemp && path == tempPath) {
                rec.originalPath = L"clipboard";
                auto manifestRecords = AttachmentStore::LoadManifest(sessionKey);
                for (auto& item : manifestRecords) {
                    if (item.id == rec.id) {
                        item.originalPath = rec.originalPath;
                        break;
                    }
                }
                AttachmentStore::WriteManifest(sessionKey, manifestRecords);
            }
            Persistence::SaveAttachmentRecord(m_currentChatKey, rec);
            records.push_back(std::move(rec));
        }
    }
    if (deleteTemp && !tempPath.empty()) DeleteFileW(tempPath.c_str());

    if (records.empty()) {
        m_view->NewBlock(L"error");
        m_view->AppendText(L"이미지 붙여넣기 실패\n\n- 클립보드 이미지를 첨부 저장소에 등록하지 못했습니다.");
        m_view->ScrollLatestIntoView();
        return true;
    }

    AppendAttachmentCard(records);
    return true;
}

} // namespace orange
