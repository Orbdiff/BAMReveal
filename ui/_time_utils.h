#pragma once
#include <windows.h>
#include <ctime>
#include <string>
#include <ntsecapi.h>
#include <iostream>
#include <iomanip>
#include <lmcons.h>

time_t FileTimeToTimeT(const FILETIME& ft);
std::string FormatUptime(time_t startTime);
std::string FormatTime(time_t t);
time_t GetCurrentUserLogonTime();