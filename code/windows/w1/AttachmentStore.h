#pragma once

#include <string>
#include <vector>

namespace orange {

struct AttachmentRecord {
    std::wstring id;
    std::wstring kind;
    std::wstring name;
    std::wstring originalPath;
    std::wstring storedPath;
    std::wstring thumbnailPath;
    std::wstring mime;
    unsigned long long sizeBytes = 0;
};

class AttachmentStore {
public:
    static std::wstring RootDir();
    static std::wstring SessionDir(const std::wstring& sessionKey);
    static std::wstring ManifestPath(const std::wstring& sessionKey);
    static bool EnsureSessionDir(const std::wstring& sessionKey);
    static bool WriteManifest(const std::wstring& sessionKey,
                              const std::vector<AttachmentRecord>& records);
    static std::vector<AttachmentRecord> LoadManifest(const std::wstring& sessionKey);
    static bool AddFile(const std::wstring& sessionKey,
                        const std::wstring& sourcePath,
                        AttachmentRecord* outRecord);
    static bool HasManifest(const std::wstring& sessionKey);
    static bool DeleteSession(const std::wstring& sessionKey);
};

} // namespace orange
