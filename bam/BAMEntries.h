#pragma once
#include <string>
#include <vector>

class BAMReaderaa {
public:
    struct Entry {
        std::wstring path;
    };

    static std::vector<Entry> GetBAMValues();
    static std::wstring ConvertHardDiskVolumeToLetter(const std::wstring& path);
    static std::wstring SearchInFile();
};

std::wstring SearchInFile();