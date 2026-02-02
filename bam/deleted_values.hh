#pragma once
#include <windows.h>
#include <winioctl.h>
#include <vector>
#include <string>
#include <unordered_set>
#include <algorithm>
#include <cwchar>
#include <optional>
#include <memory>
#include <string_view>

struct HandleDeleter {
    void operator()(HANDLE handle) const {
        if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
};
using UniqueHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleDeleter>;

constexpr DWORD BUFFER_SIZE = 16 * 1024 * 1024 ;
constexpr std::string_view SEARCH_STRING = "\\Device\\HarddiskVolume";
constexpr std::string_view EXTENSION_STRING = ".exe";

inline std::wstring ConvertStringToLowerCase(const std::wstring& input);
inline std::wstring ConvertUtf8StringToWstring(std::string_view str);
bool RetrieveClustersFromFileSequentially(HANDLE fileHandle, std::vector<std::pair<ULONGLONG, ULONGLONG>>& outClusters, ULONGLONG& outStartVCN);
bool CopyFileRawDataIntoMemorySequentially(const std::wstring& filePath, std::vector<BYTE>& outBuffer);


struct DeletedBAMEntriesResult {
    std::vector<std::wstring> deletedPaths;
};

DeletedBAMEntriesResult FindDeletedBAMEntriesInSystemHive(const std::unordered_set<std::wstring>& currentBamPathsLowerCase);