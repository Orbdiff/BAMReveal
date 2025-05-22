#include "BAMEntries.h"

#include <windows.h>
#include <winioctl.h>
#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <shlobj_core.h>
#include <regex>
#include <unordered_set>
#include <algorithm>
#include <cwchar>
#include <cstdio>
#include <sstream>

inline std::string ToASCII(const std::wstring& wstr) {
    int len = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len == 0) return "";
    std::string str(len, 0);
    WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, &str[0], len, nullptr, nullptr);
    return str;
}

inline std::wstring ToUnicode(const std::string& str) {
    int len = MultiByteToWideChar(CP_ACP, 0, str.c_str(), -1, nullptr, 0);
    if (len == 0) return L"";
    std::wstring wstr(len, 0);
    MultiByteToWideChar(CP_ACP, 0, str.c_str(), -1, &wstr[0], len);
    return wstr;
}

inline std::wstring Utf8ToWstring(const std::string& str) {
    if (str.empty()) return std::wstring();

    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

class File {
public:
    File(HANDLE handle) : handle_(handle) {}
    ~File() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }
    HANDLE Get() const { return handle_; }
private:
    HANDLE handle_;
};

class Volume {
public:
    Volume(HANDLE handle) : handle_(handle) {}
    ~Volume() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }
    HANDLE Get() const { return handle_; }
private:
    HANDLE handle_;
};

class Output {
public:
    static void ErrorExit(const std::wstring& msg) {
        std::wcerr << msg << L" Error code: " << GetLastError() << std::endl;
        exit(1);
    }

    Output(const std::wstring& path) {
        file_.open(ToASCII(path), std::ios::binary);
        if (!file_.is_open()) ErrorExit(L"Failed to open output file.");
    }

    ~Output() {
        if (file_.is_open()) file_.close();
    }

    void Write(const char* data, std::streamsize size) {
        std::lock_guard<std::mutex> lock(mutex_);
        file_.write(data, size);
    }

private:
    std::ofstream file_;
    std::mutex mutex_;
};

class RawCopier {
public:
    int Run(const std::wstring& inputPath, const std::wstring& outputPath) {
        if (inputPath.size() < 3 || inputPath[1] != L':' || inputPath[2] != L'\\') {
            std::wcerr << L"Invalid input path format." << std::endl;
            return 1;
        }

        File inputFile(CreateFileW(inputPath.c_str(), 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL));

        if (inputFile.Get() == INVALID_HANDLE_VALUE) {
            Output::ErrorExit(L"Cannot open input file.");
        }

        std::vector<std::pair<ULONGLONG, ULONGLONG>> clusters;
        ULONGLONG startVCN = 0;

        if (!ReadClusters(inputFile.Get(), clusters, startVCN)) {
            return 1;
        }

        wchar_t drive = towupper(inputPath[0]);
        wchar_t vol[] = L"\\\\.\\X:";
        vol[4] = drive;

        wchar_t root[] = L"X:\\";
        root[0] = drive;

        DWORD spc, bps, freeClusters, totalClusters;
        if (!GetDiskFreeSpaceW(root, &spc, &bps, &freeClusters, &totalClusters)) {
            Output::ErrorExit(L"Failed to retrieve disk info.");
        }

        DWORD clusterSize = spc * bps;
        if (clusterSize > 1024 * 1024 * 1024) {
            std::wcerr << L"Cluster size too large." << std::endl;
            return 1;
        }

        Volume volume(CreateFileW(vol, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
            OPEN_EXISTING, 0, NULL));

        if (volume.Get() == INVALID_HANDLE_VALUE) {
            Output::ErrorExit(L"Cannot open volume.");
        }

        Output out(outputPath);
        std::atomic<int> error = 0;
        std::vector<std::thread> threads;
        size_t threadCount = std::thread::hardware_concurrency();
        if (threadCount == 0) threadCount = 4;

        std::mutex idxMutex;
        size_t idx = 0;

        auto worker = [&](size_t id) {
            while (true) {
                size_t current;
                {
                    std::lock_guard<std::mutex> lock(idxMutex);
                    if (idx >= clusters.size()) break;
                    current = idx++;
                }

                const std::pair<ULONGLONG, ULONGLONG>& extent = clusters[current];
                ULONGLONG count = extent.first;
                ULONGLONG lcn = extent.second;

                std::vector<BYTE> buffer(clusterSize);

                for (ULONGLONG i = 0; i < count; ++i) {
                    LARGE_INTEGER offset;
                    offset.QuadPart = (lcn + i) * clusterSize;

                    if (!SetFilePointerEx(volume.Get(), offset, NULL, FILE_BEGIN)) {
                        std::wcerr << L"Seek error at offset: " << offset.QuadPart << L" - " << GetLastError() << std::endl;
                        error.store(1);
                        return;
                    }

                    DWORD bytesRead = 0;
                    if (!ReadFile(volume.Get(), buffer.data(), clusterSize, &bytesRead, NULL)) {
                        std::wcerr << L"Read error at cluster: " << (lcn + i) << L" - " << GetLastError() << std::endl;
                        error.store(1);
                        return;
                    }

                    if (bytesRead > clusterSize) {
                        std::wcerr << L"Buffer overflow detected." << std::endl;
                        error.store(1);
                        return;
                    }

                    out.Write(reinterpret_cast<const char*>(buffer.data()), bytesRead);
                }
            }
            };

        for (size_t i = 0; i < threadCount; ++i) {
            threads.emplace_back(worker, i);
        }

        for (auto& t : threads) {
            t.join();
        }

        if (error.load()) return 1;

        return 0;
    }

private:
    bool ReadClusters(HANDLE file, std::vector<std::pair<ULONGLONG, ULONGLONG>>& clusters, ULONGLONG& startVCN) {
        BYTE buffer[4096];
        STARTING_VCN_INPUT_BUFFER input = { 0 };
        DWORD returned = 0;

        if (!DeviceIoControl(file, FSCTL_GET_RETRIEVAL_POINTERS,
            &input, sizeof(input),
            buffer, sizeof(buffer),
            &returned, NULL)) {
            std::wcerr << L"Cluster info read failed (FSCTL_GET_RETRIEVAL_POINTERS): " << GetLastError() << std::endl;
            return false;
        }

        auto* ptrs = reinterpret_cast<RETRIEVAL_POINTERS_BUFFER*>(buffer);
        startVCN = ptrs->StartingVcn.QuadPart;

        for (DWORD i = 0; i < ptrs->ExtentCount; ++i) {
            const auto& ext = ptrs->Extents[i];
            if (ext.Lcn.QuadPart == static_cast<ULONGLONG>(-1)) {
                std::wcerr << L"File has sparse clusters." << std::endl;
                return false;
            }

            ULONGLONG prevVCN = (i == 0) ? startVCN : ptrs->Extents[i - 1].NextVcn.QuadPart;
            clusters.emplace_back(ext.NextVcn.QuadPart - prevVCN, ext.Lcn.QuadPart);
        }
        return true;
    }
};

// RAWCOPY CODE ʌ

void CopySystemToTemp() {
    wchar_t* tempDir = nullptr;
    size_t len = 0;
    errno_t err = _wdupenv_s(&tempDir, &len, L"TEMP");
    if (err != 0 || tempDir == nullptr) {
        return;
    }

    std::wstring outputPath = std::wstring(tempDir) + L"\\SYSTEM_backup";

    free(tempDir);

    RawCopier copier;
    int result = copier.Run(L"C:\\Windows\\System32\\config\\SYSTEM", outputPath);

    if (result == 0) {

    }

}

std::vector<BAMReaderaa::Entry> BAMReaderaa::GetBAMValues() {
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
                    std::wstring path(valueName);
                    entries.push_back({ path });
                }
            }

            index++;
        }

        RegCloseKey(hUserKey);
    }

    RegCloseKey(hUserSettingsKey);
    return entries;
}

std::wstring ToLower(const std::wstring& input) {
    std::wstring result = input;
    std::transform(result.begin(), result.end(), result.begin(), ::towlower);
    return result;
}

std::unordered_set<std::wstring> GetBAMPathsSet() {
    std::unordered_set<std::wstring> bamPaths;
    for (const auto& entry : BAMReaderaa::GetBAMValues()) {
        std::wstring converted = BAMReaderaa::ConvertHardDiskVolumeToLetter(entry.path);
        bamPaths.insert(ToLower(converted));
    }
    return bamPaths;
}

std::wstring BAMReaderaa::ConvertHardDiskVolumeToLetter(const std::wstring& path) {
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

std::wstring SearchInFile() {
    std::wstringstream ss;

    CopySystemToTemp();

    wchar_t* tempDir = nullptr;
    size_t len = 0;
    if (_wdupenv_s(&tempDir, &len, L"TEMP") != 0 || tempDir == nullptr) {
        ss << L"[!] Error getting TEMP directory\n";
        return ss.str();
    }

    std::wstring filePath = std::wstring(tempDir) + L"\\SYSTEM_backup";
    free(tempDir);

    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        ss << L"[!] Unable to open SYSTEM_backup\n";
        return ss.str();
    }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    std::wstring wcontent = Utf8ToWstring(content);

    std::unordered_set<std::wstring> bamPaths = GetBAMPathsSet();

    std::wstring searchStr = L"\\Device\\HarddiskVolume";
    std::wstring extension = L".exe";

    size_t pos = 0;
    bool foundAny = false;

    while (true) {
        pos = wcontent.find(searchStr, pos);
        if (pos == std::wstring::npos) break;

        size_t endPos = wcontent.find(extension, pos);
        if (endPos == std::wstring::npos) {
            break;
        }
        endPos += extension.size();

        std::wstring found = wcontent.substr(pos, endPos - pos);
        std::wstring converted = BAMReaderaa::ConvertHardDiskVolumeToLetter(found);
        std::wstring lowerConverted = ToLower(converted);

        if (bamPaths.find(lowerConverted) == bamPaths.end()) {
            ss << converted << L"\n";
            foundAny = true;
        }

        pos = endPos;
    }

    if (!foundAny) {
        ss << L"[!] No BAM Entries Deleted\n";
    }

    return ss.str();
}