#include "ui/GlslEditor.h"
#include "ui/Theme.h"
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <unordered_set>

namespace glue::ui {
namespace {

const std::unordered_set<std::string_view>& keywords() {
    static const std::unordered_set<std::string_view> k = {
        "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue", "return",
        "discard", "struct", "const", "uniform", "in", "out", "inout", "layout", "flat", "smooth",
        "noperspective", "centroid", "sample", "patch", "precision", "highp", "mediump", "lowp",
        "buffer", "shared", "coherent", "volatile", "restrict", "readonly", "writeonly", "invariant",
        "true", "false", "subroutine", "attribute", "varying"};
    return k;
}
bool isTypeName(std::string_view w) {
    static const std::unordered_set<std::string_view> t = {
        "void", "bool", "int", "uint", "float", "double", "atomic_uint",
        "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4", "uvec2", "uvec3", "uvec4", "bvec2", "bvec3", "bvec4",
        "dvec2", "dvec3", "dvec4", "mat2", "mat3", "mat4", "mat2x2", "mat2x3", "mat2x4", "mat3x2", "mat3x3",
        "mat3x4", "mat4x2", "mat4x3", "mat4x4", "sampler1D", "sampler2D", "sampler3D", "samplerCube",
        "sampler2DArray", "sampler2DShadow", "sampler2DMS", "isampler2D", "usampler2D", "image1D", "image2D",
        "image3D", "iimage2D", "uimage2D", "imageCube", "image2DArray", "samplerBuffer", "imageBuffer"};
    return t.count(w) > 0;
}
bool isBuiltinFn(std::string_view w) {
    static const std::unordered_set<std::string_view> f = {
        "radians", "degrees", "sin", "cos", "tan", "asin", "acos", "atan", "sinh", "cosh", "tanh", "pow",
        "exp", "log", "exp2", "log2", "sqrt", "inversesqrt", "abs", "sign", "floor", "trunc", "round", "ceil",
        "fract", "mod", "modf", "min", "max", "clamp", "mix", "step", "smoothstep", "isnan", "isinf", "length",
        "distance", "dot", "cross", "normalize", "faceforward", "reflect", "refract", "matrixCompMult",
        "outerProduct", "transpose", "determinant", "inverse", "lessThan", "greaterThan", "equal", "notEqual",
        "any", "all", "not", "texture", "textureLod", "textureProj", "textureGrad", "textureSize", "texelFetch",
        "imageLoad", "imageStore", "imageSize", "dFdx", "dFdy", "fwidth", "barrier", "memoryBarrier",
        "atomicAdd", "atomicMin", "atomicMax", "floatBitsToInt", "intBitsToFloat", "packHalf2x16",
        "unpackHalf2x16", "bitfieldExtract", "findLSB", "findMSB", "bitCount", "EmitVertex", "EndPrimitive"};
    return f.count(w) > 0;
}
bool isIdentStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool isIdent(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

ImU32 colourOf(GlslEditor::Kind k) {
    // Catppuccin Mocha syntax colours.
    switch (k) {
        case GlslEditor::Keyword:    return IM_COL32(0xCB, 0xA6, 0xF7, 255);  // mauve
        case GlslEditor::Type:       return IM_COL32(0xF9, 0xE2, 0xAF, 255);  // yellow
        case GlslEditor::Builtin:    return IM_COL32(0x89, 0xB4, 0xFA, 255);  // blue
        case GlslEditor::BuiltinVar: return IM_COL32(0x94, 0xE2, 0xD5, 255);  // teal
        case GlslEditor::Number:     return IM_COL32(0xFA, 0xB3, 0x87, 255);  // peach
        case GlslEditor::Comment:    return IM_COL32(0x6C, 0x70, 0x86, 255);  // overlay0
        case GlslEditor::Preproc:    return IM_COL32(0xF5, 0xC2, 0xE7, 255);  // pink
        case GlslEditor::Op:         return IM_COL32(0x89, 0xDC, 0xEB, 255);  // sky
        case GlslEditor::Call:       return IM_COL32(0x74, 0xC7, 0xEC, 255);  // sapphire
        case GlslEditor::Swizzle:    return IM_COL32(0xEB, 0xA0, 0xAC, 255);  // maroon
        default:                     return IM_COL32(0xCD, 0xD6, 0xF4, 255);  // text
    }
}

} // namespace

void GlslEditor::lex(const std::string& s) {
    spans_.clear();
    lineStart_.assign(1, 0);
    for (size_t i = 0; i < s.size(); ++i) if (s[i] == '\n') lineStart_.push_back((int)i + 1);

    auto push = [&](int b, int e, Kind k) {
        // spans never cross a newline: split them so each line is drawn on its own
        while (b < e) {
            int nl = b;
            while (nl < e && s[nl] != '\n') ++nl;
            if (nl > b && k != Plain) spans_.push_back({b, nl, k});
            b = nl + 1;
        }
    };
    const int n = (int)s.size();
    int i = 0;
    bool lineStartSeen = true;       // only whitespace so far on this line (for '#')
    while (i < n) {
        char c = s[i];
        if (c == '\n') { lineStartSeen = true; ++i; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            int e = i; while (e < n && s[e] != '\n') ++e;
            push(i, e, Comment); i = e; continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            int e = i + 2; while (e + 1 < n && !(s[e] == '*' && s[e + 1] == '/')) ++e;
            e = std::min(n, e + 2);
            push(i, e, Comment); i = e; continue;
        }
        if (c == '#' && lineStartSeen) {
            int e = i; while (e < n && s[e] != '\n') ++e;       // the directive, up to a // comment
            int cm = i; while (cm + 1 < e && !(s[cm] == '/' && s[cm + 1] == '/')) ++cm;
            int stop = (cm + 1 < e) ? cm : e;
            push(i, stop, Preproc);
            if (stop < e) push(stop, e, Comment);
            i = e; continue;
        }
        lineStartSeen = false;
        if (std::isdigit((unsigned char)c) || (c == '.' && i + 1 < n && std::isdigit((unsigned char)s[i + 1]))) {
            int e = i;
            if (c == '0' && e + 1 < n && (s[e + 1] == 'x' || s[e + 1] == 'X')) {
                e += 2; while (e < n && std::isxdigit((unsigned char)s[e])) ++e;
            } else {
                while (e < n && (std::isdigit((unsigned char)s[e]) || s[e] == '.')) ++e;
                if (e < n && (s[e] == 'e' || s[e] == 'E')) {
                    ++e; if (e < n && (s[e] == '+' || s[e] == '-')) ++e;
                    while (e < n && std::isdigit((unsigned char)s[e])) ++e;
                }
            }
            while (e < n && (s[e] == 'f' || s[e] == 'F' || s[e] == 'u' || s[e] == 'U')) ++e;
            push(i, e, Number); i = e; continue;
        }
        if (isIdentStart(c)) {
            int e = i; while (e < n && isIdent(s[e])) ++e;
            std::string_view w(s.data() + i, e - i);
            Kind k = Plain;
            if (keywords().count(w)) k = Keyword;
            else if (isTypeName(w)) k = Type;
            else if (w.rfind("gl_", 0) == 0) k = BuiltinVar;
            else if (isBuiltinFn(w)) k = Builtin;
            else {
                int j = e; while (j < n && (s[j] == ' ' || s[j] == '\t')) ++j;
                if (j < n && s[j] == '(') k = Call;
            }
            // a swizzle: .xyz .rgba .stpq right after a '.' or ')' / ']' / identifier
            if (k == Plain && i > 0 && s[i - 1] == '.' && e - i <= 4) {
                bool sw = true;
                for (char ch : w) if (!std::strchr("xyzwrgbastpq", ch)) { sw = false; break; }
                if (sw) k = Swizzle;
            }
            push(i, e, k); i = e; continue;
        }
        // operators and punctuation, one byte at a time
        if (std::strchr("+-*/%=<>!&|^~?:;,.()[]{}", c)) push(i, i + 1, Op);
        ++i;
    }
    // first span of every line
    lineFirstSpan_.assign(lineStart_.size() + 1, (int)spans_.size());
    size_t sp = 0;
    for (size_t ln = 0; ln < lineStart_.size(); ++ln) {
        while (sp < spans_.size() && spans_[sp].begin < lineStart_[ln]) ++sp;
        lineFirstSpan_[ln] = (int)sp;
    }
    lexed_ = s;
}

std::vector<int> GlslEditor::parseErrorLines(const std::string& log) {
    std::vector<int> out;
    auto add = [&](int v) { if (v > 0 && std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v); };
    for (size_t i = 0; i + 2 < log.size(); ++i) {
        // "0:12" or "0(12)" at the start of a diagnostic
        if (log[i] == '0' && (log[i + 1] == ':' || log[i + 1] == '(') && std::isdigit((unsigned char)log[i + 2])
            && (i == 0 || !std::isalnum((unsigned char)log[i - 1]))) {
            add(std::atoi(log.c_str() + i + 2));
        }
    }
    return out;
}

bool GlslEditor::draw(const char* id, std::string* text, ImVec2 size) {
    const Palette& pal = palette();
    ImGui::PushFont(fonts().code);
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImGuiID inputId = ImGui::GetID(id);
    const ImGuiWindow* parent = g.CurrentWindow;
    const float gutter = ImGui::CalcTextSize("0000").x + 18.0f;

    // The text is invisible: the highlighted copy below replaces it. The caret is drawn by hand.
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, pal.bg0);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {gutter, 10.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    bool edited = ImGui::InputTextMultiline(id, text, size, ImGuiInputTextFlags_AllowTabInput);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);

    if (edited || lexed_ != *text) lex(*text);

    // The multiline field lives in a child window named "<parent>/<label>_<id>".
    char name[256];
    std::snprintf(name, sizeof name, "%s/%s_%08X", parent->Name, id, inputId);
    ImGuiWindow* win = ImGui::FindWindowByName(name);
    if (win) {
        ImDrawList* dl = win->DrawList;
        dl->PushClipRect(win->InnerClipRect.Min, win->InnerClipRect.Max, true);
        const float lineH = g.FontSize;
        // InputText adds FramePadding to the child's cursor start before drawing: same offset here.
        const ImVec2 origin = {win->DC.CursorStartPos.x + gutter, win->DC.CursorStartPos.y + 10.0f};  // first glyph
        const int lines = (int)lineStart_.size();
        const int first = std::max(0, (int)std::floor((win->InnerClipRect.Min.y - origin.y) / lineH));
        const int last = std::min(lines - 1, (int)std::ceil((win->InnerClipRect.Max.y - origin.y) / lineH));

        // gutter background and a hairline
        const float gx = win->Pos.x;
        dl->AddRectFilled({gx, win->InnerClipRect.Min.y}, {gx + gutter - 8.0f, win->InnerClipRect.Max.y},
                          ImGui::GetColorU32(pal.bg1), 0.0f);
        dl->AddLine({gx + gutter - 8.0f, win->InnerClipRect.Min.y}, {gx + gutter - 8.0f, win->InnerClipRect.Max.y},
                    ImGui::GetColorU32(pal.border));

        // caret line and caret position
        int caretLine = -1, caretCol = 0;
        ImGuiInputTextState* st = ImGui::GetInputTextState(inputId);
        if (st) {
            const int cp = std::min<int>(st->GetCursorPos(), (int)text->size());
            auto it = std::upper_bound(lineStart_.begin(), lineStart_.end(), cp);
            caretLine = (int)(it - lineStart_.begin()) - 1;
            caretCol = cp - lineStart_[caretLine];
        }
        ImFont* font = ImGui::GetFont();
        for (int ln = first; ln <= last; ++ln) {
            const float y = origin.y + ln * lineH;
            const int b = lineStart_[ln];
            int e = (ln + 1 < lines) ? lineStart_[ln + 1] - 1 : (int)text->size();
            if (e > b && (*text)[e - 1] == '\r') --e;
            const bool isErr = std::find(errorLines_.begin(), errorLines_.end(), ln + 1) != errorLines_.end();
            if (ln == caretLine && !isErr)
                dl->AddRectFilled({win->InnerClipRect.Min.x, y}, {win->InnerClipRect.Max.x, y + lineH}, IM_COL32(255, 255, 255, 9));
            if (isErr) {
                dl->AddRectFilled({win->InnerClipRect.Min.x, y}, {win->InnerClipRect.Max.x, y + lineH},
                                  ImGui::GetColorU32(ImVec4(pal.danger.x, pal.danger.y, pal.danger.z, 0.14f)));
                dl->AddRectFilled({gx, y}, {gx + 3, y + lineH}, ImGui::GetColorU32(pal.danger));
            }
            // line number
            char num[16];
            std::snprintf(num, sizeof num, "%d", ln + 1);
            const float nw = ImGui::CalcTextSize(num).x;
            dl->AddText({gx + gutter - 18.0f - nw, y},
                        ImGui::GetColorU32(ln == caretLine ? pal.textDim : pal.textFaint), num);

            // tokens: plain text in the gaps, coloured text for each span
            const char* base = text->c_str();
            int pos = b;
            // x of a byte offset: the *unrounded* width of the line prefix, the same measure ImGui uses
            // when it draws the whole line (CalcTextSize would round each piece up to a pixel and drift).
            auto xAt = [&](int off) {
                return origin.x + font->CalcTextSizeA(g.FontSize, FLT_MAX, 0.0f, base + b, base + off).x;
            };
            auto emit = [&](int from, int to, ImU32 col) {
                if (to <= from) return;
                dl->AddText(font, g.FontSize, {xAt(from), y}, col, base + from, base + to);
            };
            for (int si = lineFirstSpan_[ln]; si < (int)spans_.size() && spans_[si].begin < e; ++si) {
                const Span& sp = spans_[si];
                if (sp.begin < pos) continue;
                emit(pos, sp.begin, colourOf(Plain));
                const int se = std::min(sp.end, e);
                emit(sp.begin, se, colourOf(sp.kind));
                pos = se;
            }
            emit(pos, e, colourOf(Plain));

            if (ln == caretLine && st) {
                const bool vis = !g.IO.ConfigInputTextCursorBlink || st->CursorAnim <= 0.0f ||
                                 std::fmod(st->CursorAnim, 1.20f) <= 0.80f;
                if (vis) {
                    const float cx = xAt(b + std::min(caretCol, e - b));
                    dl->AddRectFilled({cx, y + 1}, {cx + 2, y + lineH - 1}, ImGui::GetColorU32(pal.accent), 1.0f);
                }
            }
        }
        dl->PopClipRect();
    }
    ImGui::PopFont();
    return edited;
}

} // namespace glue::ui
