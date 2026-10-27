#pragma once
#include <string>

namespace gyro4all {

// Plain-text log beside the mod (Gyro4All\Gyro4All.log), one line per call, flushed immediately.
void log_open(const std::wstring& path);
void log(const char* fmt, ...);
// Closes Gyro4All.log (a launcher handing the session to the game it starts, which then opens it).
void log_close();

// Probe results go to their own file (Gyro4All\Gyro4All-probe.log), recreated each session with a header
// that explains how to read it. probe_log falls back to Gyro4All.log if the file couldn't be opened.
bool probe_log_open(const std::wstring& path, const std::string& header);
void probe_log(const char* fmt, ...);

std::string narrow(const std::wstring& text);

}  // namespace gyro4all
