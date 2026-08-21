#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>

#include "core/service.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t window_class_name[] = L"DshLauncherWindow";
constexpr int id_app_icon = 101;
constexpr UINT message_task = WM_APP + 1;
constexpr int id_primary = 101;
constexpr int id_stop = 102;
constexpr UINT_PTR id_auto_close = 201;
constexpr COLORREF background_color = RGB(246, 248, 252);
constexpr COLORREF surface_color = RGB(255, 255, 255);
constexpr COLORREF ink_color = RGB(30, 41, 59);
constexpr COLORREF primary_color = RGB(37, 99, 235);
constexpr COLORREF success_color = RGB(22, 163, 74);
constexpr COLORREF stopped_color = RGB(220, 38, 38);
constexpr COLORREF error_color = RGB(220, 38, 38);
constexpr COLORREF border_color = RGB(226, 232, 240);
constexpr COLORREF disabled_color = RGB(148, 163, 184);

std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

HFONT create_font(int points, UINT dpi, int weight = FW_NORMAL) {
    const int height = -MulDiv(points, static_cast<int>(dpi), 72);
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

class LauncherWindow {
public:
    explicit LauncherWindow(HINSTANCE instance) : instance_(instance) {}
    ~LauncherWindow() {
        alive_.store(false);
        for (auto font : {title_font_, status_font_, normal_font_, small_font_}) if (font) DeleteObject(font);
        if (background_brush_) DeleteObject(background_brush_);
        if (surface_brush_) DeleteObject(surface_brush_);
    }

    bool create() {
        HDC screen = GetDC(nullptr);
        dpi_ = static_cast<UINT>(GetDeviceCaps(screen, LOGPIXELSY));
        ReleaseDC(nullptr, screen);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = static_window_proc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(id_app_icon));
        wc.hIconSm = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(id_app_icon), IMAGE_ICON,
                                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        wc.hbrBackground = CreateSolidBrush(background_color);
        wc.lpszClassName = window_class_name;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

        title_font_ = create_font(22, dpi_, FW_BOLD);
        status_font_ = create_font(15, dpi_, FW_SEMIBOLD);
        normal_font_ = create_font(11, dpi_);
        small_font_ = create_font(9, dpi_);
        background_brush_ = CreateSolidBrush(background_color);
        surface_brush_ = CreateSolidBrush(surface_color);
        hwnd_ = CreateWindowExW(0, window_class_name, L"DSH Launcher",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, scale(586), scale(500),
                                nullptr, nullptr, instance_, this);
        return hwnd_ != nullptr;
    }

    int run(int show_command) {
        const DWORD rounded = 2;
        DwmSetWindowAttribute(hwnd_, 33, &rounded, sizeof(rounded));
        ShowWindow(hwnd_, show_command);
        UpdateWindow(hwnd_);
        begin_auto_flow();
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

private:
    struct PostedTask { std::function<void()> action; };

    static LRESULT CALLBACK static_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<LauncherWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<LauncherWindow*>(create->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->window_proc(message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    LRESULT window_proc(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE: create_controls(); return 0;
        case WM_PAINT: paint(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_COMMAND:
            if (HIWORD(wparam) == BN_CLICKED) {
                KillTimer(hwnd_, id_auto_close);
                handle_command(LOWORD(wparam));
            }
            return 0;
        case WM_TIMER:
            if (wparam == id_auto_close) {
                KillTimer(hwnd_, id_auto_close);
                DestroyWindow(hwnd_);
            }
            return 0;
        case WM_DRAWITEM: draw_button(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam)); return TRUE;
        case WM_CTLCOLORSTATIC: {
            auto dc = reinterpret_cast<HDC>(wparam);
            SetBkMode(dc, TRANSPARENT);
            const auto control = reinterpret_cast<HWND>(lparam);
            SetTextColor(dc, control == status_label_ ? status_color_ : ink_color);
            RECT rect{};
            GetWindowRect(control, &rect);
            MapWindowPoints(HWND_DESKTOP, hwnd_, reinterpret_cast<POINT*>(&rect), 2);
            return reinterpret_cast<LRESULT>((control == status_label_ || control == detail_label_ || control == action_label_)
                                                 ? surface_brush_ : background_brush_);
        }
        case WM_CTLCOLOREDIT: {
            auto dc = reinterpret_cast<HDC>(wparam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, ink_color);
            return reinterpret_cast<LRESULT>(surface_brush_);
        }
        case message_task: {
            std::unique_ptr<PostedTask> task(reinterpret_cast<PostedTask*>(lparam));
            task->action();
            return 0;
        }
        case WM_CLOSE: DestroyWindow(hwnd_); return 0;
        case WM_DESTROY: alive_.store(false); PostQuitMessage(0); return 0;
        default: return DefWindowProcW(hwnd_, message, wparam, lparam);
        }
    }

    HWND add_label(const wchar_t* text, int x, int y, int width, int height, HFONT font, DWORD style = SS_LEFT) {
        auto control = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | style,
                                       scale(x), scale(y), scale(width), scale(height), hwnd_, nullptr, instance_, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return control;
    }

    HWND add_button(const wchar_t* text, int id, int x, int y, int width) {
        auto control = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                       scale(x), scale(y), scale(width), scale(48), hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(normal_font_), TRUE);
        return control;
    }

    void create_controls() {
        add_label(L"DeepSeek Harness", 28, 20, 380, 38, title_font_);
        add_label(L"双击即可使用，其余交给启动器", 30, 58, 400, 22, small_font_);
        add_label(L"v0.1.0-dev", 442, 31, 104, 22, small_font_, SS_RIGHT);
        status_label_ = add_label(L"正在检查 DSH…", 52, 112, 456, 30, status_font_);
        detail_label_ = add_label(L"这通常只需要一小会儿", 52, 147, 456, 24, normal_font_);
        progress_ = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | PBS_MARQUEE,
                                    scale(52), scale(184), scale(456), scale(7), hwnd_, nullptr, instance_, nullptr);
        SendMessageW(progress_, PBM_SETBARCOLOR, 0, primary_color);
        SendMessageW(progress_, PBM_SETBKCOLOR, 0, RGB(226, 232, 240));
        SendMessageW(progress_, PBM_SETMARQUEE, TRUE, 24);
        add_label(L"执行记录", 30, 231, 160, 22, normal_font_);
        action_label_ = CreateWindowExW(0, L"EDIT", L"",
                                        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                            ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                                        scale(48), scale(265), scale(472), scale(72),
                                        hwnd_, nullptr, instance_, nullptr);
        SendMessageW(action_label_, WM_SETFONT, reinterpret_cast<WPARAM>(normal_font_), TRUE);
        primary_button_ = add_button(L"请稍候…", id_primary, 28, 363, 334);
        stop_button_ = add_button(L"停止服务", id_stop, 376, 363, 170);
        EnableWindow(primary_button_, FALSE);
        EnableWindow(stop_button_, FALSE);
        footer_label_ = add_label(L"正在本地检查 · 更新将在后台静默进行", 28, 427, 518, 22, small_font_, SS_CENTER);
    }

    void paint() {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd_, &ps);
        RECT client{};
        GetClientRect(hwnd_, &client);
        FillRect(dc, &client, background_brush_);
        HBRUSH shadow = CreateSolidBrush(RGB(231, 235, 243));
        HPEN shadow_pen = CreatePen(PS_SOLID, 1, RGB(231, 235, 243));
        auto old_brush = SelectObject(dc, shadow);
        auto old_pen = SelectObject(dc, shadow_pen);
        RoundRect(dc, scale(30), scale(94), scale(550), scale(215), scale(24), scale(24));
        HPEN surface_pen = CreatePen(PS_SOLID, 1, border_color);
        SelectObject(dc, surface_brush_);
        SelectObject(dc, surface_pen);
        RoundRect(dc, scale(28), scale(91), scale(548), scale(212), scale(24), scale(24));
        SelectObject(dc, surface_brush_);
        SelectObject(dc, surface_pen);
        RoundRect(dc, scale(28), scale(256), scale(548), scale(348), scale(20), scale(20));
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(shadow);
        DeleteObject(shadow_pen);
        DeleteObject(surface_pen);
        EndPaint(hwnd_, &ps);
    }

    void draw_button(const DRAWITEMSTRUCT& item) {
        wchar_t text[96]{};
        GetWindowTextW(item.hwndItem, text, 96);
        const bool disabled = (item.itemState & ODS_DISABLED) != 0;
        const bool pressed = (item.itemState & ODS_SELECTED) != 0;
        const bool primary = item.CtlID == id_primary;
        COLORREF fill = primary ? primary_color : surface_color;
        COLORREF outline = primary ? primary_color : border_color;
        COLORREF foreground = primary ? RGB(255, 255, 255) : ink_color;
        if (pressed) fill = primary ? RGB(29, 78, 216) : RGB(241, 245, 249);
        if (disabled) { fill = RGB(241, 245, 249); outline = border_color; foreground = disabled_color; }
        HBRUSH brush = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 1, outline);
        const auto old_brush = SelectObject(item.hDC, brush);
        const auto old_pen = SelectObject(item.hDC, pen);
        RoundRect(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom, 18, 18);
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, foreground);
        SelectObject(item.hDC, normal_font_);
        RECT text_rect = item.rcItem;
        if (pressed) OffsetRect(&text_rect, 0, 1);
        DrawTextW(item.hDC, text, -1, &text_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(item.hDC, old_brush);
        SelectObject(item.hDC, old_pen);
        DeleteObject(brush);
        DeleteObject(pen);
    }

    void handle_command(int id) {
        if (busy_.load()) return;
        if (id == id_primary) running_ ? open_web() : start_existing();
        else if (id == id_stop) stop();
    }

    void begin_auto_flow() {
        KillTimer(hwnd_, id_auto_close);
        append_action(L"正在检查本机环境");
        set_busy(L"正在检查 DSH…", L"这通常只需要一小会儿");
        background([this] {
            auto status = service_.detect();
            if (!status.installed) {
                std::string install_error;
                const bool installed = service_.ensure_installed(
                    [this](const std::string& step) { post([this, step] { append_action(utf8_to_wide(step)); }); },
                    install_error);
                if (!installed) { post([this, install_error] { show_error(install_error); }); return; }
                status = service_.detect();
            } else {
                post([this, status] { append_action(utf8_to_wide("已找到 DSH " + status.version)); });
            }
            if (status.running) {
                std::string ignored;
                service_.open_web(ignored);
                post([this, status] {
                    append_action(L"服务已在运行，网页已打开");
                    show_ready(status, L"服务已经在运行，已为你打开网页");
                });
                return;
            }
            post([this] {
                append_action(L"正在启动 DSH Web 服务");
                set_busy(L"正在启动 DSH…", L"首次启动可能需要几十秒");
            });
            std::string error;
            if (!service_.start(error)) { post([this, error] { show_error(error); }); return; }
            dsh::Status current;
            for (int index = 0; index < 180 && alive_.load(); ++index) {
                current = service_.detect();
                if (current.running) break;
                Sleep(500);
            }
            if (!current.running) { post([this] { show_error("DSH 启动超时，请查看日志后重试。"); }); return; }
            std::string ignored;
            service_.open_web(ignored);
            post([this, current] {
                append_action(L"启动完成，网页已打开");
                show_ready(current, L"已启动并为你打开网页");
            });
        });
    }

    void show_missing() {
        busy_.store(false);
        running_ = false;
        set_text(status_label_, L"尚未安装 DSH");
        set_text(detail_label_, L"下一步将自动安装，无需输入命令");
        set_text(primary_button_, L"安装并启动");
        set_text(footer_label_, L"正在迁移原 BAT 的 Node/npm 自动安装流程");
        SendMessageW(progress_, PBM_SETMARQUEE, FALSE, 0);
        ShowWindow(progress_, SW_HIDE);
        EnableWindow(primary_button_, FALSE);
        EnableWindow(stop_button_, FALSE);
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void show_ready(const dsh::Status& status, const wchar_t* detail) {
        busy_.store(false);
        running_ = true;
        status_color_ = success_color;
        set_text(status_label_, L"● DSH 已准备好");
        set_text(detail_label_, detail);
        set_text(primary_button_, L"打开 DSH 网页");
        set_text(footer_label_, utf8_to_wide("DSH " + status.version + " · 60 秒后自动关闭窗口，服务继续运行"));
        SendMessageW(progress_, PBM_SETMARQUEE, FALSE, 0);
        ShowWindow(progress_, SW_HIDE);
        EnableWindow(primary_button_, TRUE);
        EnableWindow(stop_button_, TRUE);
        SetTimer(hwnd_, id_auto_close, 60000, nullptr);
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void show_stopped() {
        busy_.store(false);
        running_ = false;
        status_color_ = stopped_color;
        set_text(status_label_, L"● DSH 已停止");
        set_text(detail_label_, L"需要时可以再次一键启动");
        set_text(primary_button_, L"重新启动 DSH");
        set_text(footer_label_, L"配置和会话数据已保留");
        ShowWindow(progress_, SW_HIDE);
        EnableWindow(primary_button_, TRUE);
        EnableWindow(stop_button_, FALSE);
        append_action(L"后台服务已停止");
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void show_error(const std::string& error) {
        busy_.store(false);
        running_ = false;
        status_color_ = error_color;
        set_text(status_label_, L"启动没有完成");
        set_text(detail_label_, utf8_to_wide(error));
        set_text(primary_button_, L"重试");
        set_text(footer_label_, L"日志保存在本机应用数据目录");
        ShowWindow(progress_, SW_HIDE);
        EnableWindow(primary_button_, TRUE);
        EnableWindow(stop_button_, FALSE);
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void set_busy(const wchar_t* status, const wchar_t* detail) {
        busy_.store(true);
        running_ = false;
        status_color_ = primary_color;
        set_text(status_label_, status);
        set_text(detail_label_, detail);
        set_text(primary_button_, L"请稍候…");
        set_text(footer_label_, L"正在处理 · 请不要重复打开启动器");
        ShowWindow(progress_, SW_SHOW);
        SendMessageW(progress_, PBM_SETMARQUEE, TRUE, 24);
        EnableWindow(primary_button_, FALSE);
        EnableWindow(stop_button_, FALSE);
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void start_existing() { begin_auto_flow(); }

    void stop() {
        append_action(L"正在停止 DSH 后台服务");
        set_busy(L"正在停止 DSH…", L"配置和会话数据不会删除");
        background([this] {
            std::string error;
            if (!service_.stop(error)) post([this, error] { show_error(error); });
            else post([this] { show_stopped(); });
        });
    }

    void open_web() {
        std::string error;
        if (!service_.open_web(error)) show_error(error);
        else append_action(L"已使用默认浏览器打开 DSH");
    }

    int scale(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }

    void append_action(const std::wstring& action) {
        if (action.empty()) return;
        actions_.push_back(L"• " + action);
        if (actions_.size() > 100) actions_.erase(actions_.begin());
        std::wstring joined;
        for (const auto& item : actions_) {
            if (!joined.empty()) joined += L"\r\n";
            joined += item;
        }
        set_text(action_label_, joined);
        SendMessageW(action_label_, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
        SendMessageW(action_label_, WM_VSCROLL, SB_BOTTOM, 0);
    }

    void set_text(HWND control, const std::wstring& text) { SetWindowTextW(control, text.c_str()); }
    void background(std::function<void()> action) { std::thread([this, action = std::move(action)] { if (alive_.load()) action(); }).detach(); }
    void post(std::function<void()> action) {
        if (!alive_.load()) return;
        auto* task = new PostedTask{std::move(action)};
        if (!PostMessageW(hwnd_, message_task, 0, reinterpret_cast<LPARAM>(task))) delete task;
    }

    HINSTANCE instance_{};
    HWND hwnd_{};
    HWND status_label_{};
    HWND detail_label_{};
    HWND action_label_{};
    HWND footer_label_{};
    HWND progress_{};
    HWND primary_button_{};
    HWND stop_button_{};
    HFONT title_font_{};
    HFONT status_font_{};
    HFONT normal_font_{};
    HFONT small_font_{};
    HBRUSH background_brush_{};
    HBRUSH surface_brush_{};
    UINT dpi_{96};
    COLORREF status_color_{primary_color};
    std::vector<std::wstring> actions_;
    dsh::Service service_;
    std::atomic_bool alive_{true};
    std::atomic_bool busy_{false};
    bool running_{};
};

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show_command) {
    using SetDpiAwareness = BOOL(WINAPI*)(HANDLE);
    if (const auto user32 = GetModuleHandleW(L"user32.dll")) {
        if (const auto set_awareness = reinterpret_cast<SetDpiAwareness>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) {
            set_awareness(reinterpret_cast<HANDLE>(-4));
        }
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    LauncherWindow window(instance);
    if (!window.create()) return 1;
    return window.run(show_command);
}

#endif
