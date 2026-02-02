#include "bam_ui.h"
#include "../ui/_time_utils.h"
#include "../signature/_signature_parser.h"

#include <sstream>
#include <iomanip>
#include <future>
#include <mutex>

std::string FileTimeToStringUI(const FILETIME& ft)
{
    SYSTEMTIME utc{}, local{};
    FileTimeToSystemTime(&ft, &utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);

    char buffer[41];
    sprintf_s(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d",
        local.wYear, local.wMonth, local.wDay,
        local.wHour, local.wMinute, local.wSecond);
    return std::string(buffer);
}

std::vector<BAMEntryUI> ConvertToUI(const BamResult& in)
{
    std::vector<BAMEntryUI> out;
    out.reserve(in.size());

    const size_t max_threads = std::max(2u, std::thread::hardware_concurrency() / 2);
    std::vector<std::future<std::vector<BAMEntryUI>>> futures;

    auto process_chunk = [](const std::vector<BAMEntry>& chunk) -> std::vector<BAMEntryUI> {
        std::vector<BAMEntryUI> localOut;
        localOut.reserve(chunk.size());

        for (const auto& e : chunk) {
            BAMEntryUI ui{};
            ui.path = WideToUtf8(e.path);
            ui.execTime = FileTimeToTimeT(e.lastExecution);

            std::tm tmStruct{};
            localtime_s(&tmStruct, &ui.execTime);
            std::ostringstream oss;
            oss << std::put_time(&tmStruct, "%Y-%m-%d %H:%M:%S");
            ui.time = oss.str();
            ui.signature = e.signature;

            ui.replaces.reserve(e.replaces.size());
            for (const auto& r : e.replaces) {
                BAMReplaceUI rui{};
                rui.type = r.type;
                rui.startTime = FileTimeToStringUI(r.startTime);
                rui.endTime = FileTimeToStringUI(r.endTime);
                rui.lastUsn = std::to_string(r.lastUsn);

                rui.events.reserve(r.events.size());
                for (const auto& ev : r.events) {
                    rui.events.push_back({
                        FileTimeToStringUI(ev.date),
                        ev.reason
                        });
                }

                ui.replaces.push_back(std::move(rui));
            }

            localOut.push_back(std::move(ui));
        }

        return localOut;
        };

    size_t chunk_size = std::max(size_t(1000), in.size() / max_threads);
    for (size_t i = 0; i < in.size(); i += chunk_size) {
        size_t end = std::min(i + chunk_size, in.size());
        std::vector<BAMEntry> chunk(in.begin() + i, in.begin() + end);
        futures.push_back(std::async(std::launch::async, [process_chunk, chunk]() { return process_chunk(chunk); }));
    }
    for (auto& f : futures) {
        auto local = f.get();
        out.insert(out.end(), local.begin(), local.end());
    }

    return out;
}