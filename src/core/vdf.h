// vsemvamdam-switcher — Valve KeyValues (text VDF) parser/writer, order-preserving.
#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vs::vdf {

struct Node {
    std::string key;
    std::string value;            // valid when !is_object
    bool is_object = false;
    std::vector<Node> children;   // valid when is_object

    // Case-insensitive child lookup (first match).
    Node* find(std::string_view k);
    const Node* find(std::string_view k) const;
    // Path lookup: {"UserLocalConfigStore","friends","ePersonaState"}
    Node* find_path(std::initializer_list<std::string_view> path);
    const Node* find_path(std::initializer_list<std::string_view> path) const;
    // Get or create object child.
    Node& obj(std::string_view k);
    // Set (create or replace) a string child value.
    void set(std::string_view k, std::string_view v);
    std::string get(std::string_view k, std::string_view def = "") const;
    bool remove(std::string_view k);  // remove all children with key (case-insensitive)
};

struct ParseOptions {
    bool escapes = true;  // interpret \" \\ \n \t inside quoted strings
};

// Parses a whole document. The returned root is a synthetic object whose
// children are the top-level key/values (usually one, e.g. "users").
bool parse(std::string_view text, Node& root, std::string* error = nullptr, ParseOptions opt = {});

// Serializes in Steam's own style (tabs, two tabs between key and value).
std::string write(const Node& root, bool escapes = true);

std::string escape(std::string_view s);

}  // namespace vs::vdf
