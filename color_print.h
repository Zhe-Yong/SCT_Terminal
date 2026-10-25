#pragma once

// ============ 前景色 ============
#define CLR_Black        30
#define CLR_Red          31
#define CLR_Green        32
#define CLR_Yellow       33
#define CLR_Blue         34
#define CLR_Magenta      35
#define CLR_Cyan         36
#define CLR_White        37
#define CLR_Gray         90
#define CLR_BrightRed    91
#define CLR_BrightGreen  92
#define CLR_BrightYellow 93
#define CLR_BrightBlue   94
#define CLR_BrightMagenta 95
#define CLR_BrightCyan   96
#define CLR_BrightWhite  97

// ============ 背景色 ============
#define CLR_BgBlack        40
#define CLR_BgRed          41
#define CLR_BgGreen        42
#define CLR_BgYellow       43
#define CLR_BgBlue         44
#define CLR_BgMagenta      45
#define CLR_BgCyan         46
#define CLR_BgWhite        47
#define CLR_BgGray         100
#define CLR_BgBrightRed    101
#define CLR_BgBrightGreen  102
#define CLR_BgBrightYellow 103
#define CLR_BgBrightBlue   104
#define CLR_BgBrightMagenta 105
#define CLR_BgBrightCyan   106
#define CLR_BgBrightWhite  107

// ============ 强调方式 ============
#define CLR_Reset     0
#define CLR_Bold      1
#define CLR_Dim       2
#define CLR_Italic    3
#define CLR_Underline 4
#define CLR_Blink     5
#define CLR_Reverse   7
#define CLR_Hidden    8
#define CLR_Strike    9

// ============ 特殊 ============
#define clr_rst "\033[0m"
#define CLR_NONE  (-1)

// ============ 内部状态（颜色数据，跨 print 保留）============
namespace clr_detail {
    inline int cur_fg    = CLR_NONE;
    inline int cur_bg    = CLR_NONE;
    inline int cur_style = CLR_NONE;

    inline std::string current_code() {
        std::string code = "\033[";
        bool first = true;
        auto append = [&](int v) {
            if (v == CLR_NONE) return;
            if (!first) code += ";";
            code += std::to_string(v);
            first = false;
        };
        append(cur_style);
        append(cur_fg);
        append(cur_bg);
        if (first) return "";
        return code + "m";
    }

    inline bool has_color() {
        return cur_fg != CLR_NONE || cur_bg != CLR_NONE || cur_style != CLR_NONE;
    }
}

// ============ 设置颜色（不重置、不输出文字）============
inline void color_set(int fg = CLR_NONE,
                      int bg = CLR_NONE,
                      int style = CLR_NONE) {
    if (fg    != CLR_NONE) clr_detail::cur_fg    = fg;
    if (bg    != CLR_NONE) clr_detail::cur_bg    = bg;
    if (style != CLR_NONE) clr_detail::cur_style = style;

    std::string code = clr_detail::current_code();
    if (!code.empty()) std::cout << code;
}

// ============ 彻底重置：清视觉 + 清数据 ============
inline void print_rst(std::ostream& os) {
    os << clr_rst;
    clr_detail::cur_fg    = CLR_NONE;
    clr_detail::cur_bg    = CLR_NONE;
    clr_detail::cur_style = CLR_NONE;
}

inline void print_rst() {
    print_rst(std::cout);
}

// ============ 核心 print ============
// 规则：
//   1. 传了颜色参数 → 更新保存的颜色
//   2. 输出文字时应用当前颜色
//   3. \n 前重置，\n 后恢复（换行不带色）
//   4. 全部输出完 → 输出 clr_rst（清视觉），但保留颜色数据
inline void print(std::ostream& os,
                  const std::string& text = "",
                  int fg    = CLR_NONE,
                  int bg    = CLR_NONE,
                  int style = CLR_NONE) {
    // 1. 更新保存的颜色
    if (fg    != CLR_NONE) clr_detail::cur_fg    = fg;
    if (bg    != CLR_NONE) clr_detail::cur_bg    = bg;
    if (style != CLR_NONE) clr_detail::cur_style = style;

    // 无颜色：直接输出（末尾仍重置一下，保证安全）
    if (!clr_detail::has_color()) {
        os << text;
        return;
    }

    std::string code = clr_detail::current_code();

    // 2. 逐段输出，\n 前重置、\n 后恢复
    size_t start = 0;
    while (true) {
        size_t pos = text.find('\n', start);
        if (pos == std::string::npos) {
            os << code << text.substr(start);
            break;
        }
        os << code << text.substr(start, pos - start)
           << clr_rst << '\n' << code;
        start = pos + 1;
    }

    // 3. 结束：清视觉，但保留颜色数据
    os << clr_rst;
}

inline void print(const std::string& text = "",
                  int fg    = CLR_NONE,
                  int bg    = CLR_NONE,
                  int style = CLR_NONE) {
    print(std::cout, text, fg, bg, style);
}