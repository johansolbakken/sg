// Colouring for sg's output: a small per-line syntax highlighter, the match
// highlight on top of it, and the decision whether to colour at all.
//
// Included directly from main.cpp, not compiled on its own.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace color {

/**
 * Whether to write escape codes: not when NO_COLOR is set, always when
 * CLICOLOR_FORCE is set to something other than 0, otherwise only when stdout
 * is a terminal. Same rules as the style crate the Rust tools use.
 */
[[nodiscard]] inline bool use_color() {
    if (std::getenv("NO_COLOR") != nullptr) {
        return false;
    }
    if (const char* force = std::getenv("CLICOLOR_FORCE"); force != nullptr && std::string_view(force) != "0") {
        return true;
    }
    return isatty(STDOUT_FILENO);
}

inline constexpr std::string_view RESET   = "\x1b[0m";
inline constexpr std::string_view PATH    = "\x1b[35m";       // magenta
inline constexpr std::string_view LINE_NO = "\x1b[32m";       // green
inline constexpr std::string_view MATCH   = "\x1b[1;31m";     // bold red, as grep and rg: wins over syntax

inline constexpr std::array<std::string_view, 92> CPP_KEYWORDS = {
    "alignas", "alignof", "and", "asm", "auto", "bool", "break", "case", "catch", "char",
    "char8_t", "char16_t", "char32_t", "class", "co_await", "co_return", "co_yield", "concept",
    "const", "consteval", "constexpr", "constinit", "const_cast", "continue", "decltype",
    "default", "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit", "export",
    "extern", "false", "final", "float", "for", "friend", "goto", "if", "inline", "int", "long",
    "mutable", "namespace", "new", "noexcept", "not", "nullptr", "operator", "or", "override",
    "private", "protected", "public", "register", "reinterpret_cast", "requires", "return",
    "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct", "switch",
    "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid", "typename",
    "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while",
    "size_t", "int32_t", "int64_t", "uint32_t", "uint64_t", "uint8_t",
};

inline constexpr std::array<std::string_view, 53> JAVA_KEYWORDS = {
    "abstract", "assert", "boolean", "break", "byte", "case", "catch", "char", "class", "const",
    "continue", "default", "do", "double", "else", "enum", "extends", "final", "finally", "float",
    "for", "goto", "if", "implements", "import", "instanceof", "int", "interface", "long",
    "native", "new", "package", "private", "protected", "public", "return", "short", "static",
    "strictfp", "super", "switch", "synchronized", "this", "throw", "throws", "transient", "try",
    "void", "volatile", "while", "var", "record", "null",
};

inline constexpr std::array<std::string_view, 54> RUST_KEYWORDS = {
    "as", "async", "await", "break", "const", "continue", "crate", "dyn", "else", "enum",
    "extern", "false", "fn", "for", "if", "impl", "in", "let", "loop", "match", "mod", "move",
    "mut", "pub", "ref", "return", "self", "Self", "static", "struct", "super", "trait", "true",
    "type", "unsafe", "use", "where", "while", "bool", "char", "str", "u8", "u16", "u32", "u64",
    "usize", "i8", "i16", "i32", "i64", "isize", "f32", "f64", "String",
};

/**
 * The keyword list for a file, chosen by its extension. Empty when the
 * language is unknown: strings, comments and numbers are still coloured.
 */
[[nodiscard]] inline std::span<const std::string_view> keywords_for(std::string_view path) noexcept {
    if (path.ends_with(".java")) {
        return JAVA_KEYWORDS;
    }
    if (path.ends_with(".rs")) {
        return RUST_KEYWORDS;
    }
    if (path.ends_with(".cpp") || path.ends_with(".hpp") || path.ends_with(".h") || path.ends_with(".c")) {
        return CPP_KEYWORDS;
    }
    return {};
}

enum class Fg : uint8_t { PLAIN, KEYWORD, STRING, COMMENT, NUMBER };

struct Style {
    Fg   fg    = Fg::PLAIN;
    bool match = false;

    bool operator==(const Style&) const = default;
};

/**
 * Colours one line: syntax first, then the matched ranges on top, then
 * renders it with escape codes only where the style changes. Keeps its
 * buffers between lines so printing does not allocate per line.
 */
class LinePainter {
    std::string_view   _line{};
    std::vector<Style> _styles{};
    std::string        _out{};

public:
    /**
     * Starts a new line and gives it syntax colours. The line is only seen
     * on its own, so a line inside a block comment is recognised by how it
     * starts (a comment opener, or a leading star as in doc comments), and a
     * multi-line string is not recognised.
     */
    void reset(std::string_view line, std::span<const std::string_view> keywords) {
        _line = line;
        _styles.assign(line.size(), Style{});

        size_t first = line.find_first_not_of(" \t");
        if (first != std::string_view::npos && (line.substr(first).starts_with("/*") || line[first] == '*')) {
            paint(0, line.size(), Fg::COMMENT);
            return;
        }

        size_t i = 0;
        while (i < line.size()) {
            if (line.substr(i).starts_with("//")) {
                paint(i, line.size(), Fg::COMMENT);
                return;
            }
            if (line.substr(i).starts_with("/*")) {
                size_t close = line.find("*/", i + 2);
                size_t end   = close == std::string_view::npos ? line.size() : close + 2;
                paint(i, end, Fg::COMMENT);
                i = end;
                continue;
            }
            if (line[i] == '"' || line[i] == '\'') {
                char quote = line[i];
                size_t start = i++;
                while (i < line.size() && line[i] != quote) {
                    i += (line[i] == '\\') ? 2 : 1;
                }
                i = std::min(i + 1, line.size());
                paint(start, i, Fg::STRING);
                continue;
            }
            if (is_word_char(line[i])) {
                size_t start = i;
                while (i < line.size() && is_word_char(line[i])) {
                    i++;
                }
                std::string_view word = line.substr(start, i - start);
                if (is_digit(word[0])) {
                    paint(start, i, Fg::NUMBER);
                } else if (std::ranges::find(keywords, word) != keywords.end()) {
                    paint(start, i, Fg::KEYWORD);
                }
                continue;
            }
            i++;
        }
    }

    /**
     * Marks [begin, end), relative to the line, as matched. Clipped to the
     * line, since a hit may reach past it.
     */
    void mark_match(size_t begin, size_t end) {
        end = std::min(end, _styles.size());
        for (size_t i = begin; i < end; i++) {
            _styles[i].match = true;
        }
    }

    /**
     * The line with escape codes. Valid until the next call to render.
     */
    [[nodiscard]] std::string_view render() {
        _out.clear();
        Style current{};
        for (size_t i = 0; i < _line.size(); i++) {
            if (_styles[i] != current) {
                if (current != Style{}) {
                    _out += RESET;
                }
                _out += code(_styles[i]);
                current = _styles[i];
            }
            _out += _line[i];
        }
        if (current != Style{}) {
            _out += RESET;
        }
        return _out;
    }

private:
    void paint(size_t begin, size_t end, Fg fg) {
        for (size_t i = begin; i < end; i++) {
            _styles[i].fg = fg;
        }
    }

    [[nodiscard]] static std::string_view code(const Style& style) noexcept {
        if (style.match) {
            return MATCH;
        }
        switch (style.fg) {
        case Fg::KEYWORD: return "\x1b[34m";
        case Fg::STRING:  return "\x1b[32m";
        case Fg::COMMENT: return "\x1b[90m";
        case Fg::NUMBER:  return "\x1b[35m";
        case Fg::PLAIN:   return "";
        }
        return "";
    }

    [[nodiscard]] static bool is_digit(char ch) noexcept {
        return ch >= '0' && ch <= '9';
    }

    [[nodiscard]] static bool is_word_char(char ch) noexcept {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || is_digit(ch) || ch == '_';
    }
};

} // namespace color
