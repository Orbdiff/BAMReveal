#include "BAM.h"

#include <windows.h>
#include <sddl.h>
#include <string>
#include <vector>
#include <iostream>
#include <wintrust.h>
#include <unordered_map>
#include <softpub.h>

#pragma comment (lib, "wintrust.lib")

static std::unordered_map<std::wstring, bool> signatureCache;

std::wstring FileTimeToWString(const FILETIME& ft) {
    SYSTEMTIME stUTC, stLocal;
    FileTimeToSystemTime(&ft, &stUTC);
    SystemTimeToTzSpecificLocalTime(NULL, &stUTC, &stLocal);

    wchar_t buffer[100];
    swprintf(buffer, 100, L"%02d/%02d/%04d %02d:%02d:%02d",
        stLocal.wDay, stLocal.wMonth, stLocal.wYear,
        stLocal.wHour, stLocal.wMinute, stLocal.wSecond);
    return buffer;
}

std::wstring BAMReader::ConvertHardDiskVolumeToLetter(const std::wstring& path) {
    wchar_t drives[MAX_PATH];
    if (GetLogicalDriveStringsW(MAX_PATH, drives)) {
        wchar_t volumeName[MAX_PATH];
        wchar_t driveLetter[] = L" :";

        for (wchar_t* drive = drives; *drive; drive += 4) {
            driveLetter[0] = drive[0];
            if (QueryDosDeviceW(driveLetter, volumeName, MAX_PATH)) {
                std::wstring volPath = path;
                std::wstring volName = volumeName;

                if (volPath.find(volName) == 0) {
                    return std::wstring(1, drive[0]) + L":" + volPath.substr(volName.length());
                }

                std::wstring globalRootPrefix = L"\\\\?\\GLOBALROOT";
                if (volPath.find(globalRootPrefix) == 0) {
                    volPath = volPath.substr(globalRootPrefix.length());
                    if (volPath.find(volName) == 0) {
                        return std::wstring(1, drive[0]) + L":" + volPath.substr(volName.length());
                    }
                }
            }
        }
    }
    return path;
}

// Signature Structure
bool VerifyFileSignature(const wchar_t* filePath) {
    WINTRUST_FILE_INFO fileInfo = { 0 };
    fileInfo.cbStruct = sizeof(WINTRUST_FILE_INFO);
    fileInfo.pcwszFilePath = filePath;
    fileInfo.hFile = NULL;
    fileInfo.pgKnownSubject = NULL;

    WINTRUST_DATA winTrustData = { 0 };
    winTrustData.cbStruct = sizeof(WINTRUST_DATA);
    winTrustData.dwUIChoice = WTD_UI_NONE;
    winTrustData.fdwRevocationChecks = WTD_REVOKE_NONE;
    winTrustData.dwUnionChoice = WTD_CHOICE_FILE;
    winTrustData.dwStateAction = WTD_STATEACTION_IGNORE;
    winTrustData.hWVTStateData = NULL;
    winTrustData.pFile = &fileInfo;

    GUID policyGUID = WINTRUST_ACTION_GENERIC_VERIFY_V2;

    LONG status = WinVerifyTrust(NULL, &policyGUID, &winTrustData);

    return status == ERROR_SUCCESS;
}

bool VerifySignatureForPath(const std::wstring& path) {
    auto it = signatureCache.find(path);
    if (it != signatureCache.end()) {
        return it->second;
    }

    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        signatureCache[path] = false;
        return false;
    }

    bool isValid = VerifyFileSignature(path.c_str());
    signatureCache[path] = isValid;
    return isValid;
}

std::vector<BAMReader::Entry> BAMReader::GetBAMValues() {
    std::vector<Entry> entries;

    HKEY hUserSettingsKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Services\\bam\\State\\UserSettings",
        0, KEY_READ, &hUserSettingsKey) != ERROR_SUCCESS)
    {
        return entries;
    }

    DWORD subKeyCount = 0;
    DWORD maxSubKeyLen = 0;
    if (RegQueryInfoKeyW(hUserSettingsKey, NULL, NULL, NULL,
        &subKeyCount, &maxSubKeyLen, NULL, NULL, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
    {
        RegCloseKey(hUserSettingsKey);
        return entries;
    }

    maxSubKeyLen++;
    std::vector<WCHAR> subKeyName(maxSubKeyLen);

    for (DWORD i = 0; i < subKeyCount; ++i) {
        DWORD subKeyNameLen = maxSubKeyLen;
        if (RegEnumKeyExW(hUserSettingsKey, i, subKeyName.data(), &subKeyNameLen,
            NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
        {
            continue;
        }

        std::wstring fullSubKeyPath = L"SYSTEM\\CurrentControlSet\\Services\\bam\\State\\UserSettings\\" + std::wstring(subKeyName.data());
        HKEY hUserKey;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, fullSubKeyPath.c_str(), 0, KEY_READ, &hUserKey) != ERROR_SUCCESS)
            continue;

        DWORD index = 0;
        WCHAR valueName[512];
        DWORD valueNameSize;
        BYTE data[64];
        DWORD dataSize;
        DWORD type;

        while (true) {
            valueNameSize = 512;
            dataSize = sizeof(data);

            LONG result = RegEnumValueW(hUserKey, index, valueName, &valueNameSize,
                NULL, &type, data, &dataSize);

            if (result == ERROR_NO_MORE_ITEMS)
                break;

            if (result == ERROR_SUCCESS && type == REG_BINARY && dataSize >= sizeof(FILETIME)) {
                if (wcsncmp(valueName, L"\\Device", 7) == 0) {
                    FILETIME ft;
                    memcpy(&ft, data, sizeof(FILETIME));

                    std::wstring path(valueName);
                    std::wstring convertedPath = ConvertHardDiskVolumeToLetter(path);

                    bool isSigned = false;
                    if (!convertedPath.empty() && convertedPath[1] == L':') {
                        isSigned = VerifySignatureForPath(convertedPath);
                    }

                    entries.push_back({ convertedPath, FileTimeToWString(ft), isSigned });
                }
            }

            index++;
        }

        RegCloseKey(hUserKey);
    }

    RegCloseKey(hUserSettingsKey);
    return entries;
}