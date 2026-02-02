#pragma once
#include <windows.h>
#include <ntsecapi.h>
#include <string>
#include <ctime>
#include <mutex>
#include <atomic>

extern std::atomic<time_t> cachedLogonTime;
extern std::mutex logonMutex;

time_t FileTimeToTimeT(const FILETIME& ft);
std::string FormatUptime(time_t startTime);
std::string FormatTime(time_t t);
time_t GetCurrentUserLogonTime();