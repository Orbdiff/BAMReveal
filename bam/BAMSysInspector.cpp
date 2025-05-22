#include "BamSysInspector.h"

#include <sstream>
#include <iomanip>

std::wstring GetSystemBootTimeText() {
    std::wstringstream ss;

    ULONGLONG uptimeMs = GetTickCount64();
    FILETIME currentTime;
    GetSystemTimeAsFileTime(&currentTime);

    ULONGLONG currentTime64 = ((ULONGLONG)currentTime.dwHighDateTime << 32) | currentTime.dwLowDateTime;
    ULONGLONG bootTime64 = currentTime64 - (uptimeMs * 10000);

    FILETIME bootTime;
    bootTime.dwLowDateTime = (DWORD)bootTime64;
    bootTime.dwHighDateTime = (DWORD)(bootTime64 >> 32);

    SYSTEMTIME stUTC, stLocal;
    FileTimeToSystemTime(&bootTime, &stUTC);
    SystemTimeToTzSpecificLocalTime(NULL, &stUTC, &stLocal);

    ss << L"Logon time of the PC: " << stLocal.wYear << L"/" << stLocal.wMonth << L"/" << stLocal.wDay
        << L" " << stLocal.wHour << L":" << stLocal.wMinute << L":" << stLocal.wSecond << L"\n";

    return ss.str();
}

std::wstring GetCreationTimeText(LARGE_INTEGER time) {
    std::wstringstream ss;
    if (time.QuadPart == 0) {
        ss << L"Creation date: [Unavailable]\n";
        return ss.str();
    }

    FILETIME ft;
    SYSTEMTIME stUTC, stLocal;

    ft.dwLowDateTime = time.LowPart;
    ft.dwHighDateTime = time.HighPart;

    FileTimeToSystemTime(&ft, &stUTC);
    SystemTimeToTzSpecificLocalTime(NULL, &stUTC, &stLocal);

    ss << L"Creation date: " << stLocal.wYear << L"/" << stLocal.wMonth << L"/" << stLocal.wDay
        << L" " << stLocal.wHour << L":" << stLocal.wMinute << L":" << stLocal.wSecond << L"\n";

    return ss.str();
}

DWORD GetSystemProcessId() {
    ULONG size = 0;
    NtQuerySystemInformation(5, NULL, 0, &size);
    std::vector<BYTE> buffer(size);

    NTSTATUS status = NtQuerySystemInformation(5, buffer.data(), size, &size);
    if (!NT_SUCCESS(status)) {
        return 4;
    }

    PSYSTEM_PROCESS_INFORMATION procInfo = reinterpret_cast<PSYSTEM_PROCESS_INFORMATION>(buffer.data());

    while (procInfo) {
        if (procInfo->ImageName.Buffer && wcsstr(procInfo->ImageName.Buffer, L"System")) {
            return (DWORD)(ULONG_PTR)procInfo->UniqueProcessId;
        }

        if (!procInfo->NextEntryOffset) break;
        procInfo = reinterpret_cast<PSYSTEM_PROCESS_INFORMATION>(
            reinterpret_cast<BYTE*>(procInfo) + procInfo->NextEntryOffset);
    }

    return 4;
}

PVOID GetBamSysBaseAddress() {
    LPVOID drivers[1024];
    DWORD cbNeeded;

    if (EnumDeviceDrivers(drivers, sizeof(drivers), &cbNeeded)) {
        for (int i = 0; i < (cbNeeded / sizeof(LPVOID)); i++) {
            WCHAR driverName[MAX_PATH];
            if (GetDeviceDriverBaseNameW(drivers[i], driverName, MAX_PATH)) {
                if (wcsstr(driverName, L"bam.sys")) {
                    return drivers[i];
                }
            }
        }
    }
    return nullptr;
}

std::wstring FindBamSysThreadText() {
    std::wstringstream ss;

    PVOID bamBase = GetBamSysBaseAddress();
    if (!bamBase) {
        ss << L"[!] bam.sys not found in memory\n";
        return ss.str();
    }

    DWORD systemPid = GetSystemProcessId();

    ULONG size = 0;
    NtQuerySystemInformation(5, NULL, 0, &size);
    std::vector<BYTE> buffer(size);

    NTSTATUS status = NtQuerySystemInformation(5, buffer.data(), size, &size);
    if (!NT_SUCCESS(status)) {
        ss << L"Failed to retrieve system information\n";
        return ss.str();
    }

    ULONGLONG uptimeMs = GetTickCount64();
    FILETIME currentTime;
    GetSystemTimeAsFileTime(&currentTime);
    ULONGLONG currentTime64 = ((ULONGLONG)currentTime.dwHighDateTime << 32) | currentTime.dwLowDateTime;
    ULONGLONG bootTime64 = currentTime64 - (uptimeMs * 10000);

    PSYSTEM_PROCESS_INFORMATION procInfo = reinterpret_cast<PSYSTEM_PROCESS_INFORMATION>(buffer.data());

    while (procInfo) {
        if ((DWORD)(ULONG_PTR)procInfo->UniqueProcessId == systemPid) {
            for (ULONG i = 0; i < procInfo->NumberOfThreads; i++) {
                SYSTEM_THREAD_INFORMATION& threadInfo = procInfo->Threads[i];

                if (threadInfo.StartAddress >= bamBase && threadInfo.StartAddress < (PBYTE)bamBase + 0x100000) {
                    ss << L"Start Address: 0x"
                        << std::hex << reinterpret_cast<uintptr_t>(threadInfo.StartAddress)
                        << std::dec << L"\n\n";

                    ss << GetCreationTimeText(threadInfo.CreateTime);

                    ULONGLONG threadCreateTime64 = ((ULONGLONG)threadInfo.CreateTime.HighPart << 32) | threadInfo.CreateTime.LowPart;

                    if (threadCreateTime64 != 0) {
                        ULONGLONG diffFromBoot = (threadCreateTime64 - bootTime64) / 10000000ULL;
                        ULONGLONG threadAge = (currentTime64 - threadCreateTime64) / 10000000ULL;

                        ss << L"Time after boot: " << diffFromBoot << L" seconds\n";
                        ss << L"Thread alive since: " << threadAge << L" seconds\n";
                    }

                    return ss.str();    
                }
            }
        }

        if (!procInfo->NextEntryOffset) break;
        procInfo = reinterpret_cast<PSYSTEM_PROCESS_INFORMATION>(reinterpret_cast<BYTE*>(procInfo) + procInfo->NextEntryOffset);
    }

    ss << L"[!] No bam.sys threads found\n";
    return ss.str();
}