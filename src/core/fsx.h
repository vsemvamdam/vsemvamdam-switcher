// vsemvamdam-switcher — filesystem helpers with UTF-8 paths (portable)
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vs::fsx {

namespace fs = std::filesystem;

fs::path P(const std::string& utf8);
std::string U(const fs::path& p);

bool exists(const std::string& p);
bool is_dir(const std::string& p);
bool is_file(const std::string& p);
bool read_file(const std::string& p, std::string& out);
// Atomic-ish write: write to temp file then rename over target.
bool write_file(const std::string& p, const std::string& data);
// Like write_file, but never falls back to truncating the target: on failure the old file stays intact.
bool write_file_atomic(const std::string& p, const std::string& data);
bool mkdirs(const std::string& p);
bool remove_all(const std::string& p);
bool remove_file(const std::string& p);
// Copy a file or a directory tree. Overwrites existing destination files. Never follows junctions/symlinks.
bool copy_any(const std::string& from, const std::string& to, std::string* err = nullptr);
// Atomically replaces directory `dir` with `tmp` (keeps the old one if anything fails).
bool replace_dir(const std::string& tmp, const std::string& dir);
std::string join(const std::string& a, const std::string& b);
std::string parent(const std::string& p);
std::string filename(const std::string& p);
int64_t mtime_unix(const std::string& p);
uint64_t file_size(const std::string& p);
std::vector<std::string> list_dir(const std::string& p, bool dirs_only = false);
// Simple wildcard expansion for the last path components containing * or ?
std::vector<std::string> glob(const std::string& pattern);
bool wildcard_match(const std::string& pattern, const std::string& name);  // case-insensitive

}  // namespace vs::fsx
