#pragma once
#include <windows.h>
#include <aclapi.h>
#include <string>
#include <vector>

struct DeniedRegistryEntry
{
    std::wstring keyPath;
    std::wstring deniedPermission;
};

std::wstring MaskToString(DWORD mask)
{
    DWORD fullControlMask = KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_CREATE_SUB_KEY |
        KEY_ENUMERATE_SUB_KEYS | KEY_NOTIFY | KEY_CREATE_LINK | DELETE;

    if ((mask & fullControlMask) == fullControlMask)
        return L"FullControl";

    if (mask & KEY_QUERY_VALUE)
        return L"Read";

    return L"";
}

void CheckDeniedPermissions(HKEY hKey, const std::wstring& path, std::vector<DeniedRegistryEntry>& results)
{
    PSECURITY_DESCRIPTOR pSD = nullptr;
    if (GetSecurityInfo(hKey, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr, &pSD) != ERROR_SUCCESS)
        return;

    PACL pDACL = nullptr;
    BOOL daclPresent = FALSE;
    BOOL daclDefaulted = FALSE;

    if (GetSecurityDescriptorDacl(pSD, &daclPresent, &pDACL, &daclDefaulted) && daclPresent && pDACL) {
        for (DWORD i = 0; i < pDACL->AceCount; ++i) {
            LPVOID pAce = nullptr;
            if (GetAce(pDACL, i, &pAce)) {
                auto aceHeader = &reinterpret_cast<ACE_HEADER*>(pAce)[0];
                if (aceHeader->AceType == ACCESS_DENIED_ACE_TYPE) {
                    auto denyAce = reinterpret_cast<ACCESS_DENIED_ACE*>(pAce);
                    std::wstring perm = MaskToString(denyAce->Mask);
                    if (!perm.empty()) {
                        results.push_back({ path, perm });
                    }
                }
            }
        }
    }

    LocalFree(pSD);
}

void TraverseRegistry(HKEY hParent, const std::wstring& path, std::vector<DeniedRegistryEntry>& results)
{
    HKEY hKey;
    if (RegOpenKeyExW(hParent, path.c_str(), 0, KEY_READ | READ_CONTROL, &hKey) != ERROR_SUCCESS)
        return;

    CheckDeniedPermissions(hKey, path, results);

    DWORD index = 0;
    WCHAR subKeyName[256];
    DWORD nameSize = _countof(subKeyName);

    while (RegEnumKeyExW(hKey, index, subKeyName, &nameSize, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        std::wstring subPath = path + L"\\" + subKeyName;
        TraverseRegistry(hParent, subPath, results);
        index++;
        nameSize = _countof(subKeyName);
    }

    RegCloseKey(hKey);
}

std::vector<DeniedRegistryEntry> GetDeniedBAMEntries()
{
    std::vector<DeniedRegistryEntry> results;
    TraverseRegistry(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\bam", results);
    return results;
}