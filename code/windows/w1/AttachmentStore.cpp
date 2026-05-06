#include "AttachmentStore.h"
#include "Utils.h"

#include <windows.h>
#include <wincodec.h>
#include <fstream>
#include <json/json.h>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace orange {

namespace {

std::wstring AppDataRoot() {
    wchar_t appdata[MAX_PATH] = L"";
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) == 0) return L"";
    return std::wstring(appdata) + L"\\OrangeCode";
}

std::wstring SanitizePathPart(std::wstring v) {
    if (v.empty()) return L"default";
    for (auto& ch : v) {
        const bool safe =
            (ch >= L'a' && ch <= L'z') ||
            (ch >= L'A' && ch <= L'Z') ||
            (ch >= L'0' && ch <= L'9') ||
            ch == L'-' || ch == L'_' || ch == L'.';
        if (!safe) ch = L'_';
    }
    return v;
}

void PutWide(Json::Value& obj, const char* key, const std::wstring& value) {
    obj[key] = WideToUtf8(value);
}

std::wstring GetWide(const Json::Value& obj, const char* key) {
    std::string v = obj.get(key, "").asString();
    return Utf8ToWide(v.c_str(), (int)v.size());
}

std::wstring FileNameOf(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
}

std::wstring ExtensionOf(const std::wstring& path) {
    std::wstring name = FileNameOf(path);
    size_t pos = name.find_last_of(L'.');
    if (pos == std::wstring::npos) return L"";
    std::wstring ext = name.substr(pos);
    for (auto& ch : ext) ch = (wchar_t)towlower(ch);
    return ext;
}

std::wstring MimeFromExtension(const std::wstring& ext) {
    if (ext == L".png") return L"image/png";
    if (ext == L".jpg" || ext == L".jpeg") return L"image/jpeg";
    if (ext == L".gif") return L"image/gif";
    if (ext == L".bmp") return L"image/bmp";
    if (ext == L".webp") return L"image/webp";
    if (ext == L".txt" || ext == L".md" || ext == L".log") return L"text/plain";
    if (ext == L".json") return L"application/json";
    if (ext == L".pdf") return L"application/pdf";
    return L"application/octet-stream";
}

bool IsImageMime(const std::wstring& mime) {
    return mime.rfind(L"image/", 0) == 0;
}

unsigned long long FileSizeBytes(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr)) return 0;
    ULARGE_INTEGER size{};
    size.HighPart = attr.nFileSizeHigh;
    size.LowPart = attr.nFileSizeLow;
    return size.QuadPart;
}

std::wstring NewAttachmentId() {
    static LONG counter = 0;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[80] = L"";
    swprintf_s(buf, L"att_%04u%02u%02u_%02u%02u%02u_%lu_%ld",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
               GetCurrentProcessId(), InterlockedIncrement(&counter));
    return buf;
}

bool CreateImageThumbnail(const std::wstring& sourcePath, const std::wstring& thumbPath) {
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool needUninit = SUCCEEDED(coHr);

    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* outFrame = nullptr;
    IPropertyBag2* props = nullptr;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromFilename(
        sourcePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);

    UINT w = 0, h = 0;
    if (SUCCEEDED(hr)) hr = frame->GetSize(&w, &h);
    UINT tw = w, th = h;
    if (SUCCEEDED(hr) && w > 0 && h > 0) {
        const UINT maxSide = 256;
        if (w >= h && w > maxSide) {
            tw = maxSide;
            th = (UINT)((unsigned long long)h * maxSide / w);
        } else if (h > w && h > maxSide) {
            th = maxSide;
            tw = (UINT)((unsigned long long)w * maxSide / h);
        }
    }

    if (SUCCEEDED(hr)) hr = factory->CreateBitmapScaler(&scaler);
    if (SUCCEEDED(hr)) hr = scaler->Initialize(frame, tw, th, WICBitmapInterpolationModeFant);
    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(thumbPath.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&outFrame, &props);
    if (SUCCEEDED(hr)) hr = outFrame->Initialize(props);
    if (SUCCEEDED(hr)) hr = outFrame->SetSize(tw, th);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = outFrame->SetPixelFormat(&fmt);
    if (SUCCEEDED(hr)) hr = outFrame->WriteSource(scaler, nullptr);
    if (SUCCEEDED(hr)) hr = outFrame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();

    if (props) props->Release();
    if (outFrame) outFrame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (scaler) scaler->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    if (needUninit) CoUninitialize();
    return SUCCEEDED(hr);
}

} // namespace

std::wstring AttachmentStore::RootDir() {
    std::wstring root = AppDataRoot();
    if (root.empty()) return L"";
    CreateDirectoryW(root.c_str(), nullptr);
    std::wstring dir = root + L"\\attachments";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring AttachmentStore::SessionDir(const std::wstring& sessionKey) {
    std::wstring root = RootDir();
    if (root.empty()) return L"";
    return root + L"\\" + SanitizePathPart(sessionKey);
}

std::wstring AttachmentStore::ManifestPath(const std::wstring& sessionKey) {
    std::wstring dir = SessionDir(sessionKey);
    if (dir.empty()) return L"";
    return dir + L"\\manifest.json";
}

bool AttachmentStore::EnsureSessionDir(const std::wstring& sessionKey) {
    std::wstring dir = SessionDir(sessionKey);
    if (dir.empty()) return false;
    if (CreateDirectoryW(dir.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

bool AttachmentStore::WriteManifest(const std::wstring& sessionKey,
                                    const std::vector<AttachmentRecord>& records) {
    if (!EnsureSessionDir(sessionKey)) return false;

    Json::Value root;
    PutWide(root, "session_key", sessionKey.empty() ? L"default" : sessionKey);
    Json::Value items(Json::arrayValue);
    for (const auto& rec : records) {
        Json::Value item;
        PutWide(item, "id", rec.id);
        PutWide(item, "kind", rec.kind);
        PutWide(item, "name", rec.name);
        PutWide(item, "original_path", rec.originalPath);
        PutWide(item, "stored_path", rec.storedPath);
        PutWide(item, "thumbnail_path", rec.thumbnailPath);
        PutWide(item, "mime", rec.mime);
        item["size_bytes"] = Json::UInt64(rec.sizeBytes);
        items.append(item);
    }
    root["attachments"] = items;

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    std::string data = Json::writeString(builder, root);

    std::wstring path = ManifestPath(sessionKey);
    std::ofstream ofs(WideToUtf8(path), std::ios::binary);
    if (!ofs.is_open()) return false;
    ofs.write(data.data(), (std::streamsize)data.size());
    return ofs.good();
}

std::vector<AttachmentRecord> AttachmentStore::LoadManifest(const std::wstring& sessionKey) {
    std::vector<AttachmentRecord> records;
    std::wstring path = ManifestPath(sessionKey);
    std::ifstream ifs(WideToUtf8(path), std::ios::binary);
    if (!ifs.is_open()) return records;

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    if (!Json::parseFromStream(builder, ifs, &root, &errs)) return records;

    const Json::Value items = root["attachments"];
    if (!items.isArray()) return records;
    for (const auto& item : items) {
        AttachmentRecord rec;
        rec.id = GetWide(item, "id");
        rec.kind = GetWide(item, "kind");
        rec.name = GetWide(item, "name");
        rec.originalPath = GetWide(item, "original_path");
        rec.storedPath = GetWide(item, "stored_path");
        rec.thumbnailPath = GetWide(item, "thumbnail_path");
        rec.mime = GetWide(item, "mime");
        rec.sizeBytes = item.get("size_bytes", Json::UInt64(0)).asUInt64();
        if (!rec.id.empty()) records.push_back(std::move(rec));
    }
    return records;
}

bool AttachmentStore::AddFile(const std::wstring& sessionKey,
                              const std::wstring& sourcePath,
                              AttachmentRecord* outRecord) {
    if (sourcePath.empty() || GetFileAttributesW(sourcePath.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    if (!EnsureSessionDir(sessionKey)) return false;

    AttachmentRecord rec;
    rec.id = NewAttachmentId();
    rec.name = FileNameOf(sourcePath);
    rec.originalPath = sourcePath;
    rec.mime = MimeFromExtension(ExtensionOf(sourcePath));
    rec.kind = IsImageMime(rec.mime) ? L"image" : L"file";
    rec.sizeBytes = FileSizeBytes(sourcePath);

    std::wstring itemDir = SessionDir(sessionKey) + L"\\" + rec.id;
    if (!CreateDirectoryW(itemDir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;

    std::wstring ext = ExtensionOf(sourcePath);
    rec.storedPath = itemDir + L"\\original" + ext;
    if (!CopyFileW(sourcePath.c_str(), rec.storedPath.c_str(), FALSE)) return false;

    if (rec.kind == L"image") {
        rec.thumbnailPath = itemDir + L"\\thumb_256.png";
        if (!CreateImageThumbnail(rec.storedPath, rec.thumbnailPath)) {
            rec.thumbnailPath.clear();
        }
    }

    std::vector<AttachmentRecord> records = LoadManifest(sessionKey);
    records.push_back(rec);
    if (!WriteManifest(sessionKey, records)) return false;
    if (outRecord) *outRecord = rec;
    return true;
}

bool AttachmentStore::HasManifest(const std::wstring& sessionKey) {
    std::wstring path = ManifestPath(sessionKey);
    return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool AttachmentStore::DeleteSession(const std::wstring& sessionKey) {
    std::wstring dir = SessionDir(sessionKey);
    std::wstring root = RootDir();
    if (dir.empty() || root.empty()) return false;
    if (dir.size() <= root.size() || dir.rfind(root, 0) != 0) return false;

    std::wstring pattern = dir + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            std::wstring child = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                std::wstring childKey = sessionKey + L"\\" + fd.cFileName;
                std::wstring childPattern = child + L"\\*";
                WIN32_FIND_DATAW cfd{};
                HANDLE ch = FindFirstFileW(childPattern.c_str(), &cfd);
                if (ch != INVALID_HANDLE_VALUE) {
                    do {
                        if (wcscmp(cfd.cFileName, L".") == 0 || wcscmp(cfd.cFileName, L"..") == 0) continue;
                        std::wstring grand = child + L"\\" + cfd.cFileName;
                        if (cfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                            RemoveDirectoryW(grand.c_str());
                        } else {
                            DeleteFileW(grand.c_str());
                        }
                    } while (FindNextFileW(ch, &cfd));
                    FindClose(ch);
                }
                RemoveDirectoryW(child.c_str());
            } else {
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(dir.c_str()) != 0 || GetLastError() == ERROR_FILE_NOT_FOUND;
}

} // namespace orange
