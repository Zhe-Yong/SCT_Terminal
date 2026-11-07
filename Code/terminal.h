#pragma once

class Terminal {
public:
    Terminal();
    ~Terminal();

    bool start_gui(std::string& error);
    void stop_gui();
    bool gui_active() const;

    ReadLineResult read_line(std::string& line);
    void write_utf8(const char* data, size_t length);
    void clear();
    void refresh_title_bar();
    bool set_front_enabled(bool enable, unsigned interval_ms,
                           std::string& error);
    bool front_enabled() const;
    unsigned front_interval_ms() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

Terminal& terminal();
bool terminal_gui_active();
