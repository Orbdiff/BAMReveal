#pragma once
#include <windows.h>
#include <vector>
#include <string>

enum class BamSignature
{
    Signed,
    Unsigned,
    NotFound,
    Cheat,
    Fake
};

struct BamReplaceEvent
{
    FILETIME    date;
    std::string reason;
};

struct BamReplace
{
    std::string type;
    FILETIME    startTime;
    FILETIME    endTime;
    ULONGLONG   lastUsn;
    std::vector<BamReplaceEvent> events;
};

struct BAMEntry
{
    std::wstring path;
    FILETIME     lastExecution;
    BamSignature signature;

    std::vector<BamReplace> replaces;
};

using BamResult = std::vector<BAMEntry>;


std::string  WideToUtf8(const std::wstring& w);
std::wstring FileTimeToString(const FILETIME& ft);
BamResult    ReadBAM();