// vsemvamdam-switcher — portable helpers for the generic platform engine (TcNo-compatible Platforms.json)
#pragma once
#include <map>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace vs::basic {

// One LoginFiles / PathListToClear entry, decoded.
struct Spec {
    enum Kind { File, Reg, Json } kind = File;
    std::string path;        // file/folder path (may contain * in the last component); for Reg: key path
    std::string value_name;  // Reg: value name
    std::string json_path;   // Json: dotted path inside the file
    std::string json_mode;   // "JSON_SELECT", "JSON_SELECT_FIRST", "JSON_EMPTY_VALUE"
    std::string delimiter;   // JSON_SELECT_FIRST,  -> ","
};

// "REG:HKCU\\Software\\X:Value", "JSON_SELECT_FIRST,::C:\\file::A.B", plain paths
Spec parse_spec(const std::string& raw);

// Replaces %Var% tokens (case-insensitive) using vars; unknown tokens are left as is.
std::string expand_vars(const std::string& s, const std::map<std::string, std::string>& vars);

// Dotted JSON path helpers ("Client.SavedAccountNames"). Missing parents are created on set.
const nlohmann::json* json_get(const nlohmann::json& root, const std::string& path);
bool json_set(nlohmann::json& root, const std::string& path, const nlohmann::json& value);

// Selects the first element of a delimited string ("a@x.com,b@y.com" -> "a@x.com").
std::string first_of(const std::string& s, const std::string& delim);

// Extracts a unique id from text with a TcNo-style regex: supports (?i) and (?s) prefixes and "EMAIL_REGEX".
// Returns the first match of capture group 1 (the last one when the pattern starts with a greedy ".*").
std::string regex_id(const std::string& text, const std::string& pattern);

std::string slug(const std::string& name);   // "Epic Games" -> "epic-games"
std::string random_id();                     // 32 hex chars

}  // namespace vs::basic
