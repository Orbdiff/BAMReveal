#pragma once
#include <windows.h>
#include <aclapi.h>
#include <string>
#include <vector>
#include <stack>
#include <memory>
#include <optional>

struct DeniedRegistryEntry
{
    std::wstring keyPath;
    std::wstring deniedPermission;
};

struct RegistryNode {
    HKEY hParent;
    std::wstring path;
};

struct HKeyDeleter {
    void operator()(HKEY hKey) const {
        if (hKey) RegCloseKey(hKey);
    }
};

struct SecurityDescriptorDeleter {
    void operator()(PSECURITY_DESCRIPTOR pSD) const {
        if (pSD) LocalFree(pSD);
    }
};

using UniqueHKey = std::unique_ptr<std::remove_pointer_t<HKEY>, HKeyDeleter>;
using UniqueSecurityDescriptor = std::unique_ptr<std::remove_pointer_t<PSECURITY_DESCRIPTOR>, SecurityDescriptorDeleter>;

std::wstring MaskToString(DWORD mask)
{
    const DWORD KEY_READ_MASK = KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS | KEY_NOTIFY;
    const DWORD KEY_WRITE_MASK = KEY_SET_VALUE | KEY_CREATE_SUB_KEY;
    const DWORD KEY_EXECUTE_MASK = KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS | KEY_NOTIFY;
    const DWORD KEY_FULL_CONTROL_MASK = KEY_READ_MASK | KEY_WRITE_MASK | KEY_CREATE_LINK | DELETE;

    if ((mask & KEY_FULL_CONTROL_MASK) == KEY_FULL_CONTROL_MASK) {
        return L"FullControl";
    }
    if ((mask & KEY_READ_MASK) == KEY_READ_MASK) {
        return L"Read";
    }
    if ((mask & KEY_WRITE_MASK) == KEY_WRITE_MASK) {
        return L"Write";
    }
    if ((mask & KEY_EXECUTE_MASK) == KEY_EXECUTE_MASK) {
        return L"Execute";
    }
    if (mask & DELETE) {
        return L"Delete";
    }
    if (mask & KEY_CREATE_LINK) {
        return L"CreateLink";
    }

    if (mask != 0) {
        WCHAR buffer[16];
        swprintf_s(buffer, L"0x%08X", mask);
        return buffer;
    }
    return L"";
}

void CheckDeniedPermissions(HKEY hKey, const std::wstring& path, std::vector<DeniedRegistryEntry>& results)
{
    PSECURITY_DESCRIPTOR pSD = nullptr;
    if (GetSecurityInfo(hKey, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr, &pSD) != ERROR_SUCCESS) {
        return;
    }
    UniqueSecurityDescriptor uniqueSD(pSD);

    PACL pDACL = nullptr;
    BOOL daclPresent = FALSE;
    BOOL daclDefaulted = FALSE;

    if (!GetSecurityDescriptorDacl(pSD, &daclPresent, &pDACL, &daclDefaulted) || !daclPresent || !pDACL) {
        return;
    }

    for (DWORD i = 0; i < pDACL->AceCount; ++i) {
        LPVOID pAce = nullptr;
        if (GetAce(pDACL, i, &pAce)) {
            ACE_HEADER* aceHeader = static_cast<ACE_HEADER*>(pAce);
            if (aceHeader->AceType == ACCESS_DENIED_ACE_TYPE) {
                ACCESS_DENIED_ACE* denyAce = static_cast<ACCESS_DENIED_ACE*>(pAce);
                std::wstring perm = MaskToString(denyAce->Mask);
                if (!perm.empty()) {
                    results.push_back({ path, perm });
                }
            }
        }
    }
}

void TraverseRegistry(HKEY hRoot, const std::wstring& rootPath, std::vector<DeniedRegistryEntry>& results)
{
    std::stack<RegistryNode> stack;
    stack.push({ hRoot, rootPath });

    while (!stack.empty()) {
        RegistryNode node = stack.top();
        stack.pop();

        HKEY hKey = nullptr;
        if (RegOpenKeyExW(node.hParent, node.path.c_str(), 0, KEY_READ | READ_CONTROL, &hKey) != ERROR_SUCCESS) {
            continue;
        }
        UniqueHKey uniqueKey(hKey);

        CheckDeniedPermissions(hKey, node.path, results);

        DWORD index = 0;
        WCHAR subKeyName[256];
        DWORD nameSize = _countof(subKeyName);

        while (RegEnumKeyExW(hKey, index, subKeyName, &nameSize, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            std::wstring subPath = node.path + L"\\" + subKeyName;
            stack.push({ hRoot, subPath });
            index++;
            nameSize = _countof(subKeyName);
        }
    }
}

std::vector<DeniedRegistryEntry> GetDeniedBAMEntries()
{
    std::vector<DeniedRegistryEntry> results;
    TraverseRegistry(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\bam", results);
    return results;
}