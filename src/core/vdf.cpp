#include "vdf.h"

#include "str.h"

namespace vs::vdf {

Node* Node::find(std::string_view k) {
    for (auto& c : children)
        if (iequals(c.key, k)) return &c;
    return nullptr;
}
const Node* Node::find(std::string_view k) const {
    for (auto& c : children)
        if (iequals(c.key, k)) return &c;
    return nullptr;
}
Node* Node::find_path(std::initializer_list<std::string_view> path) {
    Node* n = this;
    for (auto p : path) {
        if (!n || !n->is_object) return nullptr;
        n = n->find(p);
    }
    return n;
}
const Node* Node::find_path(std::initializer_list<std::string_view> path) const {
    const Node* n = this;
    for (auto p : path) {
        if (!n || !n->is_object) return nullptr;
        n = n->find(p);
    }
    return n;
}
Node& Node::obj(std::string_view k) {
    is_object = true;
    if (Node* n = find(k)) {
        if (!n->is_object) { n->is_object = true; n->value.clear(); }
        return *n;
    }
    Node c;
    c.key = std::string(k);
    c.is_object = true;
    children.push_back(std::move(c));
    return children.back();
}
void Node::set(std::string_view k, std::string_view v) {
    is_object = true;
    if (Node* n = find(k)) {
        n->is_object = false;
        n->children.clear();
        n->value = std::string(v);
        return;
    }
    Node c;
    c.key = std::string(k);
    c.value = std::string(v);
    children.push_back(std::move(c));
}
std::string Node::get(std::string_view k, std::string_view def) const {
    const Node* n = find(k);
    if (!n || n->is_object) return std::string(def);
    return n->value;
}
bool Node::remove(std::string_view k) {
    size_t before = children.size();
    std::vector<Node> keep;
    keep.reserve(children.size());
    for (auto& c : children)
        if (!iequals(c.key, k)) keep.push_back(std::move(c));
    children = std::move(keep);
    return children.size() != before;
}

namespace {

struct Lexer {
    std::string_view s;
    size_t i = 0;
    bool escapes = true;

    enum Kind { End, String, Open, Close, Cond, Bad };
    struct Tok { Kind k; std::string text; };

    void skip_ws() {
        for (;;) {
            while (i < s.size() && (unsigned char)s[i] <= ' ') ++i;
            if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/') {
                while (i < s.size() && s[i] != '\n') ++i;
                continue;
            }
            break;
        }
    }

    Tok next() {
        skip_ws();
        if (i >= s.size()) return {End, {}};
        char c = s[i];
        if (c == '{') { ++i; return {Open, {}}; }
        if (c == '}') { ++i; return {Close, {}}; }
        if (c == '[') {  // conditional like [$WIN32]
            size_t j = s.find(']', i);
            std::string t(s.substr(i, j == std::string_view::npos ? std::string_view::npos : j - i + 1));
            i = j == std::string_view::npos ? s.size() : j + 1;
            return {Cond, t};
        }
        if (c == '"') {
            ++i;
            std::string out;
            while (i < s.size()) {
                char d = s[i];
                if (d == '"') { ++i; return {String, out}; }
                if (d == '\\' && escapes && i + 1 < s.size()) {
                    char e = s[i + 1];
                    switch (e) {
                        case '"': out += '"'; i += 2; continue;
                        case '\\': out += '\\'; i += 2; continue;
                        case 'n': out += '\n'; i += 2; continue;
                        case 't': out += '\t'; i += 2; continue;
                        default: out += d; ++i; continue;  // keep unknown escape literally
                    }
                }
                out += d;
                ++i;
            }
            return {Bad, out};  // unterminated
        }
        // unquoted token
        std::string out;
        while (i < s.size()) {
            char d = s[i];
            if ((unsigned char)d <= ' ' || d == '{' || d == '}' || d == '"') break;
            out += d;
            ++i;
        }
        return {String, out};
    }
};

bool parse_children(Lexer& lx, Node& parent, bool top, std::string* err, int depth) {
    if (depth > 256) { if (err) *err = "nesting too deep"; return false; }
    for (;;) {
        auto t = lx.next();
        if (t.k == Lexer::Cond) continue;
        if (t.k == Lexer::End) {
            if (!top) { if (err) *err = "unexpected end of file"; return false; }
            return true;
        }
        if (t.k == Lexer::Close) {
            if (top) { if (err) *err = "unexpected '}'"; return false; }
            return true;
        }
        if (t.k != Lexer::String) { if (err) *err = "expected key"; return false; }
        Node child;
        child.key = std::move(t.text);
        auto v = lx.next();
        while (v.k == Lexer::Cond) v = lx.next();
        if (v.k == Lexer::Open) {
            child.is_object = true;
            if (!parse_children(lx, child, false, err, depth + 1)) return false;
        } else if (v.k == Lexer::String) {
            child.value = std::move(v.text);
            // optional conditional after value
            size_t save = lx.i;
            auto peek = lx.next();
            if (peek.k != Lexer::Cond) lx.i = save;
        } else {
            if (err) *err = "expected value for key '" + child.key + "'";
            return false;
        }
        parent.children.push_back(std::move(child));
    }
}

void write_node(std::string& out, const Node& n, int depth, bool esc) {
    std::string ind(depth, '\t');
    auto q = [&](const std::string& s) { return "\"" + (esc ? escape(s) : s) + "\""; };
    if (n.is_object) {
        out += ind + q(n.key) + "\n" + ind + "{\n";
        for (auto& c : n.children) write_node(out, c, depth + 1, esc);
        out += ind + "}\n";
    } else {
        out += ind + q(n.key) + "\t\t" + q(n.value) + "\n";
    }
}

}  // namespace

std::string escape(std::string_view s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        if (c == '\\') r += "\\\\";
        else if (c == '"') r += "\\\"";
        else if (c == '\n') r += "\\n";
        else if (c == '\t') r += "\\t";
        else r += c;
    }
    return r;
}

bool parse(std::string_view text, Node& root, std::string* error, ParseOptions opt) {
    // strip UTF-8 BOM
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text.remove_prefix(3);
    root = Node{};
    root.is_object = true;
    Lexer lx{text, 0, opt.escapes};
    return parse_children(lx, root, true, error, 0);
}

std::string write(const Node& root, bool escapes) {
    std::string out;
    for (auto& c : root.children) write_node(out, c, 0, escapes);
    return out;
}

}  // namespace vs::vdf
