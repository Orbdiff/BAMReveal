#include "bam.h"

#include <algorithm>
#include <future>
#include <unordered_map>

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
        local.wYear, local.wMonth, local.wDay,
        local.wHour, local.wMinute, local.wSecond);

    return buf;
}

static std::unordered_map<std::wstring, std::vector<BamReplace>>
CollectReplacesByPath(const std::wstring& volume)
{
    USNJournalReader reader(volume);
    auto replaces = reader.Run();

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
    constexpr auto BAM_KEY =
        L"SYSTEM\\CurrentControlSet\\Services\\bam\\State\\UserSettings";

    HKEY hRoot;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, BAM_KEY, 0, KEY_READ, &hRoot))
        return {};

    BamResult out;

    InitGenericRules();
    InitYara();

    auto replacesByPath = CollectReplacesByPath(L"C:");

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

        wchar_t value[1024];
        BYTE data[64];
        DWORD vSize, dSize, type;

        for (DWORD j = 0;; ++j)
        {
            vSize = 1024;
            dSize = sizeof(data);

            if (RegEnumValueW(hSid, j, value, &vSize,
                nullptr, &type, data, &dSize))
                break;

            if (type != REG_BINARY || dSize < sizeof(FILETIME))
                continue;

            std::wstring rawPath = value;
            if (!rawPath.starts_with(L"\\Device\\"))
                continue;

            BAMEntry e{};
            memcpy(&e.lastExecution, data, sizeof(FILETIME));
            e.path = DevicePathToDOSPath(rawPath);
            e.signature = BamSignature::NotFound;

            if (e.path.size() > 2 && e.path[1] == L':')
            {
                auto futureSig = GetSignatureStatusAsync(e.path);
                auto sig = futureSig.get();

                if (sig == SignatureStatus::Signed)
                    e.signature = BamSignature::Signed;
                else if (sig == SignatureStatus::Unsigned)
                    e.signature = BamSignature::Unsigned;
                else if (sig == SignatureStatus::Cheat)
                    e.signature = BamSignature::Cheat;
                else if (sig == SignatureStatus::Fake)
                    e.signature = BamSignature::Fake;

                if (e.signature == BamSignature::Unsigned)
                {
                    std::vector<std::string> yara;
                    if (FastScanFile(WideToUtf8(e.path), yara))
                        e.signature = BamSignature::Cheat;
                }
            }

            auto it = replacesByPath.find(e.path);
            if (it != replacesByPath.end())
            {
                e.replaces = it->second;
            }

            out.emplace_back(std::move(e));
        }

        RegCloseKey(hSid);
    }

    RegCloseKey(hRoot);
    FinalizeYara();

    std::sort(out.begin(), out.end(),
        [](const BAMEntry& a, const BAMEntry& b)
        {
            return CompareFileTime(&a.lastExecution, &b.lastExecution) > 0;
        });

    return out;
}