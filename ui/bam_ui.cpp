#include "bam_ui.h"
#include "../ui/_time_utils.h"

#include <sstream>
#include <iomanip>

std::string FileTimeToStringUI(const FILETIME& ft)
{
    SYSTEMTIME utc{}, local{};
    FileTimeToSystemTime(&ft, &utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);

    std::ostringstream oss;
    oss << std::setfill('0')
        << std::setw(4) << local.wYear << "-"
        << std::setw(2) << local.wMonth << "-"
        << std::setw(2) << local.wDay << " "
        << std::setw(2) << local.wHour << ":"
        << std::setw(2) << local.wMinute << ":"
        << std::setw(2) << local.wSecond;

    return oss.str();
}

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

        for (const auto& r : e.replaces)
        {
            BAMReplaceUI rui{};
            rui.type = r.type;
            rui.startTime = FileTimeToStringUI(r.startTime);
            rui.endTime = FileTimeToStringUI(r.endTime);
            rui.lastUsn = std::to_string(r.lastUsn);

            for (const auto& ev : r.events)
            {
                rui.events.push_back({
                    FileTimeToStringUI(ev.date),
                    ev.reason
                    });
            }

            ui.replaces.push_back(std::move(rui));
        }

        out.push_back(std::move(ui));
    }

    return out;
}