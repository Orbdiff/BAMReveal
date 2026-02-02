#pragma once

#include <windows.h>
#include <string>
#include <cwchar>
#include <unordered_map>
#include <string_view>

inline std::wstring DevicePathToDOSPath(const std::wstring& path)
{
    constexpr std::wstring_view GR = L"\\\\?\\GLOBALROOT";

    static std::unordered_map<std::wstring, wchar_t> volumeToDrive;
    static bool cacheInitialized = false;

    if (!cacheInitialized) {
        wchar_t drives[512];
        if (GetLogicalDriveStringsW(ARRAYSIZE(drives), drives)) {
            wchar_t volume[MAX_PATH];
            for (const wchar_t* d = drives; *d; d += 4) {
                const wchar_t driveStr[] = { d[0], L':', L'\0' };
                if (QueryDosDeviceW(driveStr, volume, ARRAYSIZE(volume))) {
                    volumeToDrive[volume] = d[0];
                }
            }
        }
        cacheInitialized = true;
    }

    std::wstring_view pathView = path;

    for (const auto& pair : volumeToDrive) {
        std::wstring_view volumeView = pair.first;
        if (pathView.starts_with(volumeView) &&
            _wcsnicmp(pathView.data(), volumeView.data(), volumeView.size()) == 0) {
            return std::wstring(1, pair.second) + L":" +
                std::wstring(pathView.substr(volumeView.size()));
        }
    }

    if (pathView.starts_with(GR)) {
        std::wstring_view trimmedView = pathView.substr(GR.size());
        for (const auto& pair : volumeToDrive) {
            std::wstring_view volumeView = pair.first;
            if (trimmedView.starts_with(volumeView) &&
                _wcsnicmp(trimmedView.data(), volumeView.data(), volumeView.size()) == 0) {
                return std::wstring(1, pair.second) + L":" +
                    std::wstring(trimmedView.substr(volumeView.size()));
            }
        }
    }

    return path;
}