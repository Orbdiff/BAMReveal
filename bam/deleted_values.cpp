#include "deleted_values.hh"
#include "../driver_map/_drive_mapper.h"

inline std::wstring ConvertStringToLowerCase(const std::wstring& input) {
    std::wstring result = input;
    std::transform(result.begin(), result.end(), result.begin(), ::towlower);
    return result;
}

inline std::wstring ConvertUtf8StringToWstring(std::string_view str) {
    if (str.empty()) return {};
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    std::wstring wstr(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), wstr.data(), size_needed);
    return wstr;
}

bool RetrieveClustersFromFileSequentially(HANDLE fileHandle, std::vector<std::pair<ULONGLONG, ULONGLONG>>& outClusters, ULONGLONG& outStartVCN) {
    BYTE buffer[4096];
    STARTING_VCN_INPUT_BUFFER inputBuffer = { 0 };
    DWORD returnedBytes = 0;

    if (!DeviceIoControl(fileHandle, FSCTL_GET_RETRIEVAL_POINTERS, &inputBuffer, sizeof(inputBuffer), buffer, sizeof(buffer), &returnedBytes, nullptr)) {
        return false;
    }

    auto* ptrs = reinterpret_cast<RETRIEVAL_POINTERS_BUFFER*>(buffer);
    outStartVCN = ptrs->StartingVcn.QuadPart;

    outClusters.reserve(ptrs->ExtentCount);
    for (DWORD i = 0; i < ptrs->ExtentCount; ++i) {
        const auto& extent = ptrs->Extents[i];
        if (extent.Lcn.QuadPart == static_cast<ULONGLONG>(-1)) return false;
        ULONGLONG previousVCN = (i == 0) ? outStartVCN : ptrs->Extents[i - 1].NextVcn.QuadPart;
        outClusters.emplace_back(extent.NextVcn.QuadPart - previousVCN, extent.Lcn.QuadPart);
    }

    return true;
}

bool CopyFileRawDataIntoMemorySequentially(const std::wstring& filePath, std::vector<BYTE>& outBuffer) {
    UniqueHandle inputFile(CreateFileW(filePath.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!inputFile) return false;

    std::vector<std::pair<ULONGLONG, ULONGLONG>> clusters;
    ULONGLONG startVCN = 0;
    if (!RetrieveClustersFromFileSequentially(inputFile.get(), clusters, startVCN)) return false;

    wchar_t driveLetter = towupper(filePath[0]);
    WCHAR volumePath[] = L"\\\\.\\X:";
    volumePath[4] = driveLetter;
    WCHAR rootPath[] = L"X:\\";
    rootPath[0] = driveLetter;

    DWORD sectorsPerCluster, bytesPerSector, freeClusters, totalClusters;
    if (!GetDiskFreeSpaceW(rootPath, &sectorsPerCluster, &bytesPerSector, &freeClusters, &totalClusters)) return false;

    DWORD clusterSize = sectorsPerCluster * bytesPerSector;
    UniqueHandle volume(CreateFileW(volumePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!volume) return false;

    std::vector<BYTE> localBuffer(clusterSize);
    outBuffer.reserve(clusters.size() * clusterSize);

    for (const auto& extent : clusters) {
        ULONGLONG count = extent.first;
        ULONGLONG lcn = extent.second;

        for (ULONGLONG i = 0; i < count; ++i) {
            LARGE_INTEGER offset;
            offset.QuadPart = (lcn + i) * clusterSize;

            if (!SetFilePointerEx(volume.get(), offset, nullptr, FILE_BEGIN)) return false;

            DWORD bytesRead = 0;
            if (!ReadFile(volume.get(), localBuffer.data(), clusterSize, &bytesRead, nullptr)) return false;

            outBuffer.insert(outBuffer.end(), localBuffer.begin(), localBuffer.begin() + bytesRead);
        }
    }

    return true;
}

DeletedBAMEntriesResult FindDeletedBAMEntriesInSystemHive(const std::unordered_set<std::wstring>& currentBamPathsLowerCase) {
    DeletedBAMEntriesResult result;
    std::vector<BYTE> systemHiveData;

    if (!CopyFileRawDataIntoMemorySequentially(L"C:\\Windows\\System32\\config\\SYSTEM", systemHiveData)) {
        return result;
    }

    std::string content(systemHiveData.begin(), systemHiveData.end());

    size_t pos = 0;
    while (pos < content.size()) {
        size_t chunkEnd = std::min(pos + BUFFER_SIZE, content.size());
        std::string_view chunk(&content[pos], chunkEnd - pos);

        size_t localPos = 0;
        while ((localPos = chunk.find(SEARCH_STRING, localPos)) != std::string_view::npos) {
            size_t endPos = chunk.find(EXTENSION_STRING, localPos);
            if (endPos == std::string_view::npos) break;
            endPos += EXTENSION_STRING.size();

            std::string foundPathStr(chunk.substr(localPos, endPos - localPos));
            std::wstring foundPath = ConvertUtf8StringToWstring(foundPathStr);
            std::wstring convertedPath = DevicePathToDOSPath(foundPath);
            std::wstring lowerPath = ConvertStringToLowerCase(convertedPath);
            if (currentBamPathsLowerCase.find(lowerPath) == currentBamPathsLowerCase.end()) {
                result.deletedPaths.push_back(std::move(convertedPath));
            }
            localPos = endPos;
        }
        pos = chunkEnd;
    }

    return result;
}