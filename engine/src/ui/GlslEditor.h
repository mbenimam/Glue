#pragma once
#include <imgui.h>
#include <string>
#include <vector>

namespace glue::ui {

// A GLSL text editor with syntax highlighting, line numbers and error-line marks.
//
// Editing (caret, selection, clipboard, undo, scrolling) is Dear ImGui's own InputTextMultiline. It
// is drawn with a fully transparent text colour, and the same text is painted on top, token by token,
// in its syntax colours at exactly the positions ImGui used. The tokens come from a small GLSL lexer.
class GlslEditor {
public:
    // Draws the editor. Returns true when the text was edited this frame.
    bool draw(const char* id, std::string* text, ImVec2 size);

    // 1-based line numbers to mark in the gutter and tint (compiler errors).
    void setErrorLines(std::vector<int> lines) { errorLines_ = std::move(lines); }
    // Line numbers from a driver info log: "0:12(5): error: ..." (Mesa), "0(12) : error" (NVIDIA),
    // "ERROR: 0:12: ..." (generic).
    static std::vector<int> parseErrorLines(const std::string& log);

    enum Kind : unsigned char { Plain, Keyword, Type, Builtin, BuiltinVar, Number, Comment, Preproc, Op, Call, Swizzle };
    struct Span { int begin, end; Kind kind; };   // byte range in the text

private:
    void lex(const std::string& s);
    std::string lexed_;                     // the text the spans belong to
    std::vector<Span> spans_;               // sorted, non-overlapping, never crossing a newline
    std::vector<int> lineStart_;            // byte offset of each line
    std::vector<int> lineFirstSpan_;        // index of the first span of each line
    std::vector<int> errorLines_;
};

} // namespace glue::ui
