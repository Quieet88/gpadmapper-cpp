// gpadmapper-cpp — gamepad to pointer/keyboard bridge (evdev -> uinput).
//
//   right stick -> pointer
//   left stick  -> scroll (wheel or two-finger touchpad gesture)
//   buttons     -> configurable mouse buttons / key combos
//
// Config: ~/.config/gpadmapper-cpp/config.toml
// Build:  make
// Run:    ./gpadmapper-cpp [-v] [--selftest] [--config PATH] [--device PATH]

#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <unistd.h>

extern char** environ;

// ---------------------------------------------------------------- константы --

static constexpr double TICK = 0.004;             // шаг интегрирования, 250 Гц
static constexpr double IDLE_TICK = 0.05;         // стики в покое — 20 Гц
static constexpr double RECONNECT_DELAY = 1.5;    // пауза перед поиском геймпада
static constexpr const char* VIRTUAL_NAME = "gpadmapper-cpp";
static constexpr const char* TOUCHPAD_NAME = "gpadmapper-scroll";

// Виртуальный тачпад: скролл двумя «пальцами». libinput превращает движение
// пары контактов в непрерывный скролл, поэтому нет дискретных «щелчков» колеса.
static constexpr int TP_MAX = 65535;              // логический диапазон осей
static constexpr int TP_RES = 40;                 // единиц на мм
static constexpr int TP_FINGER_GAP = 160;         // ~4 мм между контактами
static constexpr int TP_MARGIN = 4000;            // отступ до края перед сбросом
static constexpr double TP_UNITS_PER_NOTCH = 115.0; // единиц на «щелчок» (калибровка)
static constexpr int TP_START = TP_MAX / 2;
static constexpr double TP_FLING_GUARD = 0.30;    // пауза перед снятием второго пальца, с
static constexpr double TP_TAP_GUARD = 0.25;      // пауза между подъёмом пальцев, с
static constexpr double TP_SIGN_X = +1.0;         // направление движения пальцев
static constexpr double TP_SIGN_Y = -1.0;

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int) { g_stop = 1; }

static const char* DEFAULT_CONFIG = R"(# gpadmapper-cpp configuration. Restart after editing:
#   systemctl --user restart gpadmapper-cpp

[device]
# Substring of the device name; empty = auto-detect any gamepad.
name = ""
# true = grab the device exclusively (games will no longer see the gamepad).
grab = false

[pointer]         # right stick = pointer
deadzone = 0.06   # 0..1
curve    = 2.2    # 1 = linear, higher = finer control near the center
max_speed = 1600  # maximum speed, pixels per second
invert_x = false
invert_y = false

[scroll]          # left stick = scroll
deadzone = 0.10
curve    = 2.0
max_speed = 12.0  # wheel notches per second at full deflection
invert_x = false
invert_y = true   # stick up = scroll up
hi_res   = true   # smooth wheel scrolling (wheel mode)
mode     = "touchpad"  # touchpad = smooth gesture scroll; wheel = REL_WHEEL

[triggers]
threshold = 0.25  # how far RT/LT must travel to count as pressed

[toggle]
# Pause/resume: hold the combo for `hold` seconds.
# While paused the daemon emits nothing, so the gamepad is free for games.
enabled = true
combo   = "THUMBL+THUMBR"
hold    = 1.0
notify  = true    # desktop notification on toggle

# Button = action. Action: BTN_LEFT / BTN_RIGHT / BTN_MIDDLE /
# BTN_SIDE / BTN_EXTRA / any KEY_* / none.
# Combine keys with "+": "KEY_LEFTALT+KEY_TAB".
# Pad buttons: BTN_A BTN_B BTN_X BTN_Y, LB RB LT RT,
# SELECT START MODE THUMBL THUMBR, DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT
# PlayStation: BTN_A = Cross, BTN_B = Circle, BTN_X = Square, BTN_Y = Triangle.
# DS3 L2/R2 are digital buttons (BTN_TL2/BTN_TR2) and work as LT/RT too.
[buttons]
RT = "BTN_LEFT"
RB = "BTN_RIGHT"

MODE      = "KEY_LEFTMETA"  # 8BitDo "home" button -> Super
START     = "KEY_TAB"
SELECT    = "KEY_SPACE"
DPAD_UP    = "KEY_UP"
DPAD_DOWN  = "KEY_DOWN"
DPAD_LEFT  = "KEY_LEFT"
DPAD_RIGHT = "KEY_RIGHT"

BTN_A = "KEY_ENTER"
BTN_B = "KEY_ESC"
BTN_X = "KEY_LEFTALT+KEY_TAB"

# Examples:
# LT     = "BTN_MIDDLE"
# THUMBR = "KEY_ENTER"
# THUMBL = "KEY_ESC"
)";

// ------------------------------------------------------------------- конфиг --

struct Value {
    enum Kind { Num, Bool, Str } kind = Str;
    double num = 0.0;
    bool flag = false;
    std::string str;
};

using Section = std::map<std::string, Value>;
using Toml = std::map<std::string, Section>;

static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Обрезает комментарий '#' вне кавычек.
static std::string strip_comment(const std::string& line) {
    bool in_str = false;
    char q = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (in_str) {
            if (c == '\\') { ++i; continue; }
            if (c == q) in_str = false;
        } else if (c == '"' || c == '\'') {
            in_str = true;
            q = c;
        } else if (c == '#') {
            return line.substr(0, i);
        }
    }
    return line;
}

static std::string unquote(const std::string& s) {
    if (s.size() < 2) return s;
    char q = s.front();
    if ((q != '"' && q != '\'') || s.back() != q) return s;
    std::string out;
    for (size_t i = 1; i + 1 < s.size(); ++i) {
        char c = s[i];
        if (c == '\\' && i + 2 < s.size()) {
            char n = s[++i];
            switch (n) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                default: out.push_back(n); break;
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

static Value parse_value(const std::string& raw) {
    std::string s = trim(raw);
    Value v;
    if (!s.empty() && (s.front() == '"' || s.front() == '\'')) {
        v.kind = Value::Str;
        v.str = unquote(s);
        return v;
    }
    if (s == "true" || s == "false") {
        v.kind = Value::Bool;
        v.flag = (s == "true");
        return v;
    }
    v.kind = Value::Num;
    v.num = std::strtod(s.c_str(), nullptr);
    return v;
}

// Минимальный TOML: секции [x], пары key = value, комментарии, кавычки.
static bool parse_toml(const std::string& text, Toml& out, std::string& err) {
    std::string section = "";
    size_t line_no = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        ++line_no;

        line = trim(strip_comment(line));
        if (line.empty()) continue;

        if (line.front() == '[') {
            size_t close = line.find(']');
            if (close == std::string::npos) {
                err = "строка " + std::to_string(line_no) + ": нет ']'";
                return false;
            }
            section = trim(line.substr(1, close - 1));
            out[section];  // создать пустую секцию
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            err = "строка " + std::to_string(line_no) + ": нет '='";
            return false;
        }
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if (key.empty()) {
            err = "строка " + std::to_string(line_no) + ": пустой ключ";
            return false;
        }
        out[section][key] = parse_value(val);
    }
    return true;
}

struct Config {
    std::string device_name;
    bool grab = false;

    struct {
        double deadzone = 0.06;
        double curve = 2.2;
        double max_speed = 1600.0;
        bool invert_x = false;
        bool invert_y = false;
    } pointer;

    struct {
        double deadzone = 0.10;
        double curve = 2.0;
        double max_speed = 12.0;
        bool invert_x = false;
        bool invert_y = true;
        bool hi_res = true;
        std::string mode = "touchpad";  // wheel | touchpad
    } scroll;

    double trigger_threshold = 0.25;

    struct {
        bool enabled = true;
        std::vector<std::string> combo;
        double hold = 1.0;
        bool notify = true;
    } toggle;

    std::map<std::string, std::string> buttons;

    static Config defaults() {
        Config c;
        c.buttons = {
            {"RT", "BTN_LEFT"},       {"RB", "BTN_RIGHT"},
            {"MODE", "KEY_LEFTMETA"}, {"START", "KEY_TAB"},
            {"SELECT", "KEY_SPACE"},  {"DPAD_UP", "KEY_UP"},
            {"DPAD_DOWN", "KEY_DOWN"}, {"DPAD_LEFT", "KEY_LEFT"},
            {"DPAD_RIGHT", "KEY_RIGHT"}, {"BTN_A", "KEY_ENTER"},
            {"BTN_B", "KEY_ESC"},     {"BTN_X", "KEY_LEFTALT+KEY_TAB"},
        };
        c.toggle.combo = {"THUMBL", "THUMBR"};
        return c;
    }
};

static const Value* find_val(const Toml& t, const std::string& sec, const std::string& key) {
    auto s = t.find(sec);
    if (s == t.end()) return nullptr;
    auto k = s->second.find(key);
    return k == s->second.end() ? nullptr : &k->second;
}

static void apply_num(const Toml& t, const std::string& sec, const std::string& key, double& dst) {
    if (const Value* v = find_val(t, sec, key)) {
        if (v->kind == Value::Num) dst = v->num;
    }
}

static void apply_bool(const Toml& t, const std::string& sec, const std::string& key, bool& dst) {
    if (const Value* v = find_val(t, sec, key)) {
        if (v->kind == Value::Bool) dst = v->flag;
    }
}

static void apply_str(const Toml& t, const std::string& sec, const std::string& key, std::string& dst) {
    if (const Value* v = find_val(t, sec, key)) {
        if (v->kind == Value::Str) dst = v->str;
    }
}

static double clamp_value(double v, double lo, double hi) {
    if (!std::isfinite(v)) return lo;
    return std::clamp(v, lo, hi);
}

// Keep user values in a sane range: avoids division by zero and stuck keys.
static void sanitize(Config& c) {
    c.pointer.deadzone = clamp_value(c.pointer.deadzone, 0.0, 0.95);
    c.scroll.deadzone = clamp_value(c.scroll.deadzone, 0.0, 0.95);
    c.pointer.curve = clamp_value(c.pointer.curve, 0.1, 10.0);
    c.scroll.curve = clamp_value(c.scroll.curve, 0.1, 10.0);
    c.pointer.max_speed = std::max(0.0, c.pointer.max_speed);
    c.scroll.max_speed = std::max(0.0, c.scroll.max_speed);
    c.trigger_threshold = clamp_value(c.trigger_threshold, 0.01, 0.99);
    c.toggle.hold = std::max(0.0, c.toggle.hold);
}

static Config merge_config(const Toml& t, bool verbose) {
    Config c = Config::defaults();

    apply_str(t, "device", "name", c.device_name);
    apply_bool(t, "device", "grab", c.grab);

    apply_num(t, "pointer", "deadzone", c.pointer.deadzone);
    apply_num(t, "pointer", "curve", c.pointer.curve);
    apply_num(t, "pointer", "max_speed", c.pointer.max_speed);
    apply_bool(t, "pointer", "invert_x", c.pointer.invert_x);
    apply_bool(t, "pointer", "invert_y", c.pointer.invert_y);

    apply_num(t, "scroll", "deadzone", c.scroll.deadzone);
    apply_num(t, "scroll", "curve", c.scroll.curve);
    apply_num(t, "scroll", "max_speed", c.scroll.max_speed);
    apply_bool(t, "scroll", "invert_x", c.scroll.invert_x);
    apply_bool(t, "scroll", "invert_y", c.scroll.invert_y);
    apply_bool(t, "scroll", "hi_res", c.scroll.hi_res);
    apply_str(t, "scroll", "mode", c.scroll.mode);
    for (auto& ch : c.scroll.mode)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

    apply_num(t, "triggers", "threshold", c.trigger_threshold);

    apply_bool(t, "toggle", "enabled", c.toggle.enabled);
    apply_num(t, "toggle", "hold", c.toggle.hold);
    apply_bool(t, "toggle", "notify", c.toggle.notify);
    if (const Value* v = find_val(t, "toggle", "combo")) {
        if (v->kind == Value::Str) {
            std::vector<std::string> names;
            std::string part;
            for (char ch : v->str + "+") {
                if (ch != '+') {
                    part.push_back(ch);
                    continue;
                }
                std::string n = trim(part);
                part.clear();
                if (n.empty()) continue;
                for (auto& c2 : n)
                    c2 = static_cast<char>(std::toupper(static_cast<unsigned char>(c2)));
                names.push_back(n);
            }
            c.toggle.combo = names;
        }
    }

    // кнопки: дефолтные RT/RB + любые новые имена
    if (auto s = t.find("buttons"); s != t.end()) {
        for (const auto& [k, v] : s->second) {
            std::string name = k;
            for (auto& ch : name) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            c.buttons[name] = v.kind == Value::Str ? v.str : std::to_string(v.num);
        }
    }

    sanitize(c);
    if (verbose) {
        std::printf("конфиг: указатель %.0f px/с (deadzone %.2f, curve %.2f), "
                    "скролл %.1f щелчков/с, кнопок %zu\n",
                    c.pointer.max_speed, c.pointer.deadzone, c.pointer.curve,
                    c.scroll.max_speed, c.buttons.size());
    }
    return c;
}

static std::string config_path() {
    const char* h = std::getenv("HOME");
    return std::string(h ? h : "") + "/.config/gpadmapper-cpp/config.toml";
}

static Config load_config(const std::string& path, bool verbose) {
    namespace fs = std::filesystem;

    if (!fs::exists(path)) {
        std::error_code ec;
        fs::create_directories(fs::path(path).parent_path(), ec);
        std::ofstream f(path);
        if (f) {
            f << DEFAULT_CONFIG;
            if (verbose) std::printf("создан конфиг: %s\n", path.c_str());
        }
    }

    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "конфиг не прочитан (%s), беру значения по умолчанию\n",
                     std::strerror(errno));
        return Config::defaults();
    }
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    Toml t;
    std::string err;
    if (!parse_toml(text, t, err)) {
        std::fprintf(stderr, "конфиг не прочитан (%s), беру значения по умолчанию\n", err.c_str());
        return Config::defaults();
    }
    return merge_config(t, verbose);
}

// -------------------------------------------------------------------- коды ---

static const std::unordered_map<std::string, int>& key_map() {
    static const std::unordered_map<std::string, int> m = [] {
        std::unordered_map<std::string, int> r;
#define K(x) r.emplace(#x, x)
        K(KEY_ESC);
        K(KEY_1); K(KEY_2); K(KEY_3); K(KEY_4); K(KEY_5);
        K(KEY_6); K(KEY_7); K(KEY_8); K(KEY_9); K(KEY_0);
        K(KEY_MINUS); K(KEY_EQUAL); K(KEY_BACKSPACE); K(KEY_TAB);
        K(KEY_Q); K(KEY_W); K(KEY_E); K(KEY_R); K(KEY_T); K(KEY_Y);
        K(KEY_U); K(KEY_I); K(KEY_O); K(KEY_P);
        K(KEY_LEFTBRACE); K(KEY_RIGHTBRACE); K(KEY_ENTER);
        K(KEY_LEFTCTRL); K(KEY_A); K(KEY_S); K(KEY_D); K(KEY_F); K(KEY_G);
        K(KEY_H); K(KEY_J); K(KEY_K); K(KEY_L);
        K(KEY_SEMICOLON); K(KEY_APOSTROPHE); K(KEY_GRAVE);
        K(KEY_LEFTSHIFT); K(KEY_BACKSLASH);
        K(KEY_Z); K(KEY_X); K(KEY_C); K(KEY_V); K(KEY_B); K(KEY_N); K(KEY_M);
        K(KEY_COMMA); K(KEY_DOT); K(KEY_SLASH); K(KEY_RIGHTSHIFT);
        K(KEY_KPASTERISK); K(KEY_LEFTALT); K(KEY_SPACE); K(KEY_CAPSLOCK);
        K(KEY_F1); K(KEY_F2); K(KEY_F3); K(KEY_F4); K(KEY_F5); K(KEY_F6);
        K(KEY_F7); K(KEY_F8); K(KEY_F9); K(KEY_F10); K(KEY_F11); K(KEY_F12);
        K(KEY_NUMLOCK); K(KEY_SCROLLLOCK);
        K(KEY_KP7); K(KEY_KP8); K(KEY_KP9); K(KEY_KPMINUS);
        K(KEY_KP4); K(KEY_KP5); K(KEY_KP6); K(KEY_KPPLUS);
        K(KEY_KP1); K(KEY_KP2); K(KEY_KP3);
        K(KEY_KP0); K(KEY_KPDOT); K(KEY_KPENTER); K(KEY_KPSLASH);
        K(KEY_102ND); K(KEY_F13); K(KEY_F14); K(KEY_F15); K(KEY_F16);
        K(KEY_F17); K(KEY_F18); K(KEY_F19); K(KEY_F20); K(KEY_F21);
        K(KEY_F22); K(KEY_F23); K(KEY_F24);
        K(KEY_SYSRQ); K(KEY_PAUSE); K(KEY_HOME); K(KEY_UP);
        K(KEY_PAGEUP); K(KEY_LEFT); K(KEY_RIGHT); K(KEY_END);
        K(KEY_DOWN); K(KEY_PAGEDOWN); K(KEY_INSERT); K(KEY_DELETE);
        K(KEY_MUTE); K(KEY_VOLUMEDOWN); K(KEY_VOLUMEUP);
        K(KEY_POWER); K(KEY_KPEQUAL);
        K(KEY_STOPCD); K(KEY_PREVIOUSSONG);
        K(KEY_PLAYPAUSE); K(KEY_NEXTSONG); K(KEY_MENU); K(KEY_COMPOSE);
        K(KEY_BRIGHTNESSDOWN); K(KEY_BRIGHTNESSUP);
        K(KEY_RIGHTCTRL); K(KEY_RIGHTALT); K(KEY_RIGHTMETA);
        K(KEY_LEFTMETA);
        K(BTN_LEFT); K(BTN_RIGHT); K(BTN_MIDDLE);
        K(BTN_SIDE); K(BTN_EXTRA); K(BTN_FORWARD); K(BTN_BACK);
#undef K
        return r;
    }();
    return m;
}

static const std::unordered_map<std::string, int>& pad_map() {
    static const std::unordered_map<std::string, int> m = {
        {"BTN_A", BTN_SOUTH}, {"BTN_B", BTN_EAST},
        {"BTN_X", BTN_NORTH}, {"BTN_Y", BTN_WEST},
        {"LB", BTN_TL},       {"RB", BTN_TR},
        // L2/R2 on DualShock 3 are digital buttons; on DS4/DualSense they
        // arrive as ABS_Z/ABS_RZ and are handled in handle_abs().
        {"LT", BTN_TL2},      {"RT", BTN_TR2},
        {"SELECT", BTN_SELECT}, {"START", BTN_START},
        {"MODE", BTN_MODE},
        {"THUMBL", BTN_THUMBL}, {"THUMBR", BTN_THUMBR},
        // Some pads report the d-pad as buttons instead of ABS_HAT0X/Y.
        {"DPAD_UP", BTN_DPAD_UP}, {"DPAD_DOWN", BTN_DPAD_DOWN},
        {"DPAD_LEFT", BTN_DPAD_LEFT}, {"DPAD_RIGHT", BTN_DPAD_RIGHT},
    };
    return m;
}

static const std::unordered_map<int, std::string>& pad_code_names() {
    static const std::unordered_map<int, std::string> m = [] {
        std::unordered_map<int, std::string> r;
        for (const auto& [name, code] : pad_map()) r.emplace(code, name);
        return r;
    }();
    return m;
}

// --------------------------------------------------------------- математика --

struct Stick {
    double strength = 0.0;
    double dx = 0.0;
    double dy = 0.0;
};

static Stick stick_vector(double x, double y, double deadzone, double curve) {
    const double mag = std::hypot(x, y);
    if (mag <= deadzone || mag == 0.0) return {};
    const double clamped = std::min(mag, 1.0);
    Stick s;
    s.strength = std::pow((clamped - deadzone) / (1.0 - deadzone), curve);
    s.dx = x / mag;  // unit direction even for square-gate sticks (mag > 1)
    s.dy = y / mag;
    return s;
}

static double norm_axis(const input_absinfo* ai, int value) {
    if (!ai) return 0.0;
    if (value < 0 && ai->minimum < 0)
        return static_cast<double>(value) / static_cast<double>(-ai->minimum);
    if (ai->maximum > 0)
        return static_cast<double>(value) / static_cast<double>(ai->maximum);
    return 0.0;
}

// --------------------------------------------------------------- демон -------

class GamepadMouse {
public:
    GamepadMouse(Config cfg, bool verbose)
        : cfg_(std::move(cfg)), verbose_(verbose) {
        for (const auto& n : cfg_.toggle.combo) combo_.insert(n);
        combo_enabled_ = cfg_.toggle.enabled && !combo_.empty();
        make_uinput();
        if (touchpad_mode()) make_touchpad();
    }

    ~GamepadMouse() {
        if (touchpad_mode()) tp_end();
        if (tpad_ui_) libevdev_uinput_destroy(tpad_ui_);
        if (tpad_dev_) libevdev_free(tpad_dev_);
        if (ui_) libevdev_uinput_destroy(ui_);
        if (uidev_) libevdev_free(uidev_);
        if (pad_) libevdev_free(pad_);
        if (pad_fd_ >= 0) ::close(pad_fd_);
    }

    GamepadMouse(const GamepadMouse&) = delete;
    GamepadMouse& operator=(const GamepadMouse&) = delete;

    using clock = std::chrono::steady_clock;

    const std::string& device_hint() const { return cfg_.device_name; }

    const std::string& uinput_devnode() {
        devnode_ = libevdev_uinput_get_devnode(ui_) ? libevdev_uinput_get_devnode(ui_) : "";
        return devnode_;
    }

    bool uses_touchpad() const { return touchpad_mode(); }

    const std::string& touchpad_devnode() {
        tpad_node_ = (tpad_ui_ && libevdev_uinput_get_devnode(tpad_ui_))
                         ? libevdev_uinput_get_devnode(tpad_ui_)
                         : "";
        return tpad_node_;
    }

    bool open_pad(const std::string& path) {
        pad_fd_ = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (pad_fd_ < 0) return false;
        if (libevdev_new_from_fd(pad_fd_, &pad_) < 0) {
            ::close(pad_fd_);
            pad_fd_ = -1;
            pad_ = nullptr;
            return false;
        }
        return true;
    }

    void close_pad() {
        if (pad_) {
            libevdev_free(pad_);
            pad_ = nullptr;
        }
        if (pad_fd_ >= 0) {
            ::close(pad_fd_);
            pad_fd_ = -1;
        }
    }

    const char* pad_name() const { return pad_ ? libevdev_get_name(pad_) : ""; }

    void grab() {
        if (!cfg_.grab || !pad_ || grabbed_) return;
        int rc = libevdev_grab(pad_, LIBEVDEV_GRAB);
        if (rc < 0)
            std::fprintf(stderr, "не смог захватить устройство: %s\n", std::strerror(-rc));
        else
            grabbed_ = true;
    }

    void ungrab() {
        if (pad_ && grabbed_) {
            libevdev_grab(pad_, LIBEVDEV_UNGRAB);
            grabbed_ = false;
        }
    }

    // ---- главный цикл на открытом устройстве; false = устройство отвалилось ----
    bool loop() {
        auto now = clock::now();
        auto last_tick = now;
        auto next_tick = now + us(TICK);

        const bool trace = std::getenv("GM_TRACE") != nullptr;
        uint64_t n_poll = 0, n_tick = 0;
        auto t_report = now;

        while (!g_stop) {
            now = clock::now();

            // Таймаут нужен только пока реально движется стик (250 Гц), пока
            // идёт отсчёт удержания комбо или пока держим «палец» после скролла.
            // В простое блокируемся в ppoll без таймаута — просыпаемся по
            // событию с пада, как от прерывания.
            timespec ts{};
            const timespec* tsp = nullptr;
            if (active_ || combo_counting() || tp_release_pending()) {
                clock::duration wait = active_ ? (next_tick - now) : us(IDLE_TICK);
                if (tp_release_pending()) {
                    auto rel = tp_release_deadline() - now;
                    if (rel < wait) wait = rel;
                }
                if (wait < clock::duration::zero()) wait = clock::duration::zero();
                auto secs = std::chrono::duration_cast<std::chrono::seconds>(wait);
                ts.tv_sec = static_cast<time_t>(secs.count());
                ts.tv_nsec = static_cast<long>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(wait - secs).count());
                tsp = &ts;
            }
            const bool blocking = (tsp == nullptr);

            pollfd pfd{pad_fd_, POLLIN, 0};
            int pr = ::ppoll(&pfd, 1, tsp, nullptr);
            ++n_poll;
            if (pr < 0 && errno != EINTR) return false;
            if (pr > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                return false;  // устройство отвалилось
            }
            bool got_event = false;
            if (pr > 0 && (pfd.revents & POLLIN)) {
                if (!read_events()) return false;
                got_event = true;
            }

            now = clock::now();
            if (got_event) next_tick = std::min(next_tick, now);
            // Coming out of an untimed ppoll: the stick has just moved, so do
            // not integrate the whole idle period (clamped to 50 ms before).
            if (blocking && got_event) last_tick = now;
            if (now >= next_tick) {
                double dt = std::chrono::duration<double>(now - last_tick).count();
                dt = std::min(dt, 0.05);
                last_tick = now;
                tick(dt);
                ++n_tick;
                next_tick = now + us(active_ ? TICK : IDLE_TICK);
            }

            if (trace &&
                std::chrono::duration<double>(now - t_report).count() >= 1.0) {
                std::printf("trace: polls/s=%llu ticks/s=%llu active=%d\n",
                            static_cast<unsigned long long>(n_poll),
                            static_cast<unsigned long long>(n_tick),
                            active_ ? 1 : 0);
                std::fflush(stdout);
                n_poll = n_tick = 0;
                t_report = now;
            }
        }
        return false;
    }

    // ---- самопроверка: прогнать тики без реального геймпада ----
    void selftest_sticks(double rx, double ry, double sx, double sy) {
        right_[0] = rx;
        right_[1] = ry;
        left_[0] = sx;
        left_[1] = sy;
    }

    void tick_for_selftest(double dt) { tick(dt); }

    // ---- нажатия ----
    void set_button(const std::string& name, bool pressed) {
        bool was = held_.count(name) > 0;
        if (pressed == was) return;

        if (pressed) {
            held_.insert(name);
            if (paused_) return;  // на паузе пад полностью свободен
            if (combo_active_ && combo_.count(name)) return;
            auto codes = action_codes(name);
            if (!codes.empty()) {
                for (int c : codes) press(c, 1);
                sent_.insert(name);
            }
        } else {
            held_.erase(name);
            if (sent_.count(name)) {
                sent_.erase(name);
                auto codes = action_codes(name);
                for (auto it = codes.rbegin(); it != codes.rend(); ++it) press(*it, 0);
            }
        }
        if (verbose_) {
            auto it = cfg_.buttons.find(name);
            const char* act = it == cfg_.buttons.end() ? "none" : it->second.c_str();
            std::printf("%s %s -> %s\n", pressed ? "↓" : "↑", name.c_str(), act);
            std::fflush(stdout);
        }
    }

    bool paused() const { return paused_; }

    void release_all_public() { release_all(); }
    void reset_input_state_public() { reset_input_state(); }
    void end_scroll_public() {
        if (touchpad_mode()) tp_end();
    }

private:
    static clock::duration us(double seconds) {
        return std::chrono::duration_cast<clock::duration>(
            std::chrono::duration<double>(seconds));
    }

    // ---- uinput ----
    void make_uinput() {
        uidev_ = libevdev_new();
        libevdev_set_name(uidev_, VIRTUAL_NAME);
        libevdev_set_id_vendor(uidev_, 0x1d6b);
        libevdev_set_id_product(uidev_, 0x0110);
        libevdev_set_id_version(uidev_, 1);
        libevdev_set_id_bustype(uidev_, BUS_USB);
        libevdev_enable_property(uidev_, INPUT_PROP_POINTER);

        libevdev_enable_event_type(uidev_, EV_REL);
        libevdev_enable_event_code(uidev_, EV_REL, REL_X, nullptr);
        libevdev_enable_event_code(uidev_, EV_REL, REL_Y, nullptr);
        if (!touchpad_mode()) {  // в режиме тачпада колесо не нужно
            libevdev_enable_event_code(uidev_, EV_REL, REL_WHEEL, nullptr);
            libevdev_enable_event_code(uidev_, EV_REL, REL_HWHEEL, nullptr);
            if (cfg_.scroll.hi_res) {
                libevdev_enable_event_code(uidev_, EV_REL, REL_WHEEL_HI_RES, nullptr);
                libevdev_enable_event_code(uidev_, EV_REL, REL_HWHEEL_HI_RES, nullptr);
            }
        }

        libevdev_enable_event_type(uidev_, EV_KEY);
        for (const auto& [name, code] : key_map())
            libevdev_enable_event_code(uidev_, EV_KEY, code, nullptr);

        int rc = libevdev_uinput_create_from_device(uidev_, LIBEVDEV_UINPUT_OPEN_MANAGED, &ui_);
        if (rc < 0) {
            std::fprintf(stderr, "не смог создать виртуальное устройство: %s\n",
                         std::strerror(-rc));
            std::exit(1);
        }
    }

    bool touchpad_mode() const {
        return cfg_.scroll.mode == "touchpad" || cfg_.scroll.mode == "tpad";
    }

    // ---- виртуальный тачпад: плавный скролл двумя «пальцами» ----
    void make_touchpad() {
        tpad_dev_ = libevdev_new();
        libevdev_set_name(tpad_dev_, TOUCHPAD_NAME);
        libevdev_set_id_vendor(tpad_dev_, 0x1d6b);
        libevdev_set_id_product(tpad_dev_, 0x0111);
        libevdev_set_id_version(tpad_dev_, 1);
        libevdev_set_id_bustype(tpad_dev_, BUS_USB);
        libevdev_enable_property(tpad_dev_, INPUT_PROP_POINTER);

        libevdev_enable_event_type(tpad_dev_, EV_ABS);
        auto abs_code = [&](int code, int minimum, int maximum, int res) {
            input_absinfo ai{};
            ai.minimum = minimum;
            ai.maximum = maximum;
            ai.resolution = res;
            libevdev_enable_event_code(tpad_dev_, EV_ABS, code, &ai);
        };
        abs_code(ABS_X, 0, TP_MAX, TP_RES);
        abs_code(ABS_Y, 0, TP_MAX, TP_RES);
        abs_code(ABS_PRESSURE, 0, 255, 0);
        abs_code(ABS_MT_SLOT, 0, 1, 0);
        abs_code(ABS_MT_TOUCH_MAJOR, 0, 255, 0);
        abs_code(ABS_MT_PRESSURE, 0, 255, 0);
        abs_code(ABS_MT_POSITION_X, 0, TP_MAX, TP_RES);
        abs_code(ABS_MT_POSITION_Y, 0, TP_MAX, TP_RES);
        abs_code(ABS_MT_TRACKING_ID, 0, 65535, 0);

        libevdev_enable_event_type(tpad_dev_, EV_KEY);
        libevdev_enable_event_code(tpad_dev_, EV_KEY, BTN_LEFT, nullptr);
        libevdev_enable_event_code(tpad_dev_, EV_KEY, BTN_RIGHT, nullptr);
        libevdev_enable_event_code(tpad_dev_, EV_KEY, BTN_MIDDLE, nullptr);
        libevdev_enable_event_code(tpad_dev_, EV_KEY, BTN_TOUCH, nullptr);
        libevdev_enable_event_code(tpad_dev_, EV_KEY, BTN_TOOL_FINGER, nullptr);
        libevdev_enable_event_code(tpad_dev_, EV_KEY, BTN_TOOL_DOUBLETAP, nullptr);

        int rc = libevdev_uinput_create_from_device(tpad_dev_, LIBEVDEV_UINPUT_OPEN_MANAGED, &tpad_ui_);
        if (rc < 0) {
            std::fprintf(stderr, "не смог создать виртуальный тачпад: %s\n",
                         std::strerror(-rc));
            std::exit(1);
        }
    }

    void tp_write(int type, int code, int value) {
        if (tpad_ui_) libevdev_uinput_write_event(tpad_ui_, type, code, value);
    }

    void tp_syn() { tp_write(EV_SYN, SYN_REPORT, 0); }

    void tp_pos(int slot, int x, int y) {
        tp_write(EV_ABS, ABS_MT_SLOT, slot);
        tp_write(EV_ABS, ABS_MT_POSITION_X, x);
        tp_write(EV_ABS, ABS_MT_POSITION_Y, y);
    }

    void tp_begin(double x, double y) {
        tp_x_ = x;
        tp_y_ = y;
        tp_hold_ = false;
        tp_release_ = false;
        int cx = static_cast<int>(std::lround(x));
        int cy = static_cast<int>(std::lround(y));
        tp_write(EV_ABS, ABS_MT_SLOT, 0);
        tp_write(EV_ABS, ABS_MT_TRACKING_ID, ++tp_id_ % 60000);
        tp_write(EV_ABS, ABS_MT_PRESSURE, 120);
        tp_write(EV_ABS, ABS_MT_SLOT, 1);
        tp_write(EV_ABS, ABS_MT_TRACKING_ID, ++tp_id_ % 60000);
        tp_write(EV_ABS, ABS_MT_PRESSURE, 120);
        tp_pos(0, cx, cy);
        tp_pos(1, cx, cy + TP_FINGER_GAP);
        tp_write(EV_ABS, ABS_X, cx);
        tp_write(EV_ABS, ABS_Y, cy);
        tp_write(EV_ABS, ABS_PRESSURE, 120);
        tp_write(EV_ABS, ABS_MT_SLOT, 0);
        tp_write(EV_KEY, BTN_TOUCH, 1);
        // Два пальца: только DOUBLETAP. FINGER=1 одновременно с DOUBLETAP —
        // недопустимое состояние, libinput ругается «Invalid fake finger state».
        tp_write(EV_KEY, BTN_TOOL_FINGER, 0);
        tp_write(EV_KEY, BTN_TOOL_DOUBLETAP, 1);
        tp_syn();
        tp_active_ = true;
    }

    // Прокрутка продолжилась, пока первый палец ещё на тачпаде.
    void tp_resume() {
        int cx = static_cast<int>(std::lround(tp_x_));
        int cy = static_cast<int>(std::lround(tp_y_));
        tp_write(EV_ABS, ABS_MT_SLOT, 1);
        tp_write(EV_ABS, ABS_MT_TRACKING_ID, ++tp_id_ % 60000);
        tp_write(EV_ABS, ABS_MT_PRESSURE, 120);
        tp_pos(1, cx, cy + TP_FINGER_GAP);
        tp_write(EV_ABS, ABS_MT_SLOT, 0);
        tp_write(EV_KEY, BTN_TOOL_FINGER, 0);
        tp_write(EV_KEY, BTN_TOOL_DOUBLETAP, 1);
        tp_syn();
        tp_hold_ = false;
        tp_release_ = false;
        tp_active_ = true;
    }

    // Прокрутка продолжилась, пока оба пальца ещё на тачпаде (ждём перед
    // снятием второго) — просто снимаем паузу, ничего не пересоздавая.
    void tp_continue() {
        tp_hold_ = false;
        tp_release_ = false;
        tp_active_ = true;
    }

    // Конец прокрутки. Сразу пальцы не поднимаем: сначала держим оба
    // неподвижно TP_FLING_GUARD секунд, чтобы axis_stop пришёл позже
    // kFlingStartTimeoutMs (200 мс) в Chromium — иначе он запускает
    // «инерцию» (fling). Затем снимаем второй палец, а первый держим ещё
    // TP_TAP_GUARD секунд: иначе короткий жест libinput примет за
    // двухпальцевый тап, а в Wayfire включён tap_to_click (ложный ПКМ).
    void tp_stop() {
        if (!tp_active_) return;
        tp_active_ = false;
        tp_hold_ = true;
        tp_hold_at_ = clock::now() + us(TP_FLING_GUARD);
    }

    void tp_release_step() {
        auto now = clock::now();
        if (tp_active_) {
            tp_hold_ = false;
            tp_release_ = false;
            return;
        }
        if (tp_hold_) {
            if (now < tp_hold_at_) return;
            tp_write(EV_ABS, ABS_MT_SLOT, 1);
            tp_write(EV_ABS, ABS_MT_TRACKING_ID, -1);
            tp_write(EV_ABS, ABS_MT_SLOT, 0);
            tp_write(EV_KEY, BTN_TOOL_DOUBLETAP, 0);
            tp_write(EV_KEY, BTN_TOOL_FINGER, 1);
            tp_syn();
            tp_hold_ = false;
            tp_release_ = true;
            tp_release_at_ = now + us(TP_TAP_GUARD);
            return;
        }
        if (!tp_release_ || now < tp_release_at_) return;
        tp_write(EV_ABS, ABS_MT_SLOT, 0);
        tp_write(EV_ABS, ABS_MT_TRACKING_ID, -1);
        tp_write(EV_KEY, BTN_TOUCH, 0);
        tp_write(EV_KEY, BTN_TOOL_FINGER, 0);
        tp_syn();
        tp_release_ = false;
    }

    bool tp_release_pending() const { return tp_hold_ || tp_release_; }
    clock::time_point tp_release_deadline() const {
        return tp_hold_ ? tp_hold_at_ : tp_release_at_;
    }

    void tp_end() {
        if (!tp_active_ && !tp_hold_ && !tp_release_) return;
        tp_write(EV_ABS, ABS_MT_SLOT, 0);
        tp_write(EV_ABS, ABS_MT_TRACKING_ID, -1);
        tp_write(EV_ABS, ABS_MT_SLOT, 1);
        tp_write(EV_ABS, ABS_MT_TRACKING_ID, -1);
        tp_write(EV_ABS, ABS_MT_SLOT, 0);
        tp_write(EV_KEY, BTN_TOUCH, 0);
        tp_write(EV_KEY, BTN_TOOL_FINGER, 0);
        tp_write(EV_KEY, BTN_TOOL_DOUBLETAP, 0);
        tp_syn();
        tp_active_ = false;
        tp_hold_ = false;
        tp_release_ = false;
    }

    void tp_move(double dx, double dy) {
        tp_x_ += dx;
        tp_y_ += dy;
        int cx = static_cast<int>(std::lround(tp_x_));
        int cy = static_cast<int>(std::lround(tp_y_));
        tp_pos(0, cx, cy);
        tp_pos(1, cx, cy + TP_FINGER_GAP);
        tp_write(EV_ABS, ABS_X, cx);
        tp_write(EV_ABS, ABS_Y, cy);
        tp_write(EV_ABS, ABS_MT_SLOT, 0);
        tp_syn();
    }

    void tp_drive(double rate_v, double rate_h, double dt) {
        if (!tp_active_) {
            if (tp_hold_) tp_continue();
            else if (tp_release_) tp_resume();
            else tp_begin(TP_START, TP_START);
        }
        // Пальцы двигаются в сторону движения стика; знак калибруется.
        double dx = TP_SIGN_X * rate_h * TP_UNITS_PER_NOTCH * dt;
        double dy = TP_SIGN_Y * rate_v * TP_UNITS_PER_NOTCH * dt;
        tp_move(dx, dy);
        if (tp_x_ < TP_MARGIN || tp_x_ > TP_MAX - TP_MARGIN ||
            tp_y_ < TP_MARGIN || tp_y_ > TP_MAX - TP_MARGIN) {
            tp_end();
            tp_begin(TP_START, TP_START);
        }
    }

    void press(int code, int value) {
        // Several buttons or overlapping combos may target the same key: emit
        // only real transitions, so releasing one source keeps the key held.
        auto it = key_refs_.find(code);
        if (value) {
            if (it != key_refs_.end()) {
                ++it->second;
                return;
            }
            key_refs_[code] = 1;
        } else {
            if (it == key_refs_.end()) return;
            if (--it->second > 0) return;
            key_refs_.erase(it);
        }
        libevdev_uinput_write_event(ui_, EV_KEY, code, value);
        libevdev_uinput_write_event(ui_, EV_SYN, SYN_REPORT, 0);
    }

    void rel(int code, int value) {
        libevdev_uinput_write_event(ui_, EV_REL, code, value);
        wrote_ = true;
    }

    std::vector<int> action_codes(const std::string& name) {
        std::vector<int> out;
        auto it = cfg_.buttons.find(name);
        if (it == cfg_.buttons.end()) return out;
        std::string action = trim(it->second);
        if (action.empty() || lower(action) == "none") return out;

        std::string part;
        for (char ch : action + "+") {
            if (ch != '+') {
                part.push_back(ch);
                continue;
            }
            std::string p = trim(part);
            part.clear();
            if (p.empty()) continue;
            auto k = key_map().find(p);
            if (k == key_map().end()) {
                std::fprintf(stderr, "неизвестное действие '%s' для кнопки %s\n",
                             p.c_str(), name.c_str());
                continue;
            }
            out.push_back(k->second);
        }
        return out;
    }

    void release_sent(const std::string& name) {
        if (!sent_.count(name)) return;
        sent_.erase(name);
        auto codes = action_codes(name);
        for (auto it = codes.rbegin(); it != codes.rend(); ++it) press(*it, 0);
    }

    void release_all() {
        std::vector<std::string> copy(sent_.begin(), sent_.end());
        for (const auto& n : copy) release_sent(n);
        held_.clear();
    }

    // Drop analog state after a disconnect: evdev does not replay the current
    // stick position, so stale values would keep moving the pointer/scroll.
    void reset_input_state() {
        left_[0] = left_[1] = 0.0;
        right_[0] = right_[1] = 0.0;
        trigger_[0] = trigger_[1] = 0.0;
        trigger_btn_[0] = trigger_btn_[1] = false;
        active_ = false;
        scroll_moving_ = false;
        acc_x_ = acc_y_ = 0.0;
        acc_hi_v_ = acc_hi_h_ = 0.0;
        acc_v_ = acc_h_ = 0.0;
        combo_timer_ = 0.0;
        combo_active_ = false;
    }

    void say(const char* msg) {
        std::printf("%s\n", msg);
        std::fflush(stdout);
        if (!cfg_.toggle.notify) return;

        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        const char* args[] = {"notify-send", "-a", VIRTUAL_NAME, "-t", "1500",
                              "Геймпад", msg};
        std::vector<char*> argv;
        for (const char* a : args) argv.push_back(const_cast<char*>(a));
        argv.push_back(nullptr);
        pid_t pid = 0;
        posix_spawnp(&pid, "notify-send", &fa, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&fa);
    }

    void toggle_pause() {
        paused_ = !paused_;
        acc_x_ = acc_y_ = 0.0;
        acc_hi_v_ = acc_hi_h_ = 0.0;
        acc_v_ = acc_h_ = 0.0;
        if (paused_) {
            if (touchpad_mode()) tp_end();
            release_all();
            ungrab();
            say("⏸ пауза — геймпад свободен для игр");
        } else {
            grab();
            say("▶ снова управляю мышью");
        }
    }

    void combo_tick(double dt) {
        if (!combo_enabled_) return;
        bool all = true;
        for (const auto& n : combo_)
            if (!held_.count(n)) { all = false; break; }

        if (all) {
            combo_timer_ += dt;
            if (!combo_active_ && combo_timer_ >= cfg_.toggle.hold) {
                combo_active_ = true;
                std::vector<std::string> to_release;
                for (const auto& n : sent_)
                    if (combo_.count(n)) to_release.push_back(n);
                for (const auto& n : to_release) release_sent(n);
                toggle_pause();
            }
        } else {
            combo_timer_ = 0.0;
            combo_active_ = false;
        }
    }

    // Идёт ли сейчас отсчёт удержания комбо (нужно просыпаться по таймеру).
    bool combo_counting() const {
        if (!combo_enabled_ || combo_active_ || combo_.empty()) return false;
        for (const auto& n : combo_)
            if (!held_.count(n)) return false;
        return true;
    }

    // ---- чтение ----
    bool read_events() {
        input_event ev{};
        unsigned flags = LIBEVDEV_READ_FLAG_NORMAL;
        for (;;) {
            int rc = libevdev_next_event(pad_, flags, &ev);
            if (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
                handle(ev);
                flags = LIBEVDEV_READ_FLAG_NORMAL;
            } else if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                // состояние изменилось, пока нас не было: события несут актуальные значения
                handle(ev);
                flags = LIBEVDEV_READ_FLAG_SYNC;
            } else {
                if (rc == -ENODEV) return false;  // пад отключился
                break;                            // -EAGAIN
            }
        }
        return true;
    }

    void handle(const input_event& ev) {
        if (ev.type == EV_ABS) {
            handle_abs(ev.code, ev.value);
        } else if (ev.type == EV_KEY) {
            const bool down = ev.value != 0;

            // L2/R2: DS3 sends BTN_TL2/BTN_TR2, DS4/DualSense send ABS_Z/ABS_RZ.
            // Combine both paths so a pad exposing both does not double-trigger.
            if (ev.code == BTN_TL2) {
                trigger_btn_[0] = down;
                trigger_edge("LT", 0);
                return;
            }
            if (ev.code == BTN_TR2) {
                trigger_btn_[1] = down;
                trigger_edge("RT", 1);
                return;
            }

            // Prefer the matching ABS_HAT0X/Y axis when the pad has it;
            // BTN_DPAD_* is only a fallback for the other axis.
            if (ev.code == BTN_DPAD_UP || ev.code == BTN_DPAD_DOWN) {
                if (libevdev_has_event_code(pad_, EV_ABS, ABS_HAT0Y)) return;
            } else if (ev.code == BTN_DPAD_LEFT || ev.code == BTN_DPAD_RIGHT) {
                if (libevdev_has_event_code(pad_, EV_ABS, ABS_HAT0X)) return;
            }

            const auto& names = pad_code_names();
            auto it = names.find(ev.code);
            if (it != names.end()) set_button(it->second, down);
        }
    }

    // Дрожание стика внутри мёртвой зоны будит ppoll, но не поднимает частоту:
    // таймер 250 Гц включается по active_ в tick().
    // stick: 0 = левый (скролл), 1 = правый (курсор); axis: 0 = X, 1 = Y.
    void set_stick_axis(int stick, int axis, int code, int value) {
        double* s = stick == 0 ? left_ : right_;
        s[axis] = norm_axis(libevdev_get_abs_info(pad_, code), value);
    }

    void handle_abs(int code, int value) {
        switch (code) {
            case ABS_X:
                set_stick_axis(0, 0, ABS_X, value);
                break;
            case ABS_Y:
                set_stick_axis(0, 1, ABS_Y, value);
                break;
            case ABS_RX:
                set_stick_axis(1, 0, ABS_RX, value);
                break;
            case ABS_RY:
                set_stick_axis(1, 1, ABS_RY, value);
                break;
            case ABS_Z:
                trigger_[0] = norm_axis(libevdev_get_abs_info(pad_, ABS_Z), value);
                trigger_edge("LT", 0);
                break;
            case ABS_RZ:
                trigger_[1] = norm_axis(libevdev_get_abs_info(pad_, ABS_RZ), value);
                trigger_edge("RT", 1);
                break;
            case ABS_HAT0X:
                dpad_axis("HAT0X", value);
                break;
            case ABS_HAT0Y:
                dpad_axis("HAT0Y", value);
                break;
            default:
                break;
        }
    }

    void trigger_edge(const std::string& name, int idx) {
        const double val = std::max(trigger_[idx], trigger_btn_[idx] ? 1.0 : 0.0);
        if (held_.count(name)) {
            if (val < cfg_.trigger_threshold * 0.6) set_button(name, false);
        } else if (val > cfg_.trigger_threshold) {
            set_button(name, true);
        }
    }

    void dpad_axis(const std::string& axis, int value) {
        struct Dir { const char* axis; int sign; const char* name; };
        static const Dir dirs[] = {
            {"HAT0X", -1, "DPAD_LEFT"}, {"HAT0X", 1, "DPAD_RIGHT"},
            {"HAT0Y", -1, "DPAD_UP"},   {"HAT0Y", 1, "DPAD_DOWN"},
        };
        for (const auto& d : dirs) {
            if (axis != d.axis) continue;
            set_button(d.name, value == d.sign);
        }
    }

    // ---- интеграция ----
    void tick(double dt) {
        wrote_ = false;
        if (touchpad_mode() && !scroll_moving_) tp_release_step();
        combo_tick(dt);

        if (paused_) {
            active_ = false;
            scroll_moving_ = false;
            if (touchpad_mode()) tp_end();
            return;
        }

        // курсор
        Stick s = stick_vector(right_[0], right_[1], cfg_.pointer.deadzone, cfg_.pointer.curve);
        active_ = s.strength > 0.0;
        if (s.strength > 0.0) {
            double speed = cfg_.pointer.max_speed * s.strength;
            double vx = s.dx * speed * (cfg_.pointer.invert_x ? -1.0 : 1.0);
            double vy = s.dy * speed * (cfg_.pointer.invert_y ? -1.0 : 1.0);
            acc_x_ += vx * dt;
            acc_y_ += vy * dt;
            int mx = static_cast<int>(acc_x_);
            int my = static_cast<int>(acc_y_);
            if (mx || my) {
                acc_x_ -= mx;
                acc_y_ -= my;
                if (mx) rel(REL_X, mx);
                if (my) rel(REL_Y, my);
            }
        } else {
            acc_x_ = acc_y_ = 0.0;
        }

        // прокрутка
        s = stick_vector(left_[0], left_[1], cfg_.scroll.deadzone, cfg_.scroll.curve);
        scroll_moving_ = s.strength > 0.0;
        if (s.strength > 0.0) {
            active_ = true;
            double speed = cfg_.scroll.max_speed * s.strength;
            double rate_v = cfg_.scroll.invert_y ? -s.dy * speed : s.dy * speed;
            double rate_h = s.dx * speed;
            if (cfg_.scroll.invert_x) rate_h = -rate_h;

            if (touchpad_mode()) {
                tp_drive(rate_v, rate_h, dt);
            } else {
                if (cfg_.scroll.hi_res) {
                    acc_hi_v_ += rate_v * 120.0 * dt;
                    acc_hi_h_ += rate_h * 120.0 * dt;
                    int hv = static_cast<int>(acc_hi_v_);
                    int hh = static_cast<int>(acc_hi_h_);
                    if (hv) {
                        acc_hi_v_ -= hv;
                        rel(REL_WHEEL_HI_RES, hv);
                    }
                    if (hh) {
                        acc_hi_h_ -= hh;
                        rel(REL_HWHEEL_HI_RES, hh);
                    }
                }

                acc_v_ += rate_v * dt;
                acc_h_ += rate_h * dt;
                int nv = static_cast<int>(acc_v_);
                int nh = static_cast<int>(acc_h_);
                if (nv) {
                    acc_v_ -= nv;
                    rel(REL_WHEEL, nv);
                }
                if (nh) {
                    acc_h_ -= nh;
                    rel(REL_HWHEEL, nh);
                }
            }
        } else if (touchpad_mode()) {
            tp_stop();
            acc_hi_v_ = acc_hi_h_ = 0.0;
            acc_v_ = acc_h_ = 0.0;
        } else {
            acc_hi_v_ = acc_hi_h_ = 0.0;
            acc_v_ = acc_h_ = 0.0;
        }

        if (wrote_) libevdev_uinput_write_event(ui_, EV_SYN, SYN_REPORT, 0);
    }

    Config cfg_;
    bool verbose_;
    bool paused_ = false;
    bool grabbed_ = false;
    bool active_ = false;
    bool wrote_ = false;
    bool combo_active_ = false;
    bool combo_enabled_ = false;
    double combo_timer_ = 0.0;

    std::set<std::string> combo_;
    std::set<std::string> held_;
    std::set<std::string> sent_;
    std::unordered_map<int, unsigned> key_refs_;

    double left_[2]{0.0, 0.0};
    double right_[2]{0.0, 0.0};
    double trigger_[2]{0.0, 0.0};
    bool trigger_btn_[2]{false, false};

    double acc_x_ = 0.0, acc_y_ = 0.0;
    double acc_hi_v_ = 0.0, acc_hi_h_ = 0.0;
    double acc_v_ = 0.0, acc_h_ = 0.0;

    libevdev* pad_ = nullptr;
    int pad_fd_ = -1;
    libevdev* uidev_ = nullptr;
    libevdev_uinput* ui_ = nullptr;
    std::string devnode_;

    libevdev* tpad_dev_ = nullptr;
    libevdev_uinput* tpad_ui_ = nullptr;
    std::string tpad_node_;
    bool tp_active_ = false;
    bool tp_hold_ = false;
    bool tp_release_ = false;
    bool scroll_moving_ = false;
    clock::time_point tp_hold_at_{};
    clock::time_point tp_release_at_{};
    double tp_x_ = TP_START, tp_y_ = TP_START;
    int tp_id_ = 0;
};

// --------------------------------------------------------------- поиск пада --

static std::vector<std::string> list_event_devices() {
    std::vector<std::string> out;
    DIR* d = ::opendir("/dev/input");
    if (!d) return out;
    while (dirent* e = ::readdir(d)) {
        std::string n = e->d_name;
        if (n.rfind("event", 0) == 0 && n.size() > 5) out.push_back("/dev/input/" + n);
    }
    ::closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

static std::string find_pad(const std::string& hint, bool verbose) {
    std::string best;
    int best_score = 999;
    std::string hint_lc = lower(hint);

    for (const auto& path : list_event_devices()) {
        int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        libevdev* dev = nullptr;
        if (libevdev_new_from_fd(fd, &dev) < 0) {
            ::close(fd);
            continue;
        }
        std::string name = libevdev_get_name(dev);
        bool ok = name != VIRTUAL_NAME &&
                  libevdev_has_event_type(dev, EV_ABS) &&
                  libevdev_has_event_code(dev, EV_ABS, ABS_RX) &&
                  libevdev_has_event_code(dev, EV_ABS, ABS_RY) &&
                  libevdev_has_event_code(dev, EV_KEY, BTN_TR);
        if (ok && !hint_lc.empty() && lower(name).find(hint_lc) == std::string::npos) ok = false;

        if (ok) {
            int score = lower(name).find("controller") != std::string::npos ? 0 : 1;
            if (score < best_score) {
                best_score = score;
                best = path;
                if (verbose) std::printf("геймпад: %s (%s)\n", name.c_str(), path.c_str());
            }
        }
        libevdev_free(dev);
        ::close(fd);
    }
    return best;
}

// ------------------------------------------------------------- самопроверка --

static int selftest(const Config& cfg) {
    GamepadMouse gm(cfg, false);
    const bool tpad = gm.uses_touchpad();

    auto open_dev = [](const std::string& node, libevdev** out) -> int {
        int fd = -1;
        int err = 0;
        for (int i = 0; i < 50; ++i) {  // udev может выставлять права с задержкой
            fd = ::open(node.c_str(), O_RDONLY | O_NONBLOCK);
            if (fd >= 0) break;
            err = errno;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (fd < 0 || libevdev_new_from_fd(fd, out) < 0) {
            std::fprintf(stderr, "не смог открыть %s: %s\n", node.c_str(),
                         std::strerror(err ? err : errno));
            if (fd >= 0) ::close(fd);
            return -1;
        }
        return fd;
    };

    std::string mouse_node = gm.uinput_devnode();
    std::printf("виртуальная мышь: %s (%s)\n", mouse_node.c_str(), VIRTUAL_NAME);
    libevdev* mouse = nullptr;
    int mouse_fd = open_dev(mouse_node, &mouse);
    if (mouse_fd < 0) return 1;
    if (libevdev_grab(mouse, LIBEVDEV_GRAB) < 0)
        std::fprintf(stderr, "предупреждение: не удалось захватить виртуальную мышь\n");

    libevdev* touch = nullptr;
    int touch_fd = -1;
    if (tpad) {
        std::string touch_node = gm.touchpad_devnode();
        std::printf("виртуальный тачпад: %s (%s)\n", touch_node.c_str(), TOUCHPAD_NAME);
        touch_fd = open_dev(touch_node, &touch);
        if (touch_fd < 0) {
            libevdev_grab(mouse, LIBEVDEV_UNGRAB);
            libevdev_free(mouse);
            ::close(mouse_fd);
            return 1;
        }
        if (libevdev_grab(touch, LIBEVDEV_GRAB) < 0)
            std::fprintf(stderr, "предупреждение: не удалось захватить виртуальный тачпад\n");
    }

    std::printf("%s\n", tpad ? "двигаю курсор и прокручиваю тачпадом..."
                             : "двигаю курсор вправо 0.3 с и прокручиваю вверх на 3 щелчка...");
    gm.selftest_sticks(0.8, 0.0, 0.0, -1.0);

    // Direction must stay unit-length for square-gate sticks (|x,y| > 1);
    // otherwise diagonal motion exceeds max_speed by up to sqrt(2).
    bool math_ok = true;
    for (double x : {-1.0, -0.5, 0.0, 0.5, 1.0}) {
        for (double y : {-1.0, -0.5, 0.0, 0.5, 1.0}) {
            if (std::hypot(x, y) <= 0.06) continue;
            const Stick s = stick_vector(x, y, 0.06, 2.2);
            if (!std::isfinite(s.strength) ||
                std::abs(std::hypot(s.dx, s.dy) - 1.0) > 1e-9 ||
                s.strength < 0.0 || s.strength > 1.0 + 1e-9)
                math_ok = false;
        }
    }

    std::map<int, int> seen;
    bool mt_moved = false;
    int last_mt_y = -1;
    auto start = GamepadMouse::clock::now();
    auto last = start;
    while (std::chrono::duration<double>(GamepadMouse::clock::now() - start).count() < 0.55) {
        auto now = GamepadMouse::clock::now();
        gm.tick_for_selftest(std::chrono::duration<double>(now - last).count());
        last = now;

        input_event ev{};
        while (libevdev_next_event(mouse, LIBEVDEV_READ_FLAG_NORMAL, &ev) ==
               LIBEVDEV_READ_STATUS_SUCCESS)
            seen[ev.code] += ev.value;

        if (touch) {
            while (libevdev_next_event(touch, LIBEVDEV_READ_FLAG_NORMAL, &ev) ==
                   LIBEVDEV_READ_STATUS_SUCCESS) {
                if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_Y) {
                    if (last_mt_y >= 0 && ev.value != last_mt_y) mt_moved = true;
                    last_mt_y = ev.value;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    // A reconnect must not continue with stale sticks: after reset a tick has
    // to stay silent even though the sticks were deflected just before.
    gm.reset_input_state_public();
    const int rel_x_before = seen[REL_X];
    const int rel_y_before = seen[REL_Y];
    for (int i = 0; i < 25; ++i) {
        gm.tick_for_selftest(0.004);
        input_event ev{};
        while (libevdev_next_event(mouse, LIBEVDEV_READ_FLAG_NORMAL, &ev) ==
               LIBEVDEV_READ_STATUS_SUCCESS)
            seen[ev.code] += ev.value;
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    const bool reset_ok = seen[REL_X] == rel_x_before && seen[REL_Y] == rel_y_before;

    if (touch) {
        libevdev_grab(touch, LIBEVDEV_UNGRAB);
        libevdev_free(touch);
        ::close(touch_fd);
    }
    libevdev_grab(mouse, LIBEVDEV_UNGRAB);
    libevdev_free(mouse);
    ::close(mouse_fd);

    auto name_of = [](int code) -> std::string {
        switch (code) {
            case REL_X: return "REL_X";
            case REL_Y: return "REL_Y";
            case REL_WHEEL: return "REL_WHEEL";
            case REL_HWHEEL: return "REL_HWHEEL";
            case REL_WHEEL_HI_RES: return "REL_WHEEL_HI_RES";
            case REL_HWHEEL_HI_RES: return "REL_HWHEEL_HI_RES";
            default: return std::to_string(code);
        }
    };

    std::printf("получено от виртуальной мыши:\n");
    for (const auto& [code, total] : seen)
        std::printf("  %s: %d\n", name_of(code).c_str(), total);
    if (tpad) std::printf("движение пальцев: %s\n", mt_moved ? "есть" : "нет");
    std::printf("математика стика: %s\n", math_ok ? "OK" : "ПРОБЛЕМА");
    std::printf("сброс состояния: %s\n", reset_ok ? "OK" : "ПРОБЛЕМА");

    bool ok = math_ok && reset_ok && std::abs(seen[REL_X]) > 0 &&
              (tpad ? mt_moved : std::abs(seen[REL_WHEEL]) > 0);
    std::printf("ИТОГ: %s\n", ok ? "OK" : "ПРОБЛЕМА");
    return ok ? 0 : 1;
}

// -------------------------------------------------------------------- main ---

static void wait_reconnect() {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(RECONNECT_DELAY));
    while (!g_stop && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

static int run_daemon(Config cfg, bool verbose, const std::string& device_override) {
    GamepadMouse gm(std::move(cfg), verbose);

    while (!g_stop) {
        std::string path = device_override;
        if (path.empty()) path = find_pad(gm.device_hint(), verbose);

        if (path.empty()) {
            if (verbose) std::printf("геймпад не найден, жду...\n");
            wait_reconnect();
            continue;
        }

        if (!gm.open_pad(path)) {
            wait_reconnect();
            continue;
        }

        if (verbose) std::printf("слушаю %s (%s)\n", gm.pad_name(), path.c_str());
        gm.grab();
        gm.loop();
        gm.end_scroll_public();
        gm.release_all_public();
        gm.reset_input_state_public();
        gm.ungrab();
        gm.close_pad();

        wait_reconnect();
    }
    return 0;
}

static int usage(const char* argv0) {
    std::printf(
        "gpadmapper-cpp — геймпад: курсор, прокрутка и клавиши\n\n"
        "  %s [-v] [--selftest] [--config PATH] [--device PATH]\n\n"
        "  -v, --verbose   печатать нажатия и найденное устройство\n"
        "  --selftest      проверить связку стики -> виртуальная мышь\n"
        "  --config PATH   другой файл конфига\n"
        "  --device PATH   конкретное устройство (/dev/input/eventN)\n",
        argv0);
    return 0;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGCHLD, SIG_IGN);

    bool verbose = false;
    bool do_selftest = false;
    std::string cfg_path = config_path();
    std::string device_override;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-v" || a == "--verbose") {
            verbose = true;
        } else if (a == "--selftest") {
            do_selftest = true;
        } else if (a == "--config" && i + 1 < argc) {
            cfg_path = argv[++i];
        } else if (a == "--device" && i + 1 < argc) {
            device_override = argv[++i];
        } else if (a == "-h" || a == "--help") {
            return usage(argv[0]);
        } else {
            std::fprintf(stderr, "неизвестный аргумент: %s\n", a.c_str());
            usage(argv[0]);
            return 1;
        }
    }

    Config cfg = load_config(cfg_path, verbose);
    if (do_selftest) return selftest(cfg);

    return run_daemon(std::move(cfg), verbose, device_override);
}
