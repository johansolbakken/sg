#include <algorithm>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <print>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include "colorlib.cpp"

// Syntax:
//   sg term         substring search on each line.
//   sg a b          terms separted by space is AND.
//
//   sg word:term    prefix with word or w to search for this non prefix.
//   sg w:term
//
//   sg a . b        b is the next word after a, on the same line: only
//                   whitespace and punctuation between, as in `a b`, `a(b`,
//                   `a->b`, `a::b`. Not `a x b`.
//   sg a .. b       the same, but the gap may also hold newlines, so `a\n->b`
//                   matches. The hit starts at a and every line up to b is
//                   printed.
//
//   . and .. bind tighter than AND and chain from the left: `x a .. b . c`
//   is x AND ((a .. b) . c).

/**
 * What file extensions to allow.
 */
class FileFilter {
public:
    bool allow(std::string_view file_name, bool is_dir) {
        return is_dir ||
               file_name.ends_with(".cpp") ||
               file_name.ends_with(".java") ||
               file_name.ends_with(".h") ||
               file_name.ends_with(".hpp") ||
               file_name.ends_with(".c") ||
               file_name.ends_with(".rs");
    }
};

/**
 * File entry in the file iterator.
 */
class Entry {
public:
    using Path = std::filesystem::path;
    enum class Kind { FILE, DIR };

private:
    Kind _kind;
    Path _path;

public:
    explicit Entry(const Path& path) : _path(path) {
        _kind = std::filesystem::is_directory(path) ? Kind::DIR : Kind::FILE;
    }

    [[nodiscard]] Kind kind() const noexcept { return _kind; }
    [[nodiscard]] Path path() const noexcept { return _path; }
};

class Source {
    std::string_view    _text{};
    std::vector<size_t> _line_starts{};

public:
    void set_source(std::string_view sv) {
        _text = sv;

        _line_starts.clear();
        _line_starts.push_back(0);
        for (size_t i = 0; i < _text.size(); i++) {
            if (_text[i] == '\n') {
                _line_starts.push_back(i + 1);
            }
        }
    }

    [[nodiscard]] std::string_view text() const noexcept { return _text; }

    [[nodiscard]] size_t line_of(size_t offset) const {
        auto it = std::upper_bound(_line_starts.begin(), _line_starts.end(), offset);
        return it - _line_starts.begin();
    }

    size_t line_start(size_t line) const {
        return _line_starts[line - 1];
    }

    [[nodiscard]] size_t next_line_start(size_t line) const {
        return line < _line_starts.size() ? _line_starts[line] : _text.size();
    }

    [[nodiscard]] std::string_view line_text(size_t line) const {
        size_t start = line_start(line);
        size_t end   = _text.find('\n', start);
        if (end == std::string_view::npos) {
            end = _text.size();              // last line has no '\n'
        }
        return _text.substr(start, end - start);
    }
};

struct Hit {
    size_t begin;
    size_t end;
};

class Iter {
public:
    virtual ~Iter() = default;
    virtual bool seek(size_t from, Hit& out) = 0;
    virtual void set_source(const Source& sv) = 0;

    /**
     * Appends every hit of the query's terms that begins in [begin, end).
     * Used to paint what matched on a line, so it collects the terms' own
     * hits, not the combined hit seek returns.
     */
    virtual void highlights(size_t begin, size_t end, std::vector<Hit>& out) = 0;
};

using IterP = std::unique_ptr<Iter>;
using IterList = std::vector<IterP>;

class AndIter : public Iter {
public:
    IterList         _children;
    std::vector<Hit> _hits;
    const Source*    _source;

public:
    explicit AndIter(IterList& children)
        : _children(std::move(children)), _hits(_children.size())
    {}

    ~AndIter() override = default;

    bool seek(size_t from, Hit& out) override {
        while (true) {
            // Spin all children and calculate which got furthest, because those do not have
            // hits between from and furthest.
            size_t max_line = 0;
            for (size_t i = 0; i < _hits.size(); i++) {
                if (!_children[i]->seek(from, _hits[i])) {
                    return false;
                }
                max_line = std::max(max_line, _source->line_of(_hits[i].begin));
            }

            // Check if all stands on same line.
            bool all_on_line = true;
            for (const auto& hit : _hits) {
                if (_source->line_of(hit.begin) != max_line) {
                    all_on_line = false;
                    break;
                }
            }

            // If all stands on same line, we have a match.
            if (all_on_line) {
                out.begin = _hits[0].begin;
                out.end = _hits[0].end;
                for (const auto& hit : _hits) {
                    out.begin = std::min(hit.begin, out.begin);
                    out.end = std::max(hit.end, out.end);
                }
                return true;
            }

            from = _source->line_start(max_line);
        }
    }

    void set_source(const Source& sv) override {
        _source = &sv;
        for (const auto& child : _children) {
            child->set_source(sv);
        }
    }

    void highlights(size_t begin, size_t end, std::vector<Hit>& out) override {
        for (const auto& child : _children) {
            child->highlights(begin, end, out);
        }
    }
};

class SubstringIter : public Iter {
    std::string      _term;
    const Source*    _source;

public:
    SubstringIter(const std::string& term) : _term(std::move(term)) {
    }

    ~SubstringIter() override = default;

    bool seek(size_t from, Hit& out) override {
        size_t pos = _source->text().find(_term, from);
        if (pos == std::string_view::npos) {
            return false;
        }

        out = { pos, pos + _term.size() };
        return true;
    }

    void set_source(const Source& sv) override {
        _source = &sv;
    }

    void highlights(size_t begin, size_t end, std::vector<Hit>& out) override {
        Hit hit;
        size_t from = begin;
        while (seek(from, hit) && hit.begin < end) {
            out.push_back(hit);
            from = hit.end;
        }
    }
};

/**
 * A character of an identifier. Words are runs of these; anything else is
 * whitespace or punctuation.
 */
[[nodiscard]] inline bool is_word_char(char ch) noexcept {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
}

class WordIter : public Iter {
    std::string   _term;
    const Source* _source = nullptr;

public:
    explicit WordIter(std::string term) : _term(std::move(term)) {}

    [[nodiscard]] bool seek(size_t from, Hit& out) override {
        std::string_view text = _source->text();
        size_t pos = text.find(_term, from);
        while (pos != std::string_view::npos) {
            size_t end = pos + _term.size();
            bool starts_word = pos == 0 || !is_word_char(text[pos - 1]);
            bool ends_word   = end == text.size() || !is_word_char(text[end]);
            if (starts_word && ends_word) {
                out = { pos, end };
                return true;
            }
            pos = text.find(_term, pos + 1);
        }
        return false;
    }

    void set_source(const Source& source) override {
        _source = &source;
    }

    void highlights(size_t begin, size_t end, std::vector<Hit>& out) override {
        Hit hit;
        size_t from = begin;
        while (seek(from, hit) && hit.begin < end) {
            out.push_back(hit);
            from = hit.end;
        }
    }
};

/**
 * `left . right` and `left .. right`: right is in the next word after the
 * word left ends in, with only whitespace and punctuation between. For `.`
 * the gap may not hold a newline; for `..` it may. The hit runs from left's
 * begin to right's end.
 *
 * Terms are substrings, so "word" is the word around the hit: `foo . bar`
 * matches "foox(xbar" too, the way `foo` alone matches "foox".
 */
class FollowedByIter : public Iter {
    IterP         _left;
    IterP         _right;
    bool          _same_line;
    const Source* _source = nullptr;

    // The last answer from _right: its first hit at or after _right_from.
    // Lefts that come before the same right ask the same question again, so
    // this keeps a sweep over the file linear.
    size_t        _right_from  = 0;
    bool          _right_found = false;
    Hit           _right_hit{};
    bool          _right_valid = false;

public:
    FollowedByIter(IterP left, IterP right, bool same_line)
        : _left(std::move(left)), _right(std::move(right)), _same_line(same_line) {}

    [[nodiscard]] bool seek(size_t from, Hit& out) override {
        std::string_view text = _source->text();
        Hit a;
        while (_left->seek(from, a)) {
            // Skip the rest of the word left ends in, then the gap.
            size_t i = a.end;
            while (i < text.size() && is_word_char(text[i])) {
                i++;
            }
            bool newline_in_gap = false;
            while (i < text.size() && !is_word_char(text[i])) {
                newline_in_gap |= text[i] == '\n';
                i++;
            }
            size_t word_begin = i;
            size_t word_end   = i;
            while (word_end < text.size() && is_word_char(text[word_end])) {
                word_end++;
            }

            Hit b;
            if (!seek_right(word_begin, b)) {
                return false;                   // no right after this left, so none after later lefts
            }
            if (b.begin < word_end && !(_same_line && newline_in_gap)) {
                out = { a.begin, b.end };
                return true;
            }
            from = a.begin + 1;
        }
        return false;
    }

    void set_source(const Source& source) override {
        _source = &source;
        _left->set_source(source);
        _right->set_source(source);
        _right_valid = false;
    }

    void highlights(size_t begin, size_t end, std::vector<Hit>& out) override {
        _left->highlights(begin, end, out);
        _right->highlights(begin, end, out);
    }

private:
    /**
     * _right->seek(from, out), answered from the last call when it still
     * holds: the first hit at or after an earlier `from` is also the first at
     * or after any `from` up to that hit.
     */
    [[nodiscard]] bool seek_right(size_t from, Hit& out) {
        bool reuse = _right_valid && _right_from <= from && (!_right_found || from <= _right_hit.begin);
        if (!reuse) {
            _right_from  = from;
            _right_found = _right->seek(from, _right_hit);
            _right_valid = true;
        }
        out = _right_hit;
        return _right_found;
    }
};

/**
 * The query the user wants to query on.
 */
class Query {
    std::string            _text{};
    std::unique_ptr<Iter>  _iter;

public:
    Query() = default;
    explicit Query(const std::string& text) : _text(std::move(text)) {
        trim(_text);
        if (_text.empty()) {
            throw std::runtime_error("Query was empty. Use --help for usage.");
        }
        parse();
    }

    [[nodiscard]] bool seek(size_t from, Hit& out) {
        return _iter->seek(from, out);
    }

    void set_source(const Source& source) {
        _iter->set_source(source);
    }

    void highlights(size_t begin, size_t end, std::vector<Hit>& out) {
        _iter->highlights(begin, end, out);
    }

private:
    struct Token {
        enum class Kind { IDENT, DOT, DOTDOT, COLON };
        Kind kind;
        std::string_view image;
    };

    class Lexer {
        size_t _i = 0;
        std::string_view _source;

    public:
        explicit Lexer(std::string_view source) : _source(source) {
        }

        bool lex(Token& out) {
            while (!is_at_end() && is_whitespace()) {
                _i++;
            }
            if (is_at_end()) {
                return false;
            }
            if (_source[_i] == '.') {
                if (_i + 1 < _source.size() && _source[_i + 1] == '.') {
                    out = Token { Token::Kind::DOTDOT, _source.substr(_i, 2) };
                    _i += 2;
                    return true;
                }
                out = Token { Token::Kind::DOT, _source.substr(_i++, 1) };
                return true;
            }
            if (_source[_i] == ':') {
                out = Token { Token::Kind::COLON, _source.substr(_i++, 1) };
                return true;
            }
            if (is_ident()) {
                size_t start = _i++;
                while (is_ident()) {
                    _i++;
                }
                out = Token { Token::Kind::IDENT, _source.substr(start, _i - start) };
                return true;
            }
            throw std::runtime_error(std::format("Invalid character encountered while parsing query '{}'", _source[_i]));
        }

        bool peek(Token& out) {
            size_t i = _i;
            bool return_code = lex(out);
            _i = i;
            return return_code;
        }

        bool is_at_end() {
            return _i >= _source.size();
        }

        bool is_whitespace() {
            return _source[_i] == ' ';
        }

        bool is_alpha() {
            return (_source[_i] >= 'a' && _source[_i] <= 'z') || (_source[_i] >= 'A' && _source[_i] <= 'Z');
        }

        bool is_number() {
            return _source[_i] >= '0' && _source[_i] <= '9';
        }

        bool is_ident() {
            return is_alpha() || is_number() || _source[_i] == '-' || _source[_i] == '_' || _source[_i] == '$';
        }
    };

    class Parser {
        Lexer& _lexer;
        Token _t{};

    public:
        explicit Parser(Lexer& lexer) : _lexer(lexer) {}

        std::unique_ptr<Iter> parse() {
            return parse_and_iter();
        }

    private:
        std::unique_ptr<Iter> parse_and_iter() {
            std::vector<std::unique_ptr<Iter>> children;

            while (!_lexer.is_at_end()) {
                children.emplace_back(parse_followed_by());
            }

            return std::make_unique<AndIter>(children);
        }

        // followed_by := term (('.' | '..') term)*
        std::unique_ptr<Iter> parse_followed_by() {
            std::unique_ptr<Iter> left = parse_term();
            Token op;
            while (_lexer.peek(op) && (op.kind == Token::Kind::DOT || op.kind == Token::Kind::DOTDOT)) {
                _lexer.lex(op);
                if (_lexer.is_at_end()) {
                    throw std::runtime_error(std::format("Expected term after '{}'", op.image));
                }
                std::unique_ptr<Iter> right = parse_term();
                left = std::make_unique<FollowedByIter>(std::move(left), std::move(right), op.kind == Token::Kind::DOT);
            }
            return left;
        }

        std::unique_ptr<Iter> parse_term() {
            if (!_lexer.lex(_t)) {
                throw std::runtime_error("Parsing term but encountered EOF.");
            }

            if (_t.kind != Token::Kind::IDENT) {
                throw std::runtime_error(std::format("Expected term but got '{}'", _t.image));
            }

            if (_t.image == "word" || _t.image == "w") {
                std::string_view w = _t.image;
                Token colon;
                if (_lexer.peek(colon) && colon.kind == Token::Kind::COLON) {
                    _lexer.lex(colon);
                    if (!_lexer.lex(_t)) {
                        throw std::runtime_error("Parsing word term but encountered EOF.");
                    }

                    if (_t.kind != Token::Kind::IDENT) {
                        throw std::runtime_error(std::format("Expected term after '{}:' but got '{}'", w, _t.image));
                    }

                    return std::make_unique<WordIter>(std::string(_t.image));
                }
            }

            return std::make_unique<SubstringIter>(std::string(_t.image));
        }
    };

    void parse() {
        Lexer lexer(_text);
        Parser parse(lexer);
        _iter = parse.parse();
    }

    static constexpr std::string WHITESPACE = " \t\n\r\f\v";

    // Trim from start (in place)
    void ltrim(std::string& s) {
        size_t start = s.find_first_not_of(WHITESPACE);
        s = (start == std::string::npos) ? "" : s.substr(start);
    }

    // Trim from end (in place)
    void rtrim(std::string& s) {
        size_t end = s.find_last_not_of(WHITESPACE);
        s = (end == std::string::npos) ? "" : s.substr(0, end + 1);
    }

    // Trim from both ends (in place)
    void trim(std::string& s) {
        rtrim(s);
        ltrim(s);
    }
};

struct LineColumn {
    size_t line;
    size_t column;
};

struct Token {
    std::string_view image;
    LineColumn       line_column;
};

struct MatchInfo {
    bool is_match{};
    std::vector<LineColumn> line_columns{};

    static MatchInfo match(const std::vector<Token>& tokens) {
        std::vector<LineColumn> line_columns{};
        line_columns.reserve(tokens.size());
        for (const auto& token : tokens) {
            line_columns.emplace_back(token.line_column);
        }
        return { true, line_columns };
    }
};

class Output {
    bool               _color = color::use_color();
    color::LinePainter _painter{};

public:
    /**
     * Prints one line of a hit. `hits` are the terms' hits on the line, as
     * offsets relative to the line's start. The first line of a hit is
     * `path:line:col:text`; when a hit spans lines, the rest are
     * `path-line-text`, the way grep marks context lines.
     */
    void print(std::string_view path, size_t line_no, size_t col, bool first, std::string_view line,
               const std::vector<Hit>& hits, std::span<const std::string_view> keywords) {
        if (!_color) {
            if (first) {
                std::println("{}:{}:{}:{}", path, line_no, col, line);
            } else {
                std::println("{}-{}-{}", path, line_no, line);
            }
            return;
        }
        _painter.reset(line, keywords);
        for (const auto& hit : hits) {
            _painter.mark_match(hit.begin, hit.end);
        }
        if (first) {
            std::println("{}{}{}:{}{}{}:{}:{}",
                         color::PATH, path, color::RESET,
                         color::LINE_NO, line_no, color::RESET,
                         col, _painter.render());
        } else {
            std::println("{}{}{}-{}{}{}-{}",
                         color::PATH, path, color::RESET,
                         color::LINE_NO, line_no, color::RESET,
                         _painter.render());
        }
    }
};

/**
 * Main search app.
 */
class SearchApp {
    std::queue<Entry>       _iter{};
    Query                   _query{};
    std::string             _file_backing{};
    MatchInfo               _match_info{};
    std::string             _line;
    Source                  _source;
    Output                  _output{};
    FileFilter              _filter{};
    std::vector<Hit>        _hit_terms{};
    std::vector<Hit>        _line_hits{};

private:
    void setup(int argc, char** argv) {
        std::stringstream ss;
        for (size_t i = 1; i < static_cast<size_t>(argc); i++) {
            if (i) {
                ss << ' ';
            }
            ss << argv[i];
        }
        _query = Query(ss.str());
    }

    void add_children(const Entry& path) {
        for (const auto& entry : std::filesystem::directory_iterator(path.path())) {
            _iter.push(Entry(entry));
        }
    }

    void read_file(const Entry::Path& path) {
        _file_backing.clear();
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            throw std::runtime_error(std::format("Failed to open file '{}'", path.string()));
        }

        auto size = file.tellg();
        if (size > 10000000) {
            throw std::runtime_error(std::format("File '{}' is larger than 10MB. TODO: let the user set larger limit?", path.string()));
        }

        _file_backing.resize(size);

        file.seekg(0);
        file.read(_file_backing.data(), size);
    }

    void handle_file(const Entry::Path& path) {
        read_file(path);

        if (_file_backing.size() <= 0) {
            return;
        }

        _source.set_source(_file_backing);
        _query.set_source(_source);

        std::string pstr = path.lexically_normal().string();
        auto keywords = color::keywords_for(pstr);
        size_t from = 0;
        Hit hit;
        while (_query.seek(from, hit)) {
            // A hit of `..` can span lines: print all of them.
            size_t first_line = _source.line_of(hit.begin);
            size_t last_line  = _source.line_of(hit.end - 1);
            size_t col        = hit.begin - _source.line_start(first_line) + 1;

            _hit_terms.clear();
            _query.highlights(_source.line_start(first_line), _source.next_line_start(last_line), _hit_terms);

            for (size_t line = first_line; line <= last_line; line++) {
                size_t line_begin = _source.line_start(line);
                std::string_view text = _source.line_text(line);
                size_t line_end = line_begin + text.size();

                _line_hits.clear();
                for (const auto& h : _hit_terms) {
                    if (h.begin >= line_begin && h.begin < line_end) {
                        _line_hits.push_back({ h.begin - line_begin, h.end - line_begin });
                    }
                }
                _output.print(pstr, line, col, line == first_line, text, _line_hits, keywords);
            }
            from = _source.next_line_start(last_line);
        }
    }

public:
    int run(int argc, char** argv) {
        setup(argc, argv);

        _iter.push(Entry("."));

        while (!_iter.empty()) {
            auto entry = _iter.front();
            _iter.pop();

            if (!_filter.allow(entry.path().string(), entry.kind() == Entry::Kind::DIR)) {
                continue;
            }

            switch (entry.kind()) {
            case Entry::Kind::DIR:
                add_children(entry);
                break;
            case Entry::Kind::FILE:
                handle_file(entry.path());
                break;
            }
        }

        return 0;
    }
};

int main(int argc, char** argv) {
    SearchApp app;
    int exit_code = 0;
    try {
        exit_code = app.run(argc, argv);
    } catch (const std::exception& e) {
        std::println(stderr, "search failed: {}", e.what());
        exit_code = 1;
    }
    return exit_code;
}
