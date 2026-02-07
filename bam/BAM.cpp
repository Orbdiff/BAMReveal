#include "bam.h"

#include <algorithm>
#include <future>
#include <unordered_map>
#include <ranges>
#include <unordered_set>

#include "../driver_map/_drive_mapper.h"
#include "../signature/_signature_parser.h"
#include "../yara/_yara_scan.hpp"
#include "usn_reader.h"

std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
        nullptr, 0, nullptr, nullptr);

    std::string out(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
        out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring FileTimeToString(const FILETIME& ft)
{
    SYSTEMTIME utc{}, local{};
    FileTimeToSystemTime(&ft, &utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);

    wchar_t buf[64];
    swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d:%02d",
        local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond);

    return buf;
}

static std::unordered_map<std::wstring, std::vector<BamReplace>> CollectReplacesByPath(const std::wstring& volume)
{
    auto replaces = Run(volume);

    std::unordered_map<std::wstring, std::vector<BamReplace>> map;

    for (const auto& r : replaces)
    {
        BamReplace br{};
        br.type = r.type;
        br.startTime = r.startTime;
        br.endTime = r.endTime;
        br.lastUsn = r.lastUsn;

        for (const auto& ev : r.events)
        {
            br.events.push_back({
                ev.date,
                ev.reason
                });
        }

        map[r.fullPath].push_back(std::move(br));
    }

    return map;
}

BamResult ReadBAM()
{
    constexpr auto BAM_KEY = L"SYSTEM\\CurrentControlSet\\Services\\bam\\State\\UserSettings";

    HKEY hRoot;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, BAM_KEY, 0, KEY_READ, &hRoot))
        return {};

    BamResult out;
    out.reserve(10000);

    InitGenericRules();
    InitYara();

    auto replacesByPath = CollectReplacesByPath(L"C:");

    const size_t max_threads = std::max(2u, std::thread::hardware_concurrency() / 2);
    std::vector<std::future<std::vector<BAMEntry>>> futures;

    auto process_sid = [&](const std::vector<BAMEntry>& sidEntries) -> std::vector<BAMEntry> {
        std::vector<BAMEntry> localOut;
        localOut.reserve(sidEntries.size());
        for (const auto& e : sidEntries) {
            BAMEntry localE = e;

            if (localE.path.size() > 2 && localE.path[1] == L':')
            {
                auto futureSig = GetSignatureStatusAsync(localE.path);
                auto sig = futureSig.get();

                if (sig == SignatureStatus::Signed)
                    localE.signature = BamSignature::Signed;
                else if (sig == SignatureStatus::Unsigned)
                    localE.signature = BamSignature::Unsigned;
                else if (sig == SignatureStatus::Cheat)
                    localE.signature = BamSignature::Cheat;
                else if (sig == SignatureStatus::Fake)
                    localE.signature = BamSignature::Fake;

                if (localE.signature == BamSignature::Unsigned)
                {
                    std::vector<std::string> yara;
                    if (FastScanFile(WideToUtf8(localE.path), yara))
                        localE.signature = BamSignature::Cheat;
                }
            }

            auto it = replacesByPath.find(localE.path);
            if (it != replacesByPath.end())
            {
                localE.replaces = it->second;
            }

            localOut.push_back(std::move(localE));
        }
        return localOut;
        };

    std::vector<std::vector<BAMEntry>> sidChunks;
    std::vector<BAMEntry> currentChunk;
    currentChunk.reserve(500);

    wchar_t sid[256];
    DWORD sidSize = 256;

    for (DWORD i = 0;
        RegEnumKeyExW(hRoot, i, sid, &sidSize,
            nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
        ++i, sidSize = 256)
    {
        HKEY hSid;
        std::wstring sidPath = std::wstring(BAM_KEY) + L"\\" + sid;

        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sidPath.c_str(), 0, KEY_READ, &hSid))
            continue;

        for (DWORD j = 0;; ++j)
        {
            wchar_t value[1024];
            BYTE data[64];
            DWORD vSize, dSize, type;

            vSize = 1024;
            dSize = sizeof(data);

            if (RegEnumValueW(hSid, j, value, &vSize,
                nullptr, &type, data, &dSize))
                break;

            if (type != REG_BINARY || dSize < sizeof(FILETIME))
                continue;

            std::wstring rawPath = value;
            if (!rawPath.starts_with(L"\\Device\\") && !rawPath.starts_with(L"Microsoft") && !rawPath.starts_with(L"Windows"))
                continue;

            BAMEntry e{};
            memcpy(&e.lastExecution, data, sizeof(FILETIME));
            e.path = DevicePathToDOSPath(rawPath);
            e.signature = BamSignature::NotFound;

            currentChunk.push_back(std::move(e));

            if (currentChunk.size() >= 50) {
                sidChunks.push_back(std::move(currentChunk));
                currentChunk.clear();
                currentChunk.reserve(50);
            }
        }

        RegCloseKey(hSid);
    }

    if (!currentChunk.empty()) {
        sidChunks.push_back(std::move(currentChunk));
    }

    RegCloseKey(hRoot);

    size_t chunk_size = std::max(size_t(100), sidChunks.size() / max_threads);
    for (const auto& chunk : sidChunks) {
        futures.push_back(std::async(std::launch::async, [process_sid, chunk]() { return process_sid(chunk); }));
    }
    for (auto& f : futures) {
        auto local = f.get();
        out.insert(out.end(), local.begin(), local.end());
    }

    FinalizeYara();

    std::ranges::sort(out, [](const BAMEntry& a, const BAMEntry& b) {
        return CompareFileTime(&a.lastExecution, &b.lastExecution) > 0;
        });

    return out;
}

DeletedBAMEntriesResult GetDeletedBAMEntries(const BamResult& bamResult) {
    std::unordered_set<std::wstring> currentBamPaths;
    for (const auto& entry : bamResult) {
        currentBamPaths.insert(ConvertStringToLowerCase(entry.path));
    }

    return FindDeletedBAMEntriesInSystemHive(currentBamPaths);
}