#pragma once

#include <windows.h>
#include <ntsecapi.h>
#include <string>
#include <ctime>
#include <format>
#include <mutex>
#include <atomic>
#include <lmcons.h> 

inline std::atomic<time_t> cachedLogonTime = 0;
inline std::mutex logonMutex;

std::string FormatUptime(time_t startTime)
{
    time_t now = time(nullptr);
    double seconds = difftime(now, startTime);

    int days = static_cast<int>(seconds / 86400);
    int hours = static_cast<int>((seconds - days * 86400) / 3600);
    int minutes = static_cast<int>((seconds - days * 86400 - hours * 3600) / 60);

    std::string result;
    result.reserve(128);
    if (days > 0) result += std::to_string(days) + (days > 1 ? " days " : " day ");
    if (hours > 0) result += std::to_string(hours) + (hours > 1 ? " hours " : " hour ");
    if (minutes > 0) result += std::to_string(minutes) + (minutes > 1 ? " minutes " : " minute ");
    if (days == 0 && hours == 0 && minutes == 0) result += "a few seconds ";

    struct tm localTime {};
    localtime_s(&localTime, &startTime);
    char buf[64];
    strftime(buf, sizeof(buf), "(%I:%M:%S %p %m/%d/%Y)", &localTime);
    result += buf;

    return result;
}

std::string FormatTime(time_t t)
{
    struct tm localTime {};
    localtime_s(&localTime, &t);
    char buf[64];
    strftime(buf, sizeof(buf), "%I:%M:%S %p %m/%d/%Y", &localTime);
    return std::string(buf);
}

time_t FileTimeToTimeT(const FILETIME& ft)
{
    ULARGE_INTEGER ull;
    ull.LowPart = ft.dwLowDateTime;
    ull.HighPart = ft.dwHighDateTime;
    return static_cast<time_t>((ull.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

time_t GetCurrentUserLogonTime()
{
    time_t cached = cachedLogonTime.load();
    if (cached != 0) return cached;

    std::lock_guard<std::mutex> lock(logonMutex);
    cached = cachedLogonTime.load();
    if (cached != 0) return cached;

    wchar_t username[UNLEN + 1];
    DWORD size = UNLEN + 1;
    if (!GetUserNameW(username, &size))
        return 0;

    ULONG count = 0;
    PLUID sessions = nullptr;
    NTSTATUS status = LsaEnumerateLogonSessions(&count, &sessions);
    if (status != 0 || sessions == nullptr)
        return 0;

    time_t result = 0;

    for (ULONG i = 0; i < count; i++)
    {
        PSECURITY_LOGON_SESSION_DATA pData = nullptr;
        NTSTATUS statusData = LsaGetLogonSessionData(&sessions[i], &pData);
        if (statusData == 0 && pData)
        {
            if (pData->UserName.Buffer &&
                pData->LogonType == Interactive &&
                _wcsicmp(pData->UserName.Buffer, username) == 0)
            {
                FILETIME ft;
                ft.dwLowDateTime = static_cast<DWORD>(pData->LogonTime.LowPart);
                ft.dwHighDateTime = static_cast<DWORD>(pData->LogonTime.HighPart);
                result = FileTimeToTimeT(ft);

                LsaFreeReturnBuffer(pData);
                break;
            }
            LsaFreeReturnBuffer(pData);
        }
    }

    if (sessions)
        LsaFreeReturnBuffer(sessions);

    cachedLogonTime.store(result);
    return result;
}