#include "bam_ui.h"
#include "../ui/_time_utils.h"

std::vector<BAMEntryUI> ConvertToUI(const BamResult& in)
{
    std::vector<BAMEntryUI> out;
    out.reserve(in.size());

    for (const auto& e : in)
    {
        BAMEntryUI ui{};
        ui.path = WideToUtf8(e.path);
        ui.execTime = FileTimeToTimeT(e.lastExecution);

        std::tm tmStruct{};
        localtime_s(&tmStruct, &ui.execTime);
        std::ostringstream oss;
        oss << std::put_time(&tmStruct, "%Y-%m-%d %H:%M:%S");
        ui.time = oss.str();

        ui.signature = e.signature;
        out.push_back(std::move(ui));
    }

    return out;
}