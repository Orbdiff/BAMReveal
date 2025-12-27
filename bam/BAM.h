#pragma once
#include <windows.h>
#include <vector>
#include <string>

enum class BamSignature
{
    Signed,
    Unsigned,
    NotFound,
    Cheat
};

struct BAMEntry
{
    std::wstring path;
    FILETIME     lastExecution;
    BamSignature signature;
};

using BamResult = std::vector<BAMEntry>;

std::string WideToUtf8(const std::wstring& w);
std::wstring FileTimeToString(const FILETIME& ft);
BamResult ReadBAM();