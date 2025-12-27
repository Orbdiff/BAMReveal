#include <windows.h>
#include <winioctl.h>
#include <vector>
#include <string>
#include <unordered_set>
#include <algorithm>
#include <cwchar>

inline std::wstring ConvertStringToLowerCase(const std::wstring& input) {
    std::wstring result = input;
    std::transform(result.begin(), result.end(), result.begin(), ::towlower);
    return result;
}

inline std::wstring ConvertUtf8StringToWstring(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

struct FileHandleWrapper { HANDLE handle; };
struct VolumeHandleWrapper { HANDLE handle; };

bool RetrieveClustersFromFileSequentially(HANDLE fileHandle, std::vector<std::pair<ULONGLONG, ULONGLONG>>& outClusters, ULONGLONG& outStartVCN) {
    BYTE buffer[4096];
    STARTING_VCN_INPUT_BUFFER inputBuffer = { 0 };
    DWORD returnedBytes = 0;

    if (!DeviceIoControl(fileHandle, FSCTL_GET_RETRIEVAL_POINTERS,
        &inputBuffer, sizeof(inputBuffer),
        buffer, sizeof(buffer),
        &returnedBytes, NULL)) {
        return false;
    }

    auto* ptrs = reinterpret_cast<RETRIEVAL_POINTERS_BUFFER*>(buffer);
    outStartVCN = ptrs->StartingVcn.QuadPart;

    for (DWORD i = 0; i < ptrs->ExtentCount; ++i) {
        const auto& extent = ptrs->Extents[i];
        if (extent.Lcn.QuadPart == static_cast<ULONGLONG>(-1)) return false;
        ULONGLONG previousVCN = (i == 0) ? outStartVCN : ptrs->Extents[i - 1].NextVcn.QuadPart;
        outClusters.emplace_back(extent.NextVcn.QuadPart - previousVCN, extent.Lcn.QuadPart);
    }

    return true;
}

bool CopyFileRawDataIntoMemorySequentially(const std::wstring& filePath, std::vector<BYTE>& outBuffer) {
    FileHandleWrapper inputFile = { CreateFileW(filePath.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL) };

    if (inputFile.handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    std::vector<std::pair<ULONGLONG, ULONGLONG>> clusters;
    ULONGLONG startVCN = 0;
    if (!RetrieveClustersFromFileSequentially(inputFile.handle, clusters, startVCN)) return false;

    wchar_t driveLetter = towupper(filePath[0]);
    wchar_t volumePath[] = L"\\\\.\\X:";
    volumePath[4] = driveLetter;
    wchar_t rootPath[] = L"X:\\";
    rootPath[0] = driveLetter;

    DWORD sectorsPerCluster, bytesPerSector, freeClusters, totalClusters;
    if (!GetDiskFreeSpaceW(rootPath, &sectorsPerCluster, &bytesPerSector, &freeClusters, &totalClusters)) {
        return false;
    }

    DWORD clusterSize = sectorsPerCluster * bytesPerSector;
    VolumeHandleWrapper volume = { CreateFileW(volumePath, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_EXISTING, 0, NULL) };

    if (volume.handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    std::vector<BYTE> localBuffer(clusterSize);

    for (const auto& extent : clusters) {
        ULONGLONG count = extent.first;
        ULONGLONG lcn = extent.second;

        for (ULONGLONG i = 0; i < count; ++i) {
            LARGE_INTEGER offset;
            offset.QuadPart = (lcn + i) * clusterSize;

            if (!SetFilePointerEx(volume.handle, offset, NULL, FILE_BEGIN)) {
                CloseHandle(inputFile.handle);
                CloseHandle(volume.handle);
                return false;
            }

            DWORD bytesRead = 0;
            if (!ReadFile(volume.handle, localBuffer.data(), clusterSize, &bytesRead, NULL)) {
                CloseHandle(inputFile.handle);
                CloseHandle(volume.handle);
                return false;
            }

            outBuffer.insert(outBuffer.end(), localBuffer.begin(), localBuffer.begin() + bytesRead);
        }
    }

    CloseHandle(inputFile.handle);
    CloseHandle(volume.handle);

    return true;
}

struct BAMEntryStruct { std::wstring path; };

std::vector<BAMEntryStruct> RetrieveBAMEntriesFromRegistry() {
    std::vector<BAMEntryStruct> entries;
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Services\\bam\\State\\UserSettings",
        0, KEY_READ, &hKey) != ERROR_SUCCESS) return entries;

    DWORD subKeyCount = 0, maxSubKeyLength = 0;
    RegQueryInfoKeyW(hKey, nullptr, nullptr, nullptr, &subKeyCount, &maxSubKeyLength, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    maxSubKeyLength++;
    std::vector<wchar_t> subKeyName(maxSubKeyLength);

    for (DWORD i = 0; i < subKeyCount; i++) {
        DWORD nameLen = maxSubKeyLength;
        if (RegEnumKeyExW(hKey, i, subKeyName.data(), &nameLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) continue;

        std::wstring fullSubKeyPath = L"SYSTEM\\CurrentControlSet\\Services\\bam\\State\\UserSettings\\" + std::wstring(subKeyName.data());
        HKEY hSubKey;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, fullSubKeyPath.c_str(), 0, KEY_READ, &hSubKey) != ERROR_SUCCESS) continue;

        DWORD valueIndex = 0;
        WCHAR valueName[512];
        BYTE dataBuffer[64];
        DWORD type;

        while (true) {
            DWORD valueNameLen = 512, dataLen = sizeof(dataBuffer);
            LONG r = RegEnumValueW(hSubKey, valueIndex, valueName, &valueNameLen, nullptr, &type, dataBuffer, &dataLen);
            if (r == ERROR_NO_MORE_ITEMS) break;
            if (r == ERROR_SUCCESS && type == REG_BINARY && dataLen >= sizeof(FILETIME)) {
                if (wcsncmp(valueName, L"\\Device", 7) == 0)
                    entries.push_back({ valueName });
            }
            valueIndex++;
        }

        RegCloseKey(hSubKey);
    }

    RegCloseKey(hKey);
    return entries;
}

std::wstring ConvertDevicePathToWindowsDriveLetter(const std::wstring& path) {
    wchar_t drives[MAX_PATH];
    if (GetLogicalDriveStringsW(MAX_PATH, drives)) {
        wchar_t volumeName[MAX_PATH];
        wchar_t driveLetter[] = L" :";
        for (wchar_t* drive = drives; *drive; drive += 4) {
            driveLetter[0] = drive[0];
            if (QueryDosDeviceW(driveLetter, volumeName, MAX_PATH)) {
                std::wstring volPath = path;
                std::wstring volName = volumeName;
                if (volPath.find(volName) == 0)
                    return std::wstring(1, drive[0]) + L":" + volPath.substr(volName.length());

                std::wstring globalRoot = L"\\\\?\\GLOBALROOT";
                if (volPath.find(globalRoot) == 0) {
                    volPath = volPath.substr(globalRoot.length());
                    if (volPath.find(volName) == 0)
                        return std::wstring(1, drive[0]) + L":" + volPath.substr(volName.length());
                }
            }
        }
    }
    return path;
}

std::unordered_set<std::wstring> GetBAMPathsAsLowerCaseSet() {
    std::unordered_set<std::wstring> paths;
    for (auto& entry : RetrieveBAMEntriesFromRegistry()) {
        paths.insert(ConvertStringToLowerCase(ConvertDevicePathToWindowsDriveLetter(entry.path)));
    }
    return paths;
}

struct DeletedBAMEntriesResult {
    std::vector<std::wstring> deletedPaths;
};

DeletedBAMEntriesResult FindDeletedBAMEntriesInSystemHive() {
    DeletedBAMEntriesResult result;
    std::vector<BYTE> systemHiveData;

    if (!CopyFileRawDataIntoMemorySequentially(L"C:\\Windows\\System32\\config\\SYSTEM", systemHiveData)) {
        return result;
    }

    std::string content(systemHiveData.begin(), systemHiveData.end());
    std::wstring wcontent = ConvertUtf8StringToWstring(content);
    std::unordered_set<std::wstring> bamPaths = GetBAMPathsAsLowerCaseSet();

    std::wstring searchString = L"\\Device\\HarddiskVolume";
    std::wstring extensionString = L".exe";

    size_t pos = 0;

    while ((pos = wcontent.find(searchString, pos)) != std::wstring::npos) {
        size_t endPos = wcontent.find(extensionString, pos);
        if (endPos == std::wstring::npos) break;
        endPos += extensionString.size();
        std::wstring foundPath = wcontent.substr(pos, endPos - pos);
        std::wstring convertedPath = ConvertDevicePathToWindowsDriveLetter(foundPath);
        std::wstring lowerPath = ConvertStringToLowerCase(convertedPath);
        if (bamPaths.find(lowerPath) == bamPaths.end()) {
            result.deletedPaths.push_back(convertedPath);
        }
        pos = endPos;
    }

    return result;
}