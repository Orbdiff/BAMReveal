#pragma once

#include <windows.h>
#include <string>

inline std::wstring DevicePathToDOSPath(const std::wstring& path)
{
    wchar_t drives[512];
    if (!GetLogicalDriveStringsW(512, drives))
        return path;

    wchar_t volume[MAX_PATH];
    wchar_t drive[] = L" :";

    for (wchar_t* d = drives; *d; d += 4)
    {
        drive[0] = d[0];
        if (!QueryDosDeviceW(drive, volume, MAX_PATH))
            continue;

        if (path.starts_with(volume))
            return std::wstring(1, d[0]) + L":" +
            path.substr(wcslen(volume));

        constexpr auto GR = L"\\\\?\\GLOBALROOT";
        if (path.starts_with(GR))
        {
            auto trimmed = path.substr(wcslen(GR));
            if (trimmed.starts_with(volume))
                return std::wstring(1, d[0]) + L":" +
                trimmed.substr(wcslen(volume));
        }
    }
    return path;
}