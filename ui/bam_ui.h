#pragma once
#include "../bam/bam.h"
#include <string>
#include <vector>
#include <ctime>

struct BAMReplaceUI
{
    std::string type;
    std::string startTime;
    std::string endTime;
    std::string lastUsn;

    struct Event
    {
        std::string date;
        std::string reason;
    };
    std::vector<Event> events;
};

struct BAMEntryUI
{
    std::string path;
    time_t execTime;
    std::string time;
    BamSignature signature;
    std::vector<BAMReplaceUI> replaces;
};

std::vector<BAMEntryUI> ConvertToUI(const BamResult& in);
std::string FileTimeToStringUI(const FILETIME& ft);