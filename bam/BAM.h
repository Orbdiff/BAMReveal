#pragma once
#include <string>
#include <vector>

class BAMReader {
public:
    struct Entry {
        std::wstring path;
        std::wstring timestamp;
        bool isSigned;
    };

    std::vector<Entry> GetBAMValues();

private:
    std::wstring ConvertHardDiskVolumeToLetter(const std::wstring& path);
};

bool VerifyFileSignature(const wchar_t* filePath);
bool VerifySignatureForPath(const std::wstring& path);