// vsemvamdam-switcher — tiny thread-safe file logger
#pragma once
#include <string>

namespace vs::log {

void init(const std::string& path);  // rotates when > 1 MB
void info(const std::string& msg);
void warn(const std::string& msg);
void error(const std::string& msg);

}  // namespace vs::log
