// vsemvamdam-switcher — the "--steam-helper" child process mode (see steam_helper.cpp)
#pragma once
#include <string>
#include <vector>

namespace vs {
// true if args start with "--steam-helper" (the process should exit with rc afterwards)
bool steam_helper_main(const std::vector<std::string>& args, int& rc);
}  // namespace vs
