#pragma once
#include <string>
#include <iomanip>
#include <sstream>
#include <vector>
#include "../bam/bam.h"

struct BAMEntryUI
{
    std::string path;
    std::string time;
    BamSignature signature;
    time_t execTime;
};

std::vector<BAMEntryUI> ConvertToUI(const BamResult& in);