#include "global.h"

#include <richedit.h>
#include <windowsx.h>

namespace {

constexpr UINT WM_TERMINAL_UPDATE = WM_APP + 1;
constexpr UINT WM_TERMINAL_CLEAR = WM_APP + 2;
constexpr UINT WM_TERMINAL_TITLE = WM_APP + 3;
constexpr UINT WM_TERMINAL_BEGIN_INPUT = WM_APP + 4;

constexpr int kDefaultFontPointSize = 12;
constexpr int kMinFontPointSize = 6;
constexpr int kMaxFontPointSize = 32;

constexpr int kStyleBold = 1 << 0;
constexpr int kStyleItalic = 1 << 1;
constexpr int kStyleUnderline = 1 << 2;
constexpr int kStyleStrike = 1 << 3;
constexpr int kStyleDim = 1 << 4;
constexpr int kStyleReverse = 1 << 5;
constexpr int kStyleHidden = 1 << 6;

int blend_channel(int first, int second, int amount) {
    return first + (second - first) * amount / 255;
}

void fill_caption_gradient(HDC dc, const RECT& rect,
                           COLORREF top, COLORREF bottom) {
    int height = rect.bottom - rect.top;
    if (height <= 0) return;

    for (int offset = 0; offset < height; ++offset) {
        int amount = height == 1 ? 0 : offset * 255 / (height - 1);
        COLORREF color = RGB(
            blend_channel(GetRValue(top), GetRValue(bottom), amount),
            blend_channel(GetGValue(top), GetGValue(bottom), amount),
            blend_channel(GetBValue(top), GetBValue(bottom), amount));
        RECT line{rect.left, rect.top + offset, rect.right,
                  rect.top + offset + 1};
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(dc, &line, brush);
        DeleteObject(brush);
    }
}

struct TerminalRun {
    std::string text;
    int fg = CLR_NONE;
    int bg = CLR_NONE;
    int style = 0;
};

COLORREF ansi_color(int code, bool background) {
    switch (code) {
        case 30:
        case 40:
            return RGB(0, 0, 0);
        case 31:
        case 41:
            return RGB(205, 49, 49);
        case 32:
        case 42:
            return RGB(13, 188, 121);
        case 33:
        case 43:
            return RGB(229, 229, 16);
        case 34:
        case 44:
            return RGB(36, 114, 200);
        case 35:
        case 45:
            return RGB(188, 63, 188);
        case 36:
        case 46:
            return RGB(17, 168, 205);
        case 37:
        case 47:
            return RGB(229, 229, 229);
        case 90:
        case 100:
            return RGB(102, 102, 102);
        case 91:
        case 101:
            return RGB(241, 76, 76);
        case 92:
        case 102:
            return RGB(35, 209, 139);
        case 93:
        case 103:
            return RGB(245, 245, 67);
        case 94:
        case 104:
            return RGB(59, 142, 234);
        case 95:
        case 105:
            return RGB(214, 112, 214);
        case 96:
        case 106:
            return RGB(41, 184, 219);
        case 97:
        case 107:
            return RGB(255, 255, 255);
        default:
            return background ? RGB(12, 12, 12) : RGB(220, 220, 220);
    }
}

class AnsiParser {
public:
    void feed(const char* data, size_t size, std::vector<TerminalRun>& out) {
        std::string bytes = pending_;
        pending_.clear();
        if (data && size > 0) bytes.append(data, size);

        size_t index = 0;
        while (index < bytes.size()) {
            unsigned char ch = static_cast<unsigned char>(bytes[index]);
            if (ch == 0x1b) {
                if (index + 1 >= bytes.size()) {
                    pending_ = bytes.substr(index);
                    break;
                }
                if (bytes[index + 1] != '[') {
                    index += 2;
                    continue;
                }
                size_t end = index + 2;
                while (end < bytes.size() &&
                       !(static_cast<unsigned char>(bytes[end]) >= 0x40 &&
                         static_cast<unsigned char>(bytes[end]) <= 0x7e)) {
                    ++end;
                }
                if (end >= bytes.size()) {
                    pending_ = bytes.substr(index);
                    break;
                }
                std::string parameters = bytes.substr(index + 2, end - index - 2);
                char command = bytes[end];
                if (command == 'm') {
                    apply_sgr(parameters, out);
                } else if (command == 'J' &&
                           (parameters == "2" || parameters == "3")) {
                    flush(out);
                    clear_requested_ = true;
                }
                index = end + 1;
                continue;
            }

            if (ch == '\n') {
                flush(out);
                out.push_back(TerminalRun{"\n", fg_, bg_, style_});
                ++index;
                continue;
            }
            if (ch == '\r') {
                ++index;
                continue;
            }
            if (ch == '\b') {
                flush(out);
                out.push_back(TerminalRun{"\b", fg_, bg_, style_});
                ++index;
                continue;
            }
            if (ch == '\t') {
                flush(out);
                out.push_back(TerminalRun{"\t", fg_, bg_, style_});
                ++index;
                continue;
            }

            size_t length = utf8_length(ch);
            if (length == 0) {
                ++index;
                continue;
            }
            if (index + length > bytes.size()) {
                pending_ = bytes.substr(index);
                break;
            }
            text_.append(bytes, index, length);
            index += length;
        }
        flush(out);
    }

    bool consume_clear() {
        bool result = clear_requested_;
        clear_requested_ = false;
        return result;
    }

    void reset() {
        pending_.clear();
        text_.clear();
        fg_ = CLR_NONE;
        bg_ = CLR_NONE;
        style_ = 0;
        clear_requested_ = false;
    }

private:
    static size_t utf8_length(unsigned char ch) {
        if (ch < 0x80) return 1;
        if ((ch & 0xe0) == 0xc0) return 2;
        if ((ch & 0xf0) == 0xe0) return 3;
        if ((ch & 0xf8) == 0xf0) return 4;
        return 0;
    }

    void flush(std::vector<TerminalRun>& out) {
        if (text_.empty()) return;
        out.push_back(TerminalRun{text_, fg_, bg_, style_});
        text_.clear();
    }

    void apply_sgr(const std::string& parameters, std::vector<TerminalRun>& out) {
        flush(out);
        if (parameters.empty()) {
            reset_style();
            return;
        }

        size_t position = 0;
        while (position <= parameters.size()) {
            size_t separator = parameters.find(';', position);
            std::string value = separator == std::string::npos
                                    ? parameters.substr(position)
                                    : parameters.substr(position, separator - position);
            int code = 0;
            if (!value.empty()) {
                try {
                    code = std::stoi(value);
                } catch (...) {
                    code = 0;
                }
            }
            apply_code(code);
            if (separator == std::string::npos) break;
            position = separator + 1;
        }
    }

    void reset_style() {
        fg_ = CLR_NONE;
        bg_ = CLR_NONE;
        style_ = 0;
    }

    void apply_code(int code) {
        if (code == 0) {
            reset_style();
        } else if (code == 1) {
            style_ |= kStyleBold;
        } else if (code == 2) {
            style_ |= kStyleDim;
        } else if (code == 3) {
            style_ |= kStyleItalic;
        } else if (code == 4) {
            style_ |= kStyleUnderline;
        } else if (code == 7) {
            style_ |= kStyleReverse;
        } else if (code == 8) {
            style_ |= kStyleHidden;
        } else if (code == 9) {
            style_ |= kStyleStrike;
        } else if (code == 22) {
            style_ &= ~(kStyleBold | kStyleDim);
        } else if (code == 23) {
            style_ &= ~kStyleItalic;
        } else if (code == 24) {
            style_ &= ~kStyleUnderline;
        } else if (code == 27) {
            style_ &= ~kStyleReverse;
        } else if (code == 29) {
            style_ &= ~kStyleStrike;
        } else if (code == 39) {
            fg_ = CLR_NONE;
        } else if (code == 49) {
            bg_ = CLR_NONE;
        } else if ((code >= 30 && code <= 37) || (code >= 90 && code <= 97)) {
            fg_ = code;
        } else if ((code >= 40 && code <= 47) ||
                   (code >= 100 && code <= 107)) {
            bg_ = code;
        }
    }

    std::string pending_;
    std::string text_;
    int fg_ = CLR_NONE;
    int bg_ = CLR_NONE;
    int style_ = 0;
    bool clear_requested_ = false;
};

class TerminalStreamBuf : public std::streambuf {
public:
    using Sink = std::function<void(const char*, size_t)>;

    void set_sink(Sink sink) { sink_ = std::move(sink); }

protected:
    int_type overflow(int_type ch) override {
        if (traits_type::eq_int_type(ch, traits_type::eof())) {
            return traits_type::not_eof(ch);
        }
        char value = traits_type::to_char_type(ch);
        if (sink_) sink_(&value, 1);
        return ch;
    }

    std::streamsize xsputn(const char* data, std::streamsize size) override {
        if (size > 0 && sink_) sink_(data, static_cast<size_t>(size));
        return size;
    }

    int sync() override { return 0; }

private:
    Sink sink_;
};

}

struct Terminal::Impl {
    HWND hwnd = nullptr;
    HWND output = nullptr;
    HANDLE ready_event = nullptr;
    std::thread ui_thread;
    DWORD ui_thread_id = 0;
    std::atomic<bool> gui_active{false};
    std::atomic<bool> shutting_down{false};
    std::mutex mutex;
    std::condition_variable condition;
    std::string pending_line;
    bool line_ready = false;
    std::string start_error;
    std::vector<TerminalRun> pending_runs;
    bool pending_clear = false;
    AnsiParser parser;
    std::vector<std::string> history;
    size_t history_index = 0;
    WNDPROC editor_old_proc = nullptr;
    int input_start = 0;
    bool input_active = false;
    int font_point_size = kDefaultFontPointSize;
    HFONT font = nullptr;
    HBRUSH background = nullptr;
    int caption_hot = -1;
    const wchar_t* rich_class = L"RICHEDIT50W";
    std::ios_base::fmtflags old_cout_flags{};
    std::ios_base::fmtflags old_cerr_flags{};
    std::streambuf* old_cout = nullptr;
    std::streambuf* old_cerr = nullptr;
    TerminalStreamBuf stream_buffer;
    bool streams_redirected = false;

    bool start(std::string& error);
    void stop();
    void write_utf8(const char* data, size_t length);
    void clear();
    ReadLineResult read_line(std::string& line);

    void ui_thread_main();
    bool create_window(std::string& error);
    void layout();
    void caption_rects(RECT& caption, RECT buttons[3], RECT& icon) const;
    int caption_button_from_point(POINT point) const;
    void paint_window_caption();
    int caret_position();
    int text_end();
    bool selection_in_input();
    void move_caret_to_end();
    void begin_input();
    void submit_input();
    void clear_input();
    void interrupt_input();
    void replace_input(const std::wstring& text);
    void history_move(int direction);
    void drain();
    void close_window();
    void append_run(const TerminalRun& run);
    void set_font_point_size(int point_size);
    void zoom_from_wheel(int delta);

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message,
                                        WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK editor_proc(HWND hwnd, UINT message,
                                        WPARAM wparam, LPARAM lparam);
};

bool Terminal::Impl::start(std::string& error) {
    if (gui_active.load()) return true;

    HMODULE rich_edit = LoadLibraryW(L"Msftedit.dll");
    rich_class = rich_edit ? L"RICHEDIT50W" : L"RICHEDIT20W";
    if (!rich_edit) {
        rich_edit = LoadLibraryW(L"Riched20.dll");
        if (!rich_edit) {
            error = "RichEdit is unavailable.";
            return false;
        }
    }

    SetProcessDPIAware();

    ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready_event) {
        error = "Failed to create terminal startup event.";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex);
        shutting_down.store(false);
        start_error.clear();
        gui_active.store(false);
    }

    try {
        ui_thread = std::thread(&Terminal::Impl::ui_thread_main, this);
    } catch (const std::exception& exception) {
        CloseHandle(ready_event);
        ready_event = nullptr;
        error = std::string("Failed to start terminal thread: ") +
                exception.what();
        return false;
    }

    DWORD wait = WaitForSingleObject(ready_event, 15000);
    if (wait != WAIT_OBJECT_0) {
        shutting_down.store(true);
        if (ui_thread_id) PostThreadMessageW(ui_thread_id, WM_QUIT, 0, 0);
        if (ui_thread.joinable()) ui_thread.join();
        CloseHandle(ready_event);
        ready_event = nullptr;
        error = "Timed out while creating the terminal window.";
        return false;
    }

    if (!gui_active.load()) {
        if (ui_thread.joinable()) ui_thread.join();
        CloseHandle(ready_event);
        ready_event = nullptr;
        std::lock_guard<std::mutex> lock(mutex);
        error = start_error.empty() ? "Failed to create the terminal window."
                                    : start_error;
        return false;
    }

    old_cout = std::cout.rdbuf(&stream_buffer);
    old_cerr = std::cerr.rdbuf(&stream_buffer);
    old_cout_flags = std::cout.flags();
    old_cerr_flags = std::cerr.flags();
    stream_buffer.set_sink([this](const char* data, size_t length) {
        write_utf8(data, length);
    });
    std::cout.flags(old_cout_flags | std::ios_base::unitbuf);
    std::cerr.flags(old_cerr_flags | std::ios_base::unitbuf);
    streams_redirected = true;
    return true;
}

void Terminal::Impl::stop() {
    if (!ui_thread.joinable()) return;

    shutting_down.store(true);
    condition.notify_all();

    if (hwnd) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    } else if (ui_thread_id) {
        PostThreadMessageW(ui_thread_id, WM_QUIT, 0, 0);
    }

    ui_thread.join();
    gui_active.store(false);

    if (streams_redirected) {
        stream_buffer.set_sink({});
        std::cout.rdbuf(old_cout);
        std::cerr.rdbuf(old_cerr);
        std::cout.flags(old_cout_flags);
        std::cerr.flags(old_cerr_flags);
        streams_redirected = false;
    }

    if (ready_event) {
        CloseHandle(ready_event);
        ready_event = nullptr;
    }
    if (font) {
        DeleteObject(font);
        font = nullptr;
    }
    if (background) {
        DeleteObject(background);
        background = nullptr;
    }
    hwnd = nullptr;
    output = nullptr;
    editor_old_proc = nullptr;
    input_start = 0;
    input_active = false;
}

void Terminal::Impl::write_utf8(const char* data, size_t length) {
    if (!hwnd || !gui_active.load()) return;

    bool post = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        parser.feed(data, length, pending_runs);
        if (parser.consume_clear()) {
            pending_runs.clear();
            pending_clear = true;
        }
        post = !pending_runs.empty() || pending_clear;
    }
    if (post) PostMessageW(hwnd, WM_TERMINAL_UPDATE, 0, 0);
}

void Terminal::Impl::clear() {
    if (!hwnd) return;
    {
        std::lock_guard<std::mutex> lock(mutex);
        pending_runs.clear();
        pending_clear = true;
        parser.reset();
    }
    PostMessageW(hwnd, WM_TERMINAL_CLEAR, 0, 0);
}

ReadLineResult Terminal::Impl::read_line(std::string& line) {
    if (hwnd && gui_active.load()) {
        SendMessageW(hwnd, WM_TERMINAL_BEGIN_INPUT, 0, 0);
    }

    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [this]() {
        return line_ready || shutting_down.load();
    });
    if (shutting_down.load() && !line_ready) {
        line.clear();
        return ReadLineResult::EndOfFile;
    }
    line = std::move(pending_line);
    pending_line.clear();
    line_ready = false;
    return ReadLineResult::Line;
}

void Terminal::Impl::ui_thread_main() {
    ui_thread_id = GetCurrentThreadId();
    std::string error;
    if (!create_window(error)) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            start_error = error;
        }
        gui_active.store(false);
        SetEvent(ready_event);
        return;
    }

    gui_active.store(true);
    SetEvent(ready_event);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    gui_active.store(false);
}

bool Terminal::Impl::create_window(std::string& error) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    background = CreateSolidBrush(RGB(12, 12, 12));
    HDC screen = GetDC(nullptr);
    int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
    if (screen) ReleaseDC(nullptr, screen);
    font = CreateFontW(-MulDiv(font_point_size, dpi, 72), 0, 0, 0, FW_NORMAL,
                       FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN,
                       L"Consolas");

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &Terminal::Impl::window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32513));
    HICON icon = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0,
        LR_DEFAULTSIZE | LR_SHARED));
    if (!icon) icon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.hIcon = icon;
    HICON small_icon = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_SHARED));
    if (!small_icon) small_icon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.hIconSm = small_icon;
    window_class.hbrBackground = background;
    window_class.lpszClassName = L"SCTTerminalWindow";
    if (!RegisterClassExW(&window_class) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        error = "Failed to register terminal window class.";
        return false;
    }

    hwnd = CreateWindowExW(
        0, L"SCTTerminalWindow", L"SuperCommandTerminal",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 700, nullptr,
        nullptr, instance, this);
    if (!hwnd) {
        error = "Failed to create terminal window.";
        return false;
    }

    output = CreateWindowExW(
        0, rich_class, L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
            ES_NOHIDESEL | ES_NOOLEDRAGDROP,
        0, 0, 0, 0, hwnd, (HMENU)1001, instance, nullptr);
    if (!output) {
        error = "Failed to create terminal editor control.";
        DestroyWindow(hwnd);
        hwnd = nullptr;
        return false;
    }

    SendMessageW(output, EM_SETBKGNDCOLOR, 0, RGB(12, 12, 12));
    SendMessageW(output, EM_SETUNDOLIMIT, 0, 0);
    SendMessageW(output, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    CHARFORMAT2W format{};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE | CFM_CHARSET;
    format.crTextColor = RGB(220, 220, 220);
    format.yHeight = font_point_size * 20;
    format.bCharSet = DEFAULT_CHARSET;
    wcscpy_s(format.szFaceName, L"Consolas");
    SendMessageW(output, EM_SETCHARFORMAT, SCF_DEFAULT,
                 reinterpret_cast<LPARAM>(&format));

    SetWindowLongPtrW(output, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(this));
    editor_old_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        output, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&Terminal::Impl::editor_proc)));

    layout();
    ShowWindow(hwnd, SW_SHOW);
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    } else if (!IsWindowVisible(hwnd)) {
        ShowWindow(hwnd, SW_SHOW);
    }
    SetForegroundWindow(hwnd);
    UpdateWindow(hwnd);
    SetFocus(output);
    return true;
}

void Terminal::Impl::layout() {
    if (!hwnd || !output) return;
    RECT client{};
    GetClientRect(hwnd, &client);
    const int margin = 8;
    int width = client.right - margin * 2;
    int height = client.bottom - margin * 2;
    if (width < 0) width = 0;
    if (height < 0) height = 0;
    MoveWindow(output, margin, margin, width, height, TRUE);
}

void Terminal::Impl::caption_rects(RECT& caption, RECT buttons[3],
                                   RECT& icon) const {
    caption = {};
    for (int index = 0; index < 3; ++index) buttons[index] = {};
    icon = {};
    if (!hwnd) return;

    RECT client{};
    GetClientRect(hwnd, &client);
    POINT origin{0, 0};
    ClientToScreen(hwnd, &origin);

    int caption_height = GetSystemMetrics(SM_CYCAPTION);
    int button_width = GetSystemMetrics(SM_CXSIZE);
    int button_height = GetSystemMetrics(SM_CYSIZE);

    caption.left = origin.x;
    caption.right = origin.x + client.right - client.left;
    caption.top = origin.y - caption_height;
    caption.bottom = origin.y;

    int button_top = caption.top + (caption_height - button_height) / 2;
    for (int index = 0; index < 3; ++index) {
        int right = caption.right - (2 - index) * button_width;
        buttons[index] = {right - button_width, button_top, right,
                          button_top + button_height};
    }

    int icon_width = GetSystemMetrics(SM_CXSMICON);
    int icon_height = GetSystemMetrics(SM_CYSMICON);
    icon.left = caption.left + 4;
    icon.top = caption.top + (caption_height - icon_height) / 2;
    icon.right = icon.left + icon_width;
    icon.bottom = icon.top + icon_height;
}

int Terminal::Impl::caption_button_from_point(POINT point) const {
    RECT caption{};
    RECT buttons[3]{};
    RECT icon{};
    caption_rects(caption, buttons, icon);
    for (int index = 0; index < 3; ++index) {
        if (PtInRect(&buttons[index], point)) return index;
    }
    return -1;
}

void Terminal::Impl::paint_window_caption() {
    if (!hwnd) return;

    RECT caption{};
    RECT buttons[3]{};
    RECT icon{};
    caption_rects(caption, buttons, icon);

    RECT window{};
    GetWindowRect(hwnd, &window);
    OffsetRect(&caption, -window.left, -window.top);
    OffsetRect(&icon, -window.left, -window.top);
    for (int index = 0; index < 3; ++index) {
        OffsetRect(&buttons[index], -window.left, -window.top);
    }

    HDC dc = GetWindowDC(hwnd);
    if (!dc) return;

    int saved_dc = SaveDC(dc);
    IntersectClipRect(dc, caption.left, caption.top, caption.right,
                      caption.bottom);

    bool active = GetActiveWindow() == hwnd;
    COLORREF top = GetSysColor(active ? COLOR_GRADIENTACTIVECAPTION
                                      : COLOR_GRADIENTINACTIVECAPTION);
    COLORREF bottom = GetSysColor(active ? COLOR_ACTIVECAPTION
                                         : COLOR_INACTIVECAPTION);
    fill_caption_gradient(dc, caption, top, bottom);

    HICON window_icon = reinterpret_cast<HICON>(
        GetClassLongPtrW(hwnd, GCLP_HICONSM));
    if (!window_icon) {
        window_icon = reinterpret_cast<HICON>(
            GetClassLongPtrW(hwnd, GCLP_HICON));
    }
    if (window_icon) {
        int icon_width = GetSystemMetrics(SM_CXSMICON);
        int icon_height = GetSystemMetrics(SM_CYSMICON);
        DrawIconEx(dc, icon.left, icon.top, window_icon, icon_width,
                   icon_height, 0, nullptr, DI_NORMAL);
    }

    RECT text_rect = caption;
    text_rect.left = icon.right + 6;
    text_rect.right = buttons[0].left - 6;
    if (text_rect.right > text_rect.left) {
        wchar_t title[512]{};
        int length = GetWindowTextW(hwnd, title,
                                    static_cast<int>(std::size(title)));
        if (length > 0) {
            int old_mode = SetBkMode(dc, TRANSPARENT);
            COLORREF old_color = SetTextColor(
                dc, GetSysColor(active ? COLOR_CAPTIONTEXT
                                       : COLOR_INACTIVECAPTIONTEXT));
            HFONT old_font = reinterpret_cast<HFONT>(
                SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT)));
            DrawTextW(dc, title, length, &text_rect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                          DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(dc, old_font);
            SetTextColor(dc, old_color);
            SetBkMode(dc, old_mode);
        }
    }

    bool maximized = IsZoomed(hwnd) != FALSE;
    for (int index = 0; index < 3; ++index) {
        UINT state = 0;
        if (index == 0) {
            state |= DFCS_CAPTIONMIN;
        } else if (index == 1) {
            state |= maximized ? DFCS_CAPTIONRESTORE : DFCS_CAPTIONMAX;
        } else {
            state |= DFCS_CAPTIONCLOSE;
        }
        if (!active) state |= DFCS_INACTIVE;
        if (caption_hot == index) state |= DFCS_HOT;
        DrawFrameControl(dc, &buttons[index], DFC_CAPTION, state);
    }

    RestoreDC(dc, saved_dc);
    ReleaseDC(hwnd, dc);
}

int Terminal::Impl::caret_position() {
    CHARRANGE range{};
    SendMessageW(output, EM_EXGETSEL, 0,
                 reinterpret_cast<LPARAM>(&range));
    return range.cpMax;
}

int Terminal::Impl::text_end() {
    SendMessageW(output, EM_SETSEL, static_cast<WPARAM>(-1),
                 static_cast<LPARAM>(-1));
    return caret_position();
}

bool Terminal::Impl::selection_in_input() {
    CHARRANGE range{};
    SendMessageW(output, EM_EXGETSEL, 0,
                 reinterpret_cast<LPARAM>(&range));
    return range.cpMin >= input_start;
}

void Terminal::Impl::move_caret_to_end() {
    text_end();
    SendMessageW(output, EM_SCROLLCARET, 0, 0);
}

void Terminal::Impl::begin_input() {
    drain();
    input_start = text_end();
    input_active = true;
    move_caret_to_end();
    SetFocus(output);
}

void Terminal::Impl::submit_input() {
    int end = text_end();
    int start = input_start;
    if (start > end) start = end;

    std::wstring wide;
    if (end > start) {
        wide.resize(static_cast<size_t>(end - start) + 1, L'\0');
        TEXTRANGEW range{};
        range.chrg.cpMin = start;
        range.chrg.cpMax = end;
        range.lpstrText = wide.data();
        SendMessageW(output, EM_GETTEXTRANGE, 0,
                     reinterpret_cast<LPARAM>(&range));
        wide.resize(std::wcslen(wide.c_str()));
    }

    std::string line = wide_to_utf8(wide);
    SendMessageW(output, EM_REPLACESEL, FALSE,
                 reinterpret_cast<LPARAM>(L"\n"));
    input_active = false;

    if (!line.empty() && (history.empty() || history.back() != line)) {
        history.push_back(line);
    }
    history_index = history.size();

    {
        std::lock_guard<std::mutex> lock(mutex);
        pending_line = line;
        line_ready = true;
    }
    condition.notify_one();
}

void Terminal::Impl::clear_input() {
    if (!input_active) return;
    int end = text_end();
    if (end > input_start) {
        CHARRANGE range{input_start, end};
        SendMessageW(output, EM_EXSETSEL, 0,
                     reinterpret_cast<LPARAM>(&range));
        SendMessageW(output, EM_REPLACESEL, FALSE,
                     reinterpret_cast<LPARAM>(L""));
    }
    move_caret_to_end();
}

void Terminal::Impl::interrupt_input() {
    clear_input();
    input_active = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        pending_line.clear();
        line_ready = true;
    }
    condition.notify_one();
}

void Terminal::Impl::replace_input(const std::wstring& text) {
    int end = text_end();
    if (end < input_start) input_start = end;
    CHARRANGE range{input_start, end};
    SendMessageW(output, EM_EXSETSEL, 0,
                 reinterpret_cast<LPARAM>(&range));
    SendMessageW(output, EM_REPLACESEL, FALSE,
                 reinterpret_cast<LPARAM>(text.c_str()));
    move_caret_to_end();
}

void Terminal::Impl::history_move(int direction) {
    if (!input_active || history.empty()) return;
    if (direction < 0) {
        if (history_index == 0) return;
        --history_index;
    } else {
        if (history_index >= history.size()) return;
        ++history_index;
    }

    std::wstring text;
    if (history_index < history.size()) {
        text = utf8_to_wide(history[history_index]);
    }
    replace_input(text);
}

void Terminal::Impl::drain() {
    std::vector<TerminalRun> runs;
    bool clear_output = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        runs.swap(pending_runs);
        clear_output = pending_clear;
        pending_clear = false;
    }

    if (clear_output) {
        SetWindowTextW(output, L"");
        input_start = 0;
    }
    if (runs.empty()) return;

    SendMessageW(output, WM_SETREDRAW, FALSE, 0);
    for (const TerminalRun& run : runs) append_run(run);
    SendMessageW(output, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(output, nullptr, TRUE);
    move_caret_to_end();
}

void Terminal::Impl::append_run(const TerminalRun& run) {
    if (run.text.empty()) return;
    if (run.text == "\b") {
        int end = text_end();
        if (end > 0) {
            CHARRANGE selection{end - 1, end};
            SendMessageW(output, EM_EXSETSEL, 0,
                         reinterpret_cast<LPARAM>(&selection));
            SendMessageW(output, EM_REPLACESEL, FALSE,
                         reinterpret_cast<LPARAM>(L""));
        }
        return;
    }

    std::wstring text = utf8_to_wide(run.text);
    if (text.empty()) return;

    int start = text_end();
    CHARRANGE selection{start, start};
    SendMessageW(output, EM_EXSETSEL, 0,
                 reinterpret_cast<LPARAM>(&selection));
    SendMessageW(output, EM_REPLACESEL, FALSE,
                 reinterpret_cast<LPARAM>(text.c_str()));
    int end = text_end();

    selection.cpMin = start;
    selection.cpMax = end;
    SendMessageW(output, EM_EXSETSEL, 0,
                 reinterpret_cast<LPARAM>(&selection));

    int foreground = run.fg;
    int background = run.bg;
    if ((run.style & kStyleReverse) != 0) {
        std::swap(foreground, background);
    }

    CHARFORMAT2W format{};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_COLOR | CFM_BACKCOLOR | CFM_BOLD | CFM_ITALIC |
                    CFM_UNDERLINE | CFM_STRIKEOUT | CFM_FACE | CFM_SIZE |
                    CFM_CHARSET;
    format.crTextColor = ansi_color(foreground, false);
    format.crBackColor = ansi_color(background, true);
    format.yHeight = font_point_size * 20;
    format.bCharSet = DEFAULT_CHARSET;
    wcscpy_s(format.szFaceName, L"Consolas");
    format.dwEffects = 0;
    if ((run.style & kStyleBold) != 0) format.dwEffects |= CFE_BOLD;
    if ((run.style & kStyleItalic) != 0) format.dwEffects |= CFE_ITALIC;
    if ((run.style & kStyleUnderline) != 0) format.dwEffects |= CFE_UNDERLINE;
    if ((run.style & kStyleStrike) != 0) format.dwEffects |= CFE_STRIKEOUT;
    SendMessageW(output, EM_SETCHARFORMAT, SCF_SELECTION,
                 reinterpret_cast<LPARAM>(&format));

    selection.cpMin = end;
    selection.cpMax = end;
    SendMessageW(output, EM_EXSETSEL, 0,
                 reinterpret_cast<LPARAM>(&selection));
}

void Terminal::Impl::set_font_point_size(int point_size) {
    point_size = std::clamp(point_size, kMinFontPointSize, kMaxFontPointSize);
    if (!output || point_size == font_point_size) return;
    font_point_size = point_size;

    CHARRANGE selection{};
    SendMessageW(output, EM_EXGETSEL, 0,
                 reinterpret_cast<LPARAM>(&selection));
    POINT scroll{};
    SendMessageW(output, EM_GETSCROLLPOS, 0,
                 reinterpret_cast<LPARAM>(&scroll));

    CHARFORMAT2W format{};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_FACE | CFM_SIZE | CFM_CHARSET;
    format.yHeight = font_point_size * 20;
    format.bCharSet = DEFAULT_CHARSET;
    wcscpy_s(format.szFaceName, L"Consolas");

    SendMessageW(output, WM_SETREDRAW, FALSE, 0);
    CHARRANGE all{0, -1};
    SendMessageW(output, EM_EXSETSEL, 0,
                 reinterpret_cast<LPARAM>(&all));
    SendMessageW(output, EM_SETCHARFORMAT, SCF_SELECTION,
                 reinterpret_cast<LPARAM>(&format));
    SendMessageW(output, EM_EXSETSEL, 0,
                 reinterpret_cast<LPARAM>(&selection));
    SendMessageW(output, EM_SETSCROLLPOS, 0,
                 reinterpret_cast<LPARAM>(&scroll));
    SendMessageW(output, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(output, nullptr, TRUE);
}

void Terminal::Impl::zoom_from_wheel(int delta) {
    if (delta == 0) return;
    int steps = delta / WHEEL_DELTA;
    if (steps == 0) steps = delta > 0 ? 1 : -1;
    set_font_point_size(font_point_size + steps);
}

void Terminal::Impl::close_window() {
    shutting_down.store(true);
    condition.notify_all();
    DestroyWindow(hwnd);
}

LRESULT CALLBACK Terminal::Impl::window_proc(HWND hwnd, UINT message,
                                             WPARAM wparam, LPARAM lparam) {
    Impl* self = reinterpret_cast<Impl*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<Impl*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    }

    switch (message) {
        case WM_SIZE:
            if (self) {
                self->layout();
                self->paint_window_caption();
            }
            return 0;
        case WM_SETFOCUS:
            if (self && self->output) SetFocus(self->output);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x = 500;
            info->ptMinTrackSize.y = 300;
            return 0;
        }
        case WM_NCHITTEST:
            if (self) {
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                RECT caption{};
                RECT buttons[3]{};
                RECT icon{};
                self->caption_rects(caption, buttons, icon);
                if (PtInRect(&icon, point)) return HTSYSMENU;
                for (int index = 0; index < 3; ++index) {
                    if (PtInRect(&buttons[index], point)) {
                        if (index == 0) return HTMINBUTTON;
                        if (index == 1) return HTMAXBUTTON;
                        return HTCLOSE;
                    }
                }
                if (PtInRect(&caption, point)) return HTCAPTION;
            }
            break;
        case WM_NCMOUSEMOVE:
            if (self) {
                TRACKMOUSEEVENT tracking{};
                tracking.cbSize = sizeof(tracking);
                tracking.dwFlags = TME_LEAVE | TME_NONCLIENT;
                tracking.hwndTrack = hwnd;
                TrackMouseEvent(&tracking);

                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                int hot = self->caption_button_from_point(point);
                if (hot != self->caption_hot) {
                    self->caption_hot = hot;
                    self->paint_window_caption();
                }
                return 0;
            }
            break;
        case WM_NCMOUSELEAVE:
            if (self) {
                if (self->caption_hot != -1) {
                    self->caption_hot = -1;
                    self->paint_window_caption();
                }
                return 0;
            }
            break;
        case WM_NCLBUTTONDOWN:
            if (self) {
                if (wparam == HTMINBUTTON) {
                    self->caption_hot = -1;
                    ShowWindow(hwnd, SW_MINIMIZE);
                    return 0;
                }
                if (wparam == HTMAXBUTTON) {
                    ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE
                                                    : SW_MAXIMIZE);
                    self->caption_hot = -1;
                    self->paint_window_caption();
                    return 0;
                }
                if (wparam == HTCLOSE) {
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    return 0;
                }
            }
            break;
        case WM_NCPAINT: {
            LRESULT result = DefWindowProcW(hwnd, message, wparam, lparam);
            if (self) self->paint_window_caption();
            return result;
        }
        case WM_NCACTIVATE: {
            LRESULT result = DefWindowProcW(hwnd, message, wparam, lparam);
            if (self) self->paint_window_caption();
            return result;
        }
        case WM_ERASEBKGND:
            if (self && self->background) {
                RECT rect{};
                GetClientRect(hwnd, &rect);
                FillRect(reinterpret_cast<HDC>(wparam), &rect,
                         self->background);
                return 1;
            }
            break;
        case WM_CTLCOLOREDIT:
            if (self && self->background) {
                HDC dc = reinterpret_cast<HDC>(wparam);
                SetTextColor(dc, RGB(220, 220, 220));
                SetBkColor(dc, RGB(12, 12, 12));
                return reinterpret_cast<LRESULT>(self->background);
            }
            break;
        case WM_CLOSE:
            if (self) {
                self->close_window();
                return 0;
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_TERMINAL_UPDATE:
            if (self) self->drain();
            return 0;
        case WM_TERMINAL_CLEAR:
            if (self) {
                SetWindowTextW(self->output, L"");
                self->input_start = 0;
                InvalidateRect(self->output, nullptr, TRUE);
            }
            return 0;
        case WM_TERMINAL_BEGIN_INPUT:
            if (self) self->begin_input();
            return 0;
        case WM_TERMINAL_TITLE:
            return 0;
        case WM_MOUSEWHEEL:
            if (self && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                self->zoom_from_wheel(GET_WHEEL_DELTA_WPARAM(wparam));
                return 0;
            }
            break;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK Terminal::Impl::editor_proc(HWND hwnd, UINT message,
                                             WPARAM wparam, LPARAM lparam) {
    Impl* self = reinterpret_cast<Impl*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (self) {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

        if (message == WM_MOUSEWHEEL && ctrl) {
            self->zoom_from_wheel(GET_WHEEL_DELTA_WPARAM(wparam));
            return 0;
        }

        if (message == WM_LBUTTONDOWN &&
            (GetKeyState(VK_SHIFT) & 0x8000) == 0) {
            CHARRANGE range{};
            SendMessageW(hwnd, EM_EXGETSEL, 0,
                         reinterpret_cast<LPARAM>(&range));
            if (range.cpMin != range.cpMax) {
                POINTS points = MAKEPOINTS(lparam);
                POINT point{points.x, points.y};
                LRESULT index = SendMessageW(
                    hwnd, EM_CHARFROMPOS, 0,
                    reinterpret_cast<LPARAM>(&point));
                if (index >= range.cpMin && index < range.cpMax) {
                    CHARRANGE caret{static_cast<LONG>(index),
                                    static_cast<LONG>(index)};
                    SendMessageW(hwnd, EM_EXSETSEL, 0,
                                 reinterpret_cast<LPARAM>(&caret));
                }
            }
        }

        if (message == WM_KEYDOWN) {
            if (!self->input_active) {
                if (ctrl && wparam == 'C') {
                    request_command_cancel();
                    return 0;
                }
                bool navigation =
                    wparam == VK_LEFT || wparam == VK_RIGHT ||
                    wparam == VK_UP || wparam == VK_DOWN ||
                    wparam == VK_HOME || wparam == VK_END ||
                    wparam == VK_PRIOR || wparam == VK_NEXT;
                bool selection_key =
                    ctrl && (wparam == 'A' || wparam == VK_INSERT);
                if (!navigation && !selection_key) return 0;
            } else {
                if (wparam == VK_RETURN) {
                    self->submit_input();
                    return 0;
                }
                if (wparam == VK_UP) {
                    self->history_move(-1);
                    return 0;
                }
                if (wparam == VK_DOWN) {
                    self->history_move(1);
                    return 0;
                }
                if (wparam == VK_ESCAPE) {
                    self->clear_input();
                    return 0;
                }
                if (wparam == VK_HOME) {
                    CHARRANGE range{self->input_start, self->input_start};
                    SendMessageW(hwnd, EM_EXSETSEL, 0,
                                 reinterpret_cast<LPARAM>(&range));
                    SendMessageW(hwnd, EM_SCROLLCARET, 0, 0);
                    return 0;
                }
                if (wparam == VK_END) {
                    self->move_caret_to_end();
                    return 0;
                }
                if (wparam == VK_BACK || wparam == VK_DELETE) {
                    CHARRANGE range{};
                    SendMessageW(hwnd, EM_EXGETSEL, 0,
                                 reinterpret_cast<LPARAM>(&range));
                    bool before_input = range.cpMin < self->input_start;
                    bool empty_backspace =
                        wparam == VK_BACK && range.cpMin == range.cpMax &&
                        range.cpMax <= self->input_start;
                    if (before_input || empty_backspace) {
                        self->move_caret_to_end();
                        return 0;
                    }
                }
                if (ctrl && wparam == 'C') {
                    CHARRANGE range{};
                    SendMessageW(hwnd, EM_EXGETSEL, 0,
                                 reinterpret_cast<LPARAM>(&range));
                    if (range.cpMin == range.cpMax) {
                        request_command_cancel();
                        request_prompt_interrupt();
                        self->interrupt_input();
                        return 0;
                    }
                }
            }
        }

        if (message == WM_CHAR) {
            if (!self->input_active) return 0;
            if (wparam == VK_RETURN || wparam == VK_ESCAPE) return 0;
            if (!self->selection_in_input()) self->move_caret_to_end();
        }

        if (message == WM_PASTE) {
            if (!self->input_active) return 0;
            if (!self->selection_in_input()) self->move_caret_to_end();
        }

        if (message == WM_CUT || message == WM_CLEAR ||
            message == WM_UNDO) {
            if (!self->input_active || !self->selection_in_input()) {
                return 0;
            }
            if (message == WM_UNDO) return 0;
        }

        if (message == WM_IME_STARTCOMPOSITION ||
            message == WM_IME_COMPOSITION || message == WM_IME_CHAR) {
            if (!self->input_active) return 0;
            if (!self->selection_in_input()) self->move_caret_to_end();
        }

        if (message == WM_DROPFILES) {
            if (!self->input_active || !self->selection_in_input()) {
                return 0;
            }
        }

        if (self->editor_old_proc) {
            return CallWindowProcW(self->editor_old_proc, hwnd, message,
                                   wparam, lparam);
        }
    }

    return DefWindowProcW(hwnd, message, wparam, lparam);
}

Terminal::Terminal() : impl_(std::make_unique<Impl>()) {}

Terminal::~Terminal() {
    impl_->stop();
}

bool Terminal::start_gui(std::string& error) {
    return impl_->start(error);
}

void Terminal::stop_gui() {
    impl_->stop();
}

bool Terminal::gui_active() const {
    return impl_->gui_active.load();
}

ReadLineResult Terminal::read_line(std::string& line) {
    return impl_->read_line(line);
}

void Terminal::write_utf8(const char* data, size_t length) {
    impl_->write_utf8(data, length);
}

void Terminal::clear() {
    impl_->clear();
}

Terminal& terminal() {
    static Terminal instance;
    return instance;
}

bool terminal_gui_active() {
    return terminal().gui_active();
}
