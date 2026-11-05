#include "log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>

#include "fsx.h"

namespace vs::log {

static std::mutex g_mtx;
static std::string g_path;

void init(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_path = path;
    if (fsx::file_size(path) > 1024 * 1024) {
        std::error_code ec;
        fsx::fs::rename(fsx::P(path), fsx::P(path + ".old"), ec);
    }
}

static void write(const char* level, const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_path.empty()) return;
    std::ofstream f(fsx::P(g_path), std::ios::app | std::ios::binary);
    if (!f) return;
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    int ms = (int)(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec, ms);
    f << buf << " [" << level << "] " << msg << "\n";
}

void info(const std::string& m) { write("INFO", m); }
void warn(const std::string& m) { write("WARN", m); }
void error(const std::string& m) { write("ERR ", m); }

}  // namespace vs::log
