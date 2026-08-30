#ifdef _WIN32

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00  // Windows 10: SetProcessDpiAwarenessContext etc.
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>

#include "core/service.h"
#include "core/semver.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t window_class_name[] = L"DshLauncherWindow";
constexpr wchar_t choice_dialog_class_name[] = L"DshLauncherChoiceDialog";
constexpr wchar_t launcher_version[] = L"0.1.1-beta.10";
constexpr int id_app_icon = 101;
constexpr UINT message_task = WM_APP + 1;
constexpr UINT message_choice_result = WM_APP + 2;
constexpr UINT message_tray = WM_APP + 3;
constexpr int id_primary = 101;
constexpr int id_stop = 102;
constexpr int id_details = 103;
constexpr int id_open_logs = 104;
constexpr int id_source = 105;
constexpr int id_uninstall_dsh = 106;
constexpr int id_uninstall_node = 107;
constexpr int id_uninstall_all = 108;
constexpr int id_check_updates = 109;
constexpr int id_update_all = 110;
constexpr int id_update_launcher = 111;
constexpr int id_update_dsh = 112;
constexpr int id_update_cancel = 113;
constexpr int id_settings = 114;
constexpr int id_back_home = 115;
constexpr int id_update_action = 116;
constexpr int id_update_action_menu = 117;
constexpr int id_update_notes = 118;
constexpr int id_settings_general = 119;
constexpr int id_settings_logs = 120;
constexpr int id_settings_maintenance = 121;
constexpr int id_choice_enter_updates = 122;
constexpr int id_choice_later = 123;
constexpr int id_settings_about = 124;
constexpr int id_source_menu = 125;
constexpr int id_source_mirror = 126;
constexpr int id_source_official = 127;
constexpr int id_settings_tray = 128;
constexpr int id_settings_close = 129;
constexpr UINT id_tray_restore = 301;
constexpr UINT id_tray_open_web = 302;
constexpr UINT id_tray_stop = 303;
constexpr UINT id_tray_exit = 304;
constexpr UINT_PTR id_auto_close = 201;
constexpr UINT_PTR id_elapsed = 202;
constexpr UINT_PTR id_progress = 203;
constexpr int collapsed_width = 586;
constexpr int collapsed_height = 500;
constexpr int expanded_height = 760;
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

HFONT create_icon_font(int points, UINT dpi) {
    const int height = -MulDiv(points, static_cast<int>(dpi), 72);
    return CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       // Segoe Fluent Icons is not present on many Windows 10
                       // installations. Segoe MDL2 Assets ships with Windows
                       // 10 and contains the same U+E713 Settings cog.
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
}

// Choice dialogs deliberately live in their own owned popup window. Native
// child controls always sit above parent painting, so a parent-drawn overlay
// can never reliably cover a Settings list-box or button on Windows 10.
class ChoiceDialog {
public:
    static bool show(HINSTANCE instance, HWND owner, UINT dpi, const std::wstring& title,
                     const std::wstring& detail, const std::wstring& primary,
                     const std::wstring& secondary, bool destructive,
                     bool show_memory_option = false,
                     std::wstring memory_label = L"保留对话记忆（推荐）",
                     std::wstring tertiary = {}, bool memory_checked = true) {
        auto* dialog = new ChoiceDialog(instance, owner, dpi, title, detail, primary, secondary,
                                        destructive, show_memory_option, std::move(memory_label),
                                        std::move(tertiary), memory_checked);
        if (!dialog->create()) {
            delete dialog;
            return false;
        }
        EnableWindow(owner, FALSE);
        ShowWindow(dialog->hwnd_, SW_SHOW);
        SetForegroundWindow(dialog->hwnd_);
        return true;
    }

private:
    ChoiceDialog(HINSTANCE instance, HWND owner, UINT dpi, std::wstring title,
                  std::wstring detail, std::wstring primary, std::wstring secondary,
                  bool destructive, bool show_memory_option, std::wstring memory_label,
                  std::wstring tertiary, bool memory_checked)
        : instance_(instance), owner_(owner), dpi_(dpi), title_(std::move(title)),
          detail_(std::move(detail)), primary_(std::move(primary)), secondary_(std::move(secondary)),
          memory_label_(std::move(memory_label)), tertiary_(std::move(tertiary)),
          destructive_(destructive), show_memory_option_(show_memory_option),
          memory_checked_(memory_checked) {}

    ~ChoiceDialog() {
        if (title_font_) DeleteObject(title_font_);
        if (text_font_) DeleteObject(text_font_);
        if (surface_brush_) DeleteObject(surface_brush_);
    }

    int scale(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }

    bool create() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = static_window_proc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.lpszClassName = choice_dialog_class_name;
        RegisterClassExW(&wc);

        RECT owner_rect{};
        GetWindowRect(owner_, &owner_rect);
        const int width = scale(410);
        const int height = scale(show_memory_option_ ? 252 : 212);
        const int x = (owner_rect.left + owner_rect.right - width) / 2;
        const int y = (owner_rect.top + owner_rect.bottom - height) / 2;
        hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOOLWINDOW, choice_dialog_class_name, L"",
                                WS_POPUP, x, y, width, height, owner_, nullptr, instance_, this);
        return hwnd_ != nullptr;
    }

    void create_controls() {
        title_font_ = create_font(15, dpi_, FW_SEMIBOLD);
        text_font_ = create_font(10, dpi_);
        surface_brush_ = CreateSolidBrush(RGB(255, 255, 255));
        title_label_ = CreateWindowExW(0, L"STATIC", title_.c_str(), WS_CHILD | WS_VISIBLE | SS_CENTER,
                                       scale(24), scale(28), scale(362), scale(32), hwnd_, nullptr, instance_, nullptr);
        detail_label_ = CreateWindowExW(0, L"STATIC", detail_.c_str(), WS_CHILD | WS_VISIBLE | SS_CENTER,
                                        scale(28), scale(66), scale(354), scale(show_memory_option_ ? 58 : 52), hwnd_, nullptr, instance_, nullptr);
        if (show_memory_option_) {
            memory_checkbox_ = CreateWindowExW(0, L"BUTTON", memory_label_.c_str(),
                                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                  scale(38), scale(132), scale(334), scale(26), hwnd_,
                                                  reinterpret_cast<HMENU>(4), instance_, nullptr);
            SendMessageW(memory_checkbox_, BM_SETCHECK, memory_checked_ ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        const int buttons_y = show_memory_option_ ? 184 : 148;
        const bool three_buttons = !tertiary_.empty();
        primary_button_ = CreateWindowExW(0, L"BUTTON", primary_.c_str(),
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                          scale(three_buttons ? 24 : 38), scale(buttons_y),
                                          scale(three_buttons ? 110 : 158), scale(40), hwnd_,
                                          reinterpret_cast<HMENU>(1), instance_, nullptr);
        secondary_button_ = CreateWindowExW(0, L"BUTTON", secondary_.c_str(),
                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                            scale(three_buttons ? 150 : 214), scale(buttons_y),
                                            scale(three_buttons ? 110 : 158), scale(40), hwnd_,
                                            reinterpret_cast<HMENU>(2), instance_, nullptr);
        if (three_buttons) {
            tertiary_button_ = CreateWindowExW(0, L"BUTTON", tertiary_.c_str(),
                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                               scale(276), scale(buttons_y), scale(110), scale(40), hwnd_,
                                               reinterpret_cast<HMENU>(3), instance_, nullptr);
        }
        SendMessageW(title_label_, WM_SETFONT, reinterpret_cast<WPARAM>(title_font_), TRUE);
        SendMessageW(detail_label_, WM_SETFONT, reinterpret_cast<WPARAM>(text_font_), TRUE);
        if (memory_checkbox_) SendMessageW(memory_checkbox_, WM_SETFONT, reinterpret_cast<WPARAM>(text_font_), TRUE);
        SendMessageW(primary_button_, WM_SETFONT, reinterpret_cast<WPARAM>(text_font_), TRUE);
        SendMessageW(secondary_button_, WM_SETFONT, reinterpret_cast<WPARAM>(text_font_), TRUE);
        if (tertiary_button_) SendMessageW(tertiary_button_, WM_SETFONT, reinterpret_cast<WPARAM>(text_font_), TRUE);
        const DWORD rounded = 2;
        DwmSetWindowAttribute(hwnd_, 33, &rounded, sizeof(rounded));
    }

    void draw_button(const DRAWITEMSTRUCT& item) const {
        const bool primary = item.CtlID == 1;
        const bool pressed = (item.itemState & ODS_SELECTED) != 0;
        const COLORREF fill = primary ? (destructive_ ? RGB(220, 38, 38) : RGB(37, 99, 235)) : RGB(255, 255, 255);
        const COLORREF outline = primary ? fill : RGB(226, 232, 240);
        const COLORREF foreground = primary ? RGB(255, 255, 255) : RGB(30, 41, 59);
        HBRUSH brush = CreateSolidBrush(pressed && primary ? (destructive_ ? RGB(185, 28, 28) : RGB(29, 78, 216)) : fill);
        HPEN pen = CreatePen(PS_SOLID, 1, outline);
        const auto old_brush = SelectObject(item.hDC, brush);
        const auto old_pen = SelectObject(item.hDC, pen);
        RoundRect(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom,
                  scale(12), scale(12));
        wchar_t text[96]{};
        GetWindowTextW(item.hwndItem, text, 96);
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, foreground);
        const auto old_font = SelectObject(item.hDC, text_font_);
        RECT area = item.rcItem;
        if (pressed) OffsetRect(&area, 0, scale(1));
        DrawTextW(item.hDC, text, -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(item.hDC, old_font);
        SelectObject(item.hDC, old_brush);
        SelectObject(item.hDC, old_pen);
        DeleteObject(brush);
        DeleteObject(pen);
    }

    void finish(int result) {
        if (finished_) return;
        finished_ = true;
        EnableWindow(owner_, TRUE);
        const bool preserve_memory = !show_memory_option_ ||
            SendMessageW(memory_checkbox_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        PostMessageW(owner_, message_choice_result, static_cast<WPARAM>(result), preserve_memory ? 1 : 0);
        DestroyWindow(hwnd_);
    }

    static LRESULT CALLBACK static_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<ChoiceDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<ChoiceDialog*>(create->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
        switch (message) {
        case WM_CREATE: self->create_controls(); return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rect{};
            GetClientRect(hwnd, &rect);
            FillRect(dc, &rect, self->surface_brush_);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            auto dc = reinterpret_cast<HDC>(wparam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(30, 41, 59));
            return reinterpret_cast<LRESULT>(self->surface_brush_);
        }
        case WM_DRAWITEM: self->draw_button(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam)); return TRUE;
        case WM_COMMAND:
            if (LOWORD(wparam) == 1) self->finish(1);
            else if (LOWORD(wparam) == 2) self->finish(0);
            else if (LOWORD(wparam) == 3) self->finish(2);
            return 0;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) self->finish(self->tertiary_.empty() ? 0 : 2);
            return 0;
        case WM_CLOSE: self->finish(self->tertiary_.empty() ? 0 : 2); return 0;
        case WM_NCDESTROY: {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            const auto result = DefWindowProcW(hwnd, message, wparam, lparam);
            delete self;
            return result;
        }
        default: return DefWindowProcW(hwnd, message, wparam, lparam);
        }
    }

    HINSTANCE instance_{};
    HWND owner_{};
    HWND hwnd_{};
    HWND title_label_{};
    HWND detail_label_{};
    HWND primary_button_{};
    HWND secondary_button_{};
    HWND tertiary_button_{};
    HWND memory_checkbox_{};
    UINT dpi_{96};
    std::wstring title_;
    std::wstring detail_;
    std::wstring primary_;
    std::wstring secondary_;
    std::wstring memory_label_;
    std::wstring tertiary_;
    bool destructive_{};
    bool show_memory_option_{};
    bool memory_checked_{};
    bool finished_{};
    HFONT title_font_{};
    HFONT text_font_{};
    HBRUSH surface_brush_{};
};

// Informational notices are intentionally non-modal, but still must be a
// separate popup: painting a toast in the parent puts it behind native child
// controls and produces the same white cut-through seen in the screenshots.
class ToastWindow {
public:
    static void show(HINSTANCE instance, HWND owner, UINT dpi, const std::wstring& title,
                     const std::wstring& detail) {
        if (active_window_) DestroyWindow(active_window_);
        auto* toast = new ToastWindow(instance, owner, dpi, title, detail);
        if (!toast->create()) {
            delete toast;
            return;
        }
        active_window_ = toast->hwnd_;
        ShowWindow(toast->hwnd_, SW_SHOWNOACTIVATE);
        SetTimer(toast->hwnd_, 1, 5000, nullptr);
    }

private:
    ToastWindow(HINSTANCE instance, HWND owner, UINT dpi, std::wstring title, std::wstring detail)
        : instance_(instance), owner_(owner), dpi_(dpi), title_(std::move(title)), detail_(std::move(detail)) {}
    ~ToastWindow() {
        if (title_font_) DeleteObject(title_font_);
        if (detail_font_) DeleteObject(detail_font_);
        if (brush_) DeleteObject(brush_);
    }

    int scale(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }
    bool create() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = static_window_proc;
        wc.hInstance = instance_;
        wc.lpszClassName = L"DshLauncherToast";
        RegisterClassExW(&wc);
        RECT owner_rect{};
        GetWindowRect(owner_, &owner_rect);
        const int width = scale(236);
        const int height = scale(74);
        hwnd_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP,
                                owner_rect.right - width - scale(28), owner_rect.top + scale(76), width, height,
                                owner_, nullptr, instance_, this);
        return hwnd_ != nullptr;
    }
    void create_resources() {
        title_font_ = create_font(9, dpi_, FW_SEMIBOLD);
        detail_font_ = create_font(8, dpi_);
        brush_ = CreateSolidBrush(RGB(30, 41, 59));
        const DWORD rounded = 2;
        DwmSetWindowAttribute(hwnd_, 33, &rounded, sizeof(rounded));
    }
    static LRESULT CALLBACK static_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<ToastWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<ToastWindow*>(create->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
        switch (message) {
        case WM_CREATE: self->create_resources(); return 0;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_TIMER: DestroyWindow(hwnd); return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            const HDC dc = BeginPaint(hwnd, &ps);
            RECT rect{};
            GetClientRect(hwnd, &rect);
            FillRect(dc, &rect, self->brush_);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            auto old_font = SelectObject(dc, self->title_font_);
            RECT title_rect{self->scale(14), self->scale(12), self->scale(222), self->scale(32)};
            DrawTextW(dc, self->title_.c_str(), -1, &title_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, self->detail_font_);
            RECT detail_rect{self->scale(14), self->scale(34), self->scale(222), self->scale(65)};
            DrawTextW(dc, self->detail_.c_str(), -1, &detail_rect, DT_LEFT | DT_WORDBREAK);
            SelectObject(dc, old_font);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NCDESTROY: {
            if (active_window_ == hwnd) active_window_ = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            const auto result = DefWindowProcW(hwnd, message, wparam, lparam);
            delete self;
            return result;
        }
        default: return DefWindowProcW(hwnd, message, wparam, lparam);
        }
    }

    static inline HWND active_window_{};
    HINSTANCE instance_{};
    HWND owner_{};
    HWND hwnd_{};
    UINT dpi_{96};
    std::wstring title_;
    std::wstring detail_;
    HFONT title_font_{};
    HFONT detail_font_{};
    HBRUSH brush_{};
};

class LauncherWindow {
public:
    explicit LauncherWindow(HINSTANCE instance) : instance_(instance) {}
    ~LauncherWindow() {
        alive_.store(false);
        std::lock_guard<std::mutex> lock(workers_mutex_);
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
        if (class_registered_) UnregisterClassW(window_class_name, instance_);
        if (small_icon_) DestroyIcon(small_icon_);
        for (auto font : {title_font_, status_font_, section_font_, normal_font_, small_font_, settings_icon_font_}) if (font) DeleteObject(font);
        if (class_brush_) DeleteObject(class_brush_);
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
        small_icon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(id_app_icon), IMAGE_ICON,
                                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        wc.hIconSm = small_icon_;
        class_brush_ = CreateSolidBrush(background_color);
        wc.hbrBackground = class_brush_;
        wc.lpszClassName = window_class_name;
        if (RegisterClassExW(&wc)) class_registered_ = true;
        else if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

        title_font_ = create_font(22, dpi_, FW_BOLD);
        status_font_ = create_font(15, dpi_, FW_SEMIBOLD);
        section_font_ = create_font(11, dpi_, FW_SEMIBOLD);
        normal_font_ = create_font(11, dpi_);
        small_font_ = create_font(9, dpi_);
        settings_icon_font_ = create_icon_font(15, dpi_);
        background_brush_ = CreateSolidBrush(background_color);
        surface_brush_ = CreateSolidBrush(surface_color);
        hwnd_ = CreateWindowExW(0, window_class_name, L"DSH Launcher",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, scale(collapsed_width), scale(collapsed_height),
                                nullptr, nullptr, instance_, this);
        return hwnd_ != nullptr;
    }

    int run(int show_command) {
        const DWORD rounded = 2;
        DwmSetWindowAttribute(hwnd_, 33, &rounded, sizeof(rounded));
        ShowWindow(hwnd_, show_command);
        UpdateWindow(hwnd_);
        begin_initial_launch();
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

private:
    struct PostedTask { std::function<void()> action; };
    enum class ActionMode {
        normal,
        install_prompt,
        install_dsh_directory,
        install_node_source,
        install_node_directory,
        install_node_permission,
        install_ready,
        uninstall_confirm,
    };
    enum class Page { home, updates, settings };
    enum class SettingsSection { general, logs, maintenance, about };
    enum class UpdateAction { all, dsh, launcher };
    enum class ChoiceMode { none, update, uninstall, close };
    enum class UninstallTarget { none, dsh, node, both };

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
                if (service_.minimize_to_tray() && last_status_.running) {
                    append_action(L"已自动最小化到系统托盘");
                    minimize_to_tray();
                } else {
                    // Only an explicit title-bar close asks the user. The
                    // automatic timeout keeps its existing exit behavior.
                    DestroyWindow(hwnd_);
                }
            } else if (wparam == id_elapsed && busy_.load()) {
                const auto seconds = (GetTickCount64() - operation_started_) / 1000;
                set_text(footer_label_, L"正在处理 · 已用时 " + std::to_wstring(seconds) + L" 秒");
            } else if (wparam == id_progress && busy_.load()) {
                progress_position_ = (progress_position_ + 3) % 101;
                SendMessageW(progress_, PBM_SETPOS, progress_position_, 0);
            }
            return 0;
        case WM_DRAWITEM: draw_button(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam)); return TRUE;
        case WM_CTLCOLORSTATIC: {
            auto dc = reinterpret_cast<HDC>(wparam);
            const auto control = reinterpret_cast<HWND>(lparam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, control == status_label_ ? status_color_ : ink_color);
            const bool on_surface = control == status_label_ || control == detail_label_ ||
                                    control == update_info_heading_ || control == update_versions_label_ ||
                                    control == settings_content_title_ || control == settings_content_detail_ ||
                                    control == settings_hint_;
            return reinterpret_cast<LRESULT>(on_surface
                                                 ? surface_brush_ : background_brush_);
        }
        case WM_CTLCOLORLISTBOX: {
            auto dc = reinterpret_cast<HDC>(wparam);
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, surface_color);
            SetTextColor(dc, ink_color);
            return reinterpret_cast<LRESULT>(surface_brush_);
        }
        case message_task: {
            std::unique_ptr<PostedTask> task(reinterpret_cast<PostedTask*>(lparam));
            task->action();
            return 0;
        }
        case message_choice_result:
            handle_choice_result(static_cast<int>(wparam), lparam != 0);
            return 0;
        case message_tray: {
            const UINT event = LOWORD(lparam);
            if (event == WM_LBUTTONDBLCLK) restore_from_tray();
            else if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) show_tray_menu();
            return 0;
        }
        case WM_CLOSE:
            if (busy_.load()) {
                if (!close_busy_notice_shown_) {
                    close_busy_notice_shown_ = true;
                    show_notice(L"操作正在进行", L"请等待完成，或使用红色取消按钮");
                }
                return 0;
            }
            KillTimer(hwnd_, id_auto_close);
            if (service_.close_action() == dsh::CloseAction::tray) minimize_to_tray();
            else if (service_.close_action() == dsh::CloseAction::exit) DestroyWindow(hwnd_);
            else show_close_popup();
            return 0;
        case WM_DESTROY:
            remove_tray_icon();
            alive_.store(false);
            PostQuitMessage(0);
            return 0;
        default: return DefWindowProcW(hwnd_, message, wparam, lparam);
        }
    }

    HWND add_label(const wchar_t* text, int x, int y, int width, int height, HFONT font, DWORD style = SS_LEFT) {
        auto control = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | style,
                                       scale(x), scale(y), scale(width), scale(height), hwnd_, nullptr, instance_, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return control;
    }

    HWND add_button(const wchar_t* text, int id, int x, int y, int width, int height = 48) {
        auto control = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                       scale(x), scale(y), scale(width), scale(height), hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(normal_font_), TRUE);
        return control;
    }

    void create_controls() {
        page_title_ = add_label(L"DeepSeek Harness", 28, 20, 380, 38, title_font_);
        page_subtitle_ = add_label(L"双击即可使用，其余交给启动器", 30, 58, 400, 22, small_font_);
        version_label_ = add_label((std::wstring(L"v") + launcher_version).c_str(), 420, 31, 70, 22, small_font_, SS_RIGHT);
        settings_button_ = add_button(L"\uE713", id_settings, 514, 24, 32, 30);
        SendMessageW(settings_button_, WM_SETFONT, reinterpret_cast<WPARAM>(settings_icon_font_), TRUE);
        back_button_ = add_button(L"‹", id_back_home, 28, 24, 36, 34);
        SendMessageW(back_button_, WM_SETFONT, reinterpret_cast<WPARAM>(status_font_), TRUE);
        status_label_ = add_label(L"正在检查 DSH…", 52, 112, 456, 30, status_font_);
        detail_label_ = add_label(L"这通常只需要一小会儿", 52, 147, 456, 24, normal_font_);
        progress_ = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                    scale(52), scale(184), scale(456), scale(7), hwnd_, nullptr, instance_, nullptr);
        SendMessageW(progress_, PBM_SETBARCOLOR, 0, primary_color);
        SendMessageW(progress_, PBM_SETBKCOLOR, 0, RGB(226, 232, 240));
        SendMessageW(progress_, PBM_SETPOS, 0, 0);
        activity_heading_ = add_label(L"运行详情", 30, 228, 160, 24, section_font_);
        action_list_ = CreateWindowExW(0, L"LISTBOX", L"",
                                       WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                                           LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
                                       scale(28), scale(260), scale(520), scale(102),
                                       hwnd_, nullptr, instance_, nullptr);
        SendMessageW(action_list_, WM_SETFONT, reinterpret_cast<WPARAM>(normal_font_), TRUE);
        SendMessageW(action_list_, LB_SETITEMHEIGHT, 0, scale(24));
        primary_button_ = add_button(L"请稍候…", id_primary, 28, 390, 252);
        stop_button_ = add_button(L"停止服务", id_stop, 294, 390, 120);
        check_updates_button_ = add_button(L"检查更新", id_check_updates, 294, 390, 252);
        EnableWindow(primary_button_, FALSE);
        EnableWindow(stop_button_, FALSE);
        footer_label_ = add_label(L"正在本地检查", 28, 450, 520, 22, small_font_, SS_CENTER);

        update_info_heading_ = add_label(L"可用更新", 46, 101, 160, 22, section_font_);
        update_versions_label_ = add_label(L"正在读取版本信息", 48, 130, 470, 42, normal_font_);
        update_notes_button_ = add_button(L"展开更新说明", id_update_notes, 400, 96, 130, 30);
        update_notes_list_ = CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
                                              scale(28), scale(274), scale(520), scale(54), hwnd_, nullptr, instance_, nullptr);
        SendMessageW(update_notes_list_, WM_SETFONT, reinterpret_cast<WPARAM>(small_font_), TRUE);
        source_select_button_ = add_button(L"国内镜像", id_source, 28, 342, 176, 46);
        source_menu_button_ = add_button(L"⌄", id_source_menu, 204, 342, 44, 46);
        source_mirror_button_ = add_button(L"国内镜像（默认）", id_source_mirror, 28, 396, 220, 36);
        source_official_button_ = add_button(L"官方源", id_source_official, 28, 432, 220, 36);
        update_action_button_ = add_button(L"一键更新", id_update_action, 260, 342, 240, 46);
        update_action_menu_button_ = add_button(L"⌄", id_update_action_menu, 500, 342, 46, 46);
        update_all_button_ = add_button(L"一键更新", id_update_all, 260, 396, 286, 36);
        update_dsh_button_ = add_button(L"仅更新 DSH", id_update_dsh, 260, 432, 286, 36);
        update_launcher_button_ = add_button(L"仅更新启动器", id_update_launcher, 260, 468, 286, 36);
        update_cancel_button_ = add_button(L"取消并返回首页", id_update_cancel, 28, 522, 220, 40);
        SendMessageW(source_menu_button_, WM_SETFONT, reinterpret_cast<WPARAM>(normal_font_), TRUE);
        SendMessageW(update_action_menu_button_, WM_SETFONT, reinterpret_cast<WPARAM>(normal_font_), TRUE);

        settings_nav_general_ = add_button(L"通用", id_settings_general, 38, 112, 140, 38);
        settings_nav_logs_ = add_button(L"日志与诊断", id_settings_logs, 38, 158, 140, 38);
        settings_nav_maintenance_ = add_button(L"安装与管理", id_settings_maintenance, 38, 204, 140, 38);
        settings_nav_about_ = add_button(L"关于", id_settings_about, 38, 250, 140, 38);
        settings_content_title_ = add_label(L"通用", 230, 112, 286, 30, status_font_);
        settings_content_detail_ = add_label(L"更多设置将在后续版本提供。", 230, 152, 286, 58, normal_font_);
        settings_tray_button_ = add_button(L"", id_settings_tray, 230, 230, 286, 44);
        settings_close_button_ = add_button(L"", id_settings_close, 230, 286, 286, 44);
        settings_info_list_ = CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
                                               scale(230), scale(220), scale(286), scale(76), hwnd_, nullptr, instance_, nullptr);
        SendMessageW(settings_info_list_, WM_SETFONT, reinterpret_cast<WPARAM>(small_font_), TRUE);
        settings_hint_ = add_label(L"", 230, 404, 286, 22, small_font_);
        open_logs_button_ = add_button(L"打开日志目录", id_open_logs, 230, 230, 220, 38);
        uninstall_dsh_button_ = add_button(L"卸载 DSH", id_uninstall_dsh, 230, 308, 286, 38);
        // Retained only to keep the old control ID harmless while the settings
        // page is migrated. The final design deliberately has no standalone
        // “uninstall Node.js” action: user-owned Node.js must never be guessed.
        uninstall_node_button_ = add_button(L"", id_uninstall_node, 0, 0, 1, 1);
        uninstall_all_button_ = add_button(L"卸载 DSH + Node.js", id_uninstall_all, 230, 356, 286, 38);

        for (auto control : {back_button_, update_info_heading_, update_versions_label_, update_notes_button_,
                             update_notes_list_, source_select_button_, source_menu_button_, source_mirror_button_, source_official_button_, update_action_button_, update_action_menu_button_,
                             update_all_button_, update_dsh_button_, update_launcher_button_, update_cancel_button_,
                             settings_nav_general_, settings_nav_logs_, settings_nav_maintenance_, settings_nav_about_, settings_content_title_, settings_content_detail_, settings_tray_button_, settings_close_button_, settings_info_list_, settings_hint_,
                             open_logs_button_, uninstall_dsh_button_, uninstall_node_button_,
                             uninstall_all_button_}) {
            ShowWindow(control, SW_HIDE);
        }
        update_source_selector();
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
        HPEN surface_pen = CreatePen(PS_SOLID, 1, border_color);
        if (page_ == Page::home) {
            RoundRect(dc, scale(30), scale(94), scale(550), scale(215), scale(24), scale(24));
            SelectObject(dc, surface_brush_);
            SelectObject(dc, surface_pen);
            RoundRect(dc, scale(28), scale(91), scale(548), scale(212), scale(24), scale(24));
            RoundRect(dc, scale(28), scale(252), scale(548), scale(370), scale(20), scale(20));
        } else if (page_ == Page::updates) {
            SelectObject(dc, surface_brush_);
            SelectObject(dc, surface_pen);
            if (busy_.load()) {
                RoundRect(dc, scale(28), scale(88), scale(548), scale(190), scale(20), scale(20));
                RoundRect(dc, scale(28), scale(208), scale(548), scale(392), scale(20), scale(20));
            } else {
                RoundRect(dc, scale(28), scale(88), scale(548), scale(326), scale(20), scale(20));
            }
            const auto draw_version_row = [this, dc](int top, const wchar_t* name,
                                                      const std::wstring& current,
                                                      const std::wstring& available, bool has_update) {
                HBRUSH row_brush = CreateSolidBrush(RGB(248, 250, 252));
                HPEN row_pen = CreatePen(PS_SOLID, 1, RGB(226, 232, 240));
                const auto previous_brush = SelectObject(dc, row_brush);
                const auto previous_pen = SelectObject(dc, row_pen);
                RoundRect(dc, scale(46), scale(top), scale(530), scale(top + 44), scale(12), scale(12));
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, ink_color);
                const auto previous_font = SelectObject(dc, normal_font_);
                RECT name_rect{scale(60), scale(top + 8), scale(126), scale(top + 36)};
                DrawTextW(dc, name, -1, &name_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                SetTextColor(dc, RGB(71, 85, 105));
                RECT current_rect{scale(128), scale(top + 8), scale(294), scale(top + 36)};
                DrawTextW(dc, current.c_str(), -1, &current_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                SetTextColor(dc, has_update ? primary_color : RGB(100, 116, 139));
                RECT available_rect{scale(302), scale(top + 8), scale(516), scale(top + 36)};
                DrawTextW(dc, available.c_str(), -1, &available_rect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                SelectObject(dc, previous_font);
                SelectObject(dc, previous_brush);
                SelectObject(dc, previous_pen);
                DeleteObject(row_brush);
                DeleteObject(row_pen);
            };
            if (!busy_.load()) {
                draw_version_row(132, L"DSH", utf8_to_wide(last_status_.installation_incomplete ? "安装不完整" :
                                                              last_status_.version.empty() ? "未安装" : last_status_.version),
                                 dsh_update_available_ ? (L"→ " + utf8_to_wide(latest_version_))
                                                       : dsh_version_checked_ ? L"已是最新" : L"未能检查",
                                 dsh_update_available_);
                draw_version_row(184, L"启动器", std::wstring(launcher_version),
                                 launcher_update_ ? (L"→ " + utf8_to_wide(launcher_update_->version))
                                                  : launcher_version_checked_ ? L"已是最新" : L"未能检查",
                                 launcher_update_.has_value());
            }
        } else {
            SelectObject(dc, surface_brush_);
            SelectObject(dc, surface_pen);
            RoundRect(dc, scale(28), scale(88), scale(190), scale(440), scale(20), scale(20));
            RoundRect(dc, scale(202), scale(88), scale(548), scale(440), scale(20), scale(20));
        }
        if (page_ == Page::updates && (source_menu_visible_ || update_action_menu_visible_)) {
            HBRUSH menu_shadow = CreateSolidBrush(RGB(226, 232, 240));
            HPEN menu_shadow_pen = CreatePen(PS_SOLID, 1, RGB(226, 232, 240));
            SelectObject(dc, menu_shadow);
            SelectObject(dc, menu_shadow_pen);
            if (source_menu_visible_) RoundRect(dc, scale(31), scale(400), scale(251), scale(473), scale(12), scale(12));
            if (update_action_menu_visible_) RoundRect(dc, scale(263), scale(400), scale(549), scale(509), scale(12), scale(12));
            SelectObject(dc, surface_brush_);
            SelectObject(dc, surface_pen);
            if (source_menu_visible_) RoundRect(dc, scale(28), scale(396), scale(248), scale(470), scale(12), scale(12));
            if (update_action_menu_visible_) RoundRect(dc, scale(260), scale(396), scale(546), scale(506), scale(12), scale(12));
            DeleteObject(menu_shadow);
            DeleteObject(menu_shadow_pen);
        }
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
        const bool install_entry_primary = item.CtlID == id_check_updates &&
                                           action_mode_ == ActionMode::install_prompt;
        const bool primary = (item.CtlID == id_primary && action_mode_ != ActionMode::install_prompt) ||
                             install_entry_primary || item.CtlID == id_update_action ||
                             (item.CtlID == id_update_all && update_action_menu_visible_);
        const bool update_option = item.CtlID == id_update_all || item.CtlID == id_update_dsh ||
                                   item.CtlID == id_update_launcher;
        const bool source_option = item.CtlID == id_source_mirror || item.CtlID == id_source_official;
        const bool source_selected = (item.CtlID == id_source_mirror && service_.install_source() == dsh::InstallSource::mirror) ||
                                     (item.CtlID == id_source_official && service_.install_source() == dsh::InstallSource::official);
        const bool settings_nav = item.CtlID == id_settings_general || item.CtlID == id_settings_logs ||
                                  item.CtlID == id_settings_maintenance || item.CtlID == id_settings_about;
        const bool selected_nav = (item.CtlID == id_settings_general && settings_section_ == SettingsSection::general) ||
                                  (item.CtlID == id_settings_logs && settings_section_ == SettingsSection::logs) ||
                                  (item.CtlID == id_settings_maintenance && settings_section_ == SettingsSection::maintenance) ||
                                  (item.CtlID == id_settings_about && settings_section_ == SettingsSection::about);
        const bool icon_button = item.CtlID == id_settings || item.CtlID == id_back_home;
        const bool split_arrow = item.CtlID == id_source_menu || item.CtlID == id_update_action_menu;
        const bool danger_button = item.CtlID == id_uninstall_dsh || item.CtlID == id_uninstall_all;
        const bool cancel_button = danger_button || item.CtlID == id_update_cancel ||
                                   (item.CtlID == id_stop && (busy_.load() || action_mode_ == ActionMode::uninstall_confirm ||
                                                               action_mode_ == ActionMode::install_node_permission)) ||
                                   (item.CtlID == id_primary && action_mode_ == ActionMode::install_prompt);
        COLORREF fill = primary ? primary_color : surface_color;
        COLORREF outline = primary ? primary_color : border_color;
        COLORREF foreground = primary ? RGB(255, 255, 255) : ink_color;
        if ((update_option && !primary) || source_option || item.CtlID == id_source || item.CtlID == id_source_menu) {
            fill = RGB(239, 246, 255);
            outline = RGB(191, 219, 254);
            foreground = RGB(71, 85, 105);
        }
        if (source_option && source_selected) {
            fill = primary_color;
            outline = primary_color;
            foreground = RGB(255, 255, 255);
        }
        if (settings_nav) {
            fill = selected_nav ? RGB(229, 239, 255) : surface_color;
            outline = selected_nav ? RGB(219, 234, 254) : surface_color;
            foreground = selected_nav ? primary_color : RGB(71, 85, 105);
        }
        if (icon_button) {
            fill = background_color;
            outline = background_color;
            foreground = RGB(100, 116, 139);
        }
        if (item.CtlID == id_back_home) {
            fill = surface_color;
            outline = RGB(226, 232, 240);
            foreground = RGB(148, 163, 184);
        }
        if (split_arrow) {
            fill = RGB(248, 250, 252);
            outline = RGB(226, 232, 240);
            foreground = RGB(100, 116, 139);
        }
        if (cancel_button) {
            fill = RGB(220, 38, 38);
            outline = RGB(220, 38, 38);
            foreground = RGB(255, 255, 255);
        }
        if (pressed) fill = primary ? RGB(29, 78, 216) : RGB(241, 245, 249);
        if (pressed && cancel_button) fill = RGB(185, 28, 28);
        if (disabled) {
            fill = cancel_button ? RGB(254, 226, 226) : RGB(241, 245, 249);
            outline = cancel_button ? RGB(254, 202, 202) : border_color;
            foreground = cancel_button ? RGB(248, 113, 113) : disabled_color;
        }
        HBRUSH brush = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 1, outline);
        const auto old_brush = SelectObject(item.hDC, brush);
        const auto old_pen = SelectObject(item.hDC, pen);
        const int radius = icon_button ? 20 : 14;
        RoundRect(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom, radius, radius);
        const bool geometric_icon = item.CtlID == id_back_home || split_arrow;
        if (geometric_icon) {
            const int center_x = (item.rcItem.left + item.rcItem.right) / 2;
            const int center_y = (item.rcItem.top + item.rcItem.bottom) / 2 - scale(1);
            HPEN icon_pen = CreatePen(PS_SOLID, (std::max)(1, scale(1)), foreground);
            const auto previous_icon_pen = SelectObject(item.hDC, icon_pen);
            const auto previous_icon_brush = SelectObject(item.hDC, GetStockObject(NULL_BRUSH));
            const bool up = split_arrow && std::wstring(text) == L"⌃";
            const int width = scale(split_arrow ? 5 : 4);
            const int height = scale(split_arrow ? 3 : 6);
            if (split_arrow) {
                MoveToEx(item.hDC, center_x - width, center_y + (up ? height : -height), nullptr);
                LineTo(item.hDC, center_x, center_y + (up ? -height : height));
                LineTo(item.hDC, center_x + width, center_y + (up ? height : -height));
            } else {
                MoveToEx(item.hDC, center_x + width / 2, center_y - height, nullptr);
                LineTo(item.hDC, center_x - width / 2, center_y);
                LineTo(item.hDC, center_x + width / 2, center_y + height);
            }
            SelectObject(item.hDC, previous_icon_brush);
            SelectObject(item.hDC, previous_icon_pen);
            DeleteObject(icon_pen);
        } else {
            SetBkMode(item.hDC, TRANSPARENT);
            SetTextColor(item.hDC, foreground);
            SelectObject(item.hDC, item.CtlID == id_settings ? settings_icon_font_ : normal_font_);
            RECT text_rect = item.rcItem;
            if (pressed) OffsetRect(&text_rect, 0, 1);
            DrawTextW(item.hDC, text, -1, &text_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(item.hDC, old_brush);
        SelectObject(item.hDC, old_pen);
        DeleteObject(brush);
        DeleteObject(pen);
    }

    void handle_command(int id) {
        if (choice_visible_) return;
        if (id == id_settings) { show_settings_page(SettingsSection::general); return; }
        if (id == id_back_home) { show_home(last_status_); return; }
        if (id == id_open_logs) { open_logs(); return; }
        // Settings is read-only while an operation is active.  Its navigation
        // must remain usable; the previous busy guard accidentally swallowed
        // these commands even though the worker thread was separate from UI.
        if (page_ == Page::settings) {
            if (id == id_settings_general) { show_settings_page(SettingsSection::general); return; }
            if (id == id_settings_logs) { show_settings_page(SettingsSection::logs); return; }
            if (id == id_settings_maintenance) { show_settings_page(SettingsSection::maintenance); return; }
            if (id == id_settings_about) { show_settings_page(SettingsSection::about); return; }
            if (id == id_settings_tray && !busy_.load()) { toggle_tray_setting(); return; }
            if (id == id_settings_close && !busy_.load()) { cycle_close_setting(); return; }
        }
        if (busy_.load() && (id == id_stop || id == id_update_cancel) && cancellable_.load()) {
            request_cancel();
            return;
        }
        if (busy_.load()) return;
        if (id == id_check_updates) { start_update_check(); return; }
        if (page_ == Page::updates) {
            if (id == id_update_notes) { toggle_update_notes(); return; }
            if (id == id_source || id == id_source_menu) { toggle_source_menu(); return; }
            if (id == id_source_mirror) { select_source(dsh::InstallSource::mirror); return; }
            if (id == id_source_official) { select_source(dsh::InstallSource::official); return; }
            if (id == id_update_action_menu) { toggle_update_action_menu(); return; }
            if (id == id_update_action) { start_selected_update(); return; }
            if (id == id_update_all) { select_update_action(UpdateAction::all); return; }
            if (id == id_update_dsh) { select_update_action(UpdateAction::dsh); return; }
            if (id == id_update_launcher) { select_update_action(UpdateAction::launcher); return; }
            if (id == id_update_cancel) {
                append_action(L"已取消更新选择");
                show_home(last_status_);
                return;
            }
        }
        if (id == id_uninstall_dsh) { request_uninstall(UninstallTarget::dsh); return; }
        if (id == id_uninstall_all) { request_uninstall(UninstallTarget::both); return; }
        if (action_mode_ == ActionMode::install_prompt) {
            if (id == id_primary) DestroyWindow(hwnd_);
            else if (id == id_stop) show_dsh_directory_step();
            else if (id == id_check_updates) start_one_click_install();
            return;
        }
        if (action_mode_ == ActionMode::install_dsh_directory) {
            if (id == id_primary) select_dsh_directory(service_.default_dsh_directory());
            else if (id == id_stop) {
                if (const auto directory = choose_folder(L"选择 DSH 安装目录")) select_dsh_directory(*directory);
            } else if (id == id_check_updates) show_install_prompt(last_status_, initial_environment_);
            return;
        }
        if (action_mode_ == ActionMode::install_node_source) {
            if (id == id_primary) select_node_source(dsh::InstallSource::mirror);
            else if (id == id_stop) select_node_source(dsh::InstallSource::official);
            else if (id == id_check_updates) show_dsh_directory_step();
            return;
        }
        if (action_mode_ == ActionMode::install_node_directory) {
            if (id == id_primary) select_node_directory(default_node_directory());
            else if (id == id_stop) {
                if (const auto directory = choose_folder(L"选择 Node.js 安装目录")) select_node_directory(*directory);
            } else if (id == id_check_updates) show_node_source_step();
            return;
        }
        if (action_mode_ == ActionMode::install_node_permission) {
            if (id == id_primary) start_install(pending_dsh_directory_, true);
            else if (id == id_stop) show_install_prompt(last_status_, initial_environment_);
            else if (id == id_check_updates) show_node_directory_step();
            return;
        }
        if (action_mode_ == ActionMode::install_ready) {
            if (id == id_primary) start_install(pending_dsh_directory_, false);
            else if (id == id_stop) show_dsh_directory_step();
            else if (id == id_check_updates) show_install_prompt(last_status_, initial_environment_);
            return;
        }
        if (id == id_primary) running_ ? open_web() : start_existing();
        else if (id == id_stop) stop();
    }

    void begin_initial_launch() {
        // Match the BAT's primary purpose: open DSH first. Version checks are
        // useful, but must not keep a user waiting before their local service
        // and browser become ready.
        initial_launch_ = true;
        initial_service_ready_ = false;
        initial_update_finished_ = false;
        initial_update_available_ = false;
        initial_update_check_failed_ = false;
        append_action(L"启动 DSH，同时在后台检查可用更新");
        start_service_direct(true);
        background([this] { check_for_updates(true, true); });
    }

    void show_home(const dsh::Status& status) {
        page_ = Page::home;
        hide_update_menus();
        resize_window(collapsed_height);
        last_status_ = status;
        running_ = status.running;
        set_text(page_title_, L"DeepSeek Harness");
        set_text(page_subtitle_, L"双击即可使用，其余交给启动器");
        SetWindowPos(page_title_, nullptr, scale(28), scale(20), scale(380), scale(38), SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(page_subtitle_, nullptr, scale(30), scale(58), scale(400), scale(22), SWP_NOZORDER | SWP_NOACTIVATE);
        // Before DSH exists there is nothing safe to manage or uninstall, so
        // first-install users only see the installation flow, not Settings.
        ShowWindow(settings_button_, status.installed ? SW_SHOW : SW_HIDE);
        ShowWindow(back_button_, SW_HIDE);
        for (auto control : {update_info_heading_, update_versions_label_, update_notes_button_, update_notes_list_,
                             source_select_button_, source_menu_button_, source_mirror_button_, source_official_button_,
                             update_action_button_, update_action_menu_button_, update_all_button_,
                             update_dsh_button_, update_launcher_button_, update_cancel_button_, settings_nav_general_,
                             settings_nav_logs_, settings_nav_maintenance_, settings_nav_about_, settings_content_title_, settings_content_detail_, settings_tray_button_, settings_close_button_, settings_info_list_, settings_hint_,
                             open_logs_button_, uninstall_dsh_button_, uninstall_node_button_, uninstall_all_button_}) {
            ShowWindow(control, SW_HIDE);
        }
        ShowWindow(activity_heading_, SW_SHOW);
        ShowWindow(action_list_, SW_SHOW);
        ShowWindow(footer_label_, SW_SHOW);
        ShowWindow(status_label_, SW_SHOW);
        ShowWindow(detail_label_, SW_SHOW);
        ShowWindow(progress_, busy_.load() ? SW_SHOW : SW_HIDE);
        SetWindowPos(activity_heading_, nullptr, scale(30), scale(228), scale(160), scale(24), SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(action_list_, nullptr, scale(28), scale(260), scale(520), scale(102), SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(progress_, nullptr, scale(52), scale(184), scale(456), scale(7), SWP_NOZORDER | SWP_NOACTIVATE);
        if (!busy_.load()) {
            status_color_ = status.running ? success_color : stopped_color;
            if (status.running) {
                set_text(status_label_, L"● DSH 正在运行");
                set_text(detail_label_, L"服务已就绪，可打开网页或停止服务");
                set_text(primary_button_, L"打开 DSH 网页");
                set_text(stop_button_, L"停止服务");
                SetWindowPos(primary_button_, nullptr, scale(28), scale(390), scale(164), scale(48), SWP_NOZORDER | SWP_NOACTIVATE);
                SetWindowPos(stop_button_, nullptr, scale(204), scale(390), scale(164), scale(48), SWP_NOZORDER | SWP_NOACTIVATE);
                SetWindowPos(check_updates_button_, nullptr, scale(380), scale(390), scale(166), scale(48), SWP_NOZORDER | SWP_NOACTIVATE);
                ShowWindow(stop_button_, SW_SHOW);
            } else {
                set_text(status_label_, status.installed ? L"● DSH 已停止" :
                                            status.installation_incomplete ? L"● DSH 安装不完整" : L"● 尚未安装 DSH");
                set_text(detail_label_, status.installed ? L"需要时点击启动 DSH" :
                                            status.installation_incomplete ? L"点击修复 DSH，将在原安装目录安全重装"
                                                                           : L"点击启动 DSH 后选择安装位置");
                set_text(primary_button_, status.installed ? L"启动 DSH" :
                                            status.installation_incomplete ? L"修复 DSH" : L"安装 DSH");
                SetWindowPos(primary_button_, nullptr, scale(28), scale(390), scale(252), scale(48), SWP_NOZORDER | SWP_NOACTIVATE);
                SetWindowPos(check_updates_button_, nullptr, scale(294), scale(390), scale(252), scale(48), SWP_NOZORDER | SWP_NOACTIVATE);
                ShowWindow(stop_button_, SW_HIDE);
            }
            set_text(footer_label_, status.installed ? L"更新需通过“检查更新”进入更新页" : L"安装与更新前都会先显示确认");
            ShowWindow(progress_, SW_HIDE);
            EnableWindow(primary_button_, TRUE);
            EnableWindow(stop_button_, status.running);
            EnableWindow(check_updates_button_, TRUE);
        }
        ShowWindow(primary_button_, SW_SHOW);
        ShowWindow(check_updates_button_, SW_SHOW);
        refresh_runtime_details();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void show_install_prompt(const dsh::Status& status, const dsh::EnvironmentStatus& environment) {
        KillTimer(hwnd_, id_elapsed);
        KillTimer(hwnd_, id_progress);
        busy_.store(false);
        cancellable_.store(false);
        cancel_requested_.store(false);
        initial_environment_ = environment;
        show_home(status);
        action_mode_ = ActionMode::install_prompt;
        const auto repair_directory = service_.default_dsh_directory();
        const bool can_repair_previous_install = std::filesystem::exists(repair_directory);
        set_text(status_label_, can_repair_previous_install ? L"● 检测到未完成的 DSH 安装" : L"● 检测到尚未安装 DSH");
        if (can_repair_previous_install) {
            set_text(detail_label_, L"将优先在原安装目录重新安装并修复，不会新建第二份 DSH。 ");
        } else {
            set_text(detail_label_, environment.has_node && environment.has_npm
                                        ? L"Node.js 已就绪。选择安装方式后即可开始。"
                                        : L"还需要 Node.js LTS；安装器会先说明来源、目录与授权。 ");
        }
        set_text(primary_button_, L"取消");
        set_text(stop_button_, L"开始安装");
        set_text(check_updates_button_, can_repair_previous_install ? L"一键修复" : L"一键安装！");
        SetWindowPos(primary_button_, nullptr, scale(28), scale(390), scale(160), scale(48), SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(stop_button_, nullptr, scale(202), scale(390), scale(160), scale(48), SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(check_updates_button_, nullptr, scale(376), scale(390), scale(170), scale(48), SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(stop_button_, SW_SHOW);
        EnableWindow(primary_button_, TRUE);
        EnableWindow(stop_button_, TRUE);
        EnableWindow(check_updates_button_, TRUE);
        set_text(footer_label_, can_repair_previous_install
                                    ? L"“一键修复”会保留原 DSH_HOME、插件、会话与设置，只重建程序文件"
                                    : L"“一键安装”使用国内镜像、默认目录，并在需要时请求管理员授权 · 懒人必备，一步到位");
        append_action(can_repair_previous_install ? L"检测到未完成的 DSH 安装，等待修复方式选择"
                                                   : L"检测到 DSH 未安装，等待选择安装方式");
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    std::filesystem::path default_node_directory() const {
        wchar_t program_files[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(L"ProgramFiles", program_files, MAX_PATH);
        if (length > 0 && length < MAX_PATH) return std::filesystem::path(program_files) / L"nodejs";
        return L"C:\\Program Files\\nodejs";
    }

    void show_install_step(const wchar_t* title, const wchar_t* detail,
                           const wchar_t* primary, const wchar_t* secondary,
                           const wchar_t* back, ActionMode mode) {
        show_home(last_status_);
        action_mode_ = mode;
        set_text(status_label_, title);
        set_text(detail_label_, detail);
        set_text(primary_button_, primary);
        set_text(stop_button_, secondary);
        set_text(check_updates_button_, back);
        SetWindowPos(primary_button_, nullptr, scale(28), scale(390), scale(164), scale(48), SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(stop_button_, nullptr, scale(204), scale(390), scale(164), scale(48), SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(check_updates_button_, nullptr, scale(380), scale(390), scale(166), scale(48), SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(stop_button_, SW_SHOW);
        ShowWindow(check_updates_button_, SW_SHOW);
        EnableWindow(primary_button_, TRUE);
        EnableWindow(stop_button_, TRUE);
        EnableWindow(check_updates_button_, TRUE);
        ShowWindow(settings_button_, SW_HIDE);
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void show_dsh_directory_step() {
        show_install_step(L"选择 DSH 安装目录", L"默认目录适合大多数用户；自定义目录会保留在该位置。",
                          L"默认目录", L"自定义目录", L"上一步", ActionMode::install_dsh_directory);
        set_text(footer_label_, L"默认目录：" + service_.default_dsh_directory().wstring());
        append_action(L"等待选择 DSH 安装目录");
    }

    void select_dsh_directory(const std::filesystem::path& directory) {
        pending_dsh_directory_ = directory;
        const auto environment = service_.environment();
        initial_environment_ = environment;
        append_action(L"已选择 DSH 目录：" + directory.wstring());
        if (environment.has_node && environment.has_npm) show_dsh_install_confirmation();
        else show_node_source_step();
    }

    void show_node_source_step() {
        show_install_step(L"安装 Node.js LTS", L"DSH 依赖 Node.js 与 npm。国内镜像适合中国大陆网络；官方源通常需要稳定的国际网络。",
                          L"国内镜像", L"官方源", L"上一步", ActionMode::install_node_source);
        set_text(footer_label_, L"Node.js 将安装为系统运行环境；下一步可选择默认或自定义目录");
        append_action(L"Node.js 未就绪，等待选择下载源");
    }

    void select_node_source(dsh::InstallSource source) {
        pending_node_source_ = source;
        append_action(source == dsh::InstallSource::mirror ? L"Node.js 下载源：国内镜像（npmmirror）"
                                                            : L"Node.js 下载源：官方源（winget）");
        show_node_directory_step();
    }

    void show_node_directory_step() {
        show_install_step(L"选择 Node.js 安装位置", L"Node.js 是 DSH 的运行基础，会安装 node 与 npm。默认位置是 Windows 的标准位置。",
                          L"默认目录", L"自定义目录", L"上一步", ActionMode::install_node_directory);
        set_text(footer_label_, L"默认目录：" + default_node_directory().wstring());
        append_action(L"等待选择 Node.js 安装目录");
    }

    void select_node_directory(const std::filesystem::path& directory) {
        pending_node_directory_ = directory;
        show_install_step(L"需要管理员授权", L"将使用 Windows 安装器安装 Node.js LTS；系统会弹出 UAC 确认。授权后安装 Node.js，再从国内 npm 镜像安装 DSH。",
                          L"授权并安装", L"取消", L"上一步", ActionMode::install_node_permission);
        set_text(footer_label_, L"Node.js 目录：" + directory.wstring());
        append_action(L"已选择 Node.js 目录，等待管理员授权");
    }

    void show_dsh_install_confirmation() {
        show_install_step(L"准备安装 DSH", L"Node.js 与 npm 已可用。将使用国内 npm 镜像安装 DSH，完成后自动启动并打开网页。",
                          L"开始安装", L"重选目录", L"上一步", ActionMode::install_ready);
        set_text(footer_label_, L"DSH 目录：" + pending_dsh_directory_.wstring());
        append_action(L"Node.js 已就绪，等待确认安装 DSH");
    }

    void start_one_click_install() {
        pending_dsh_directory_ = service_.default_dsh_directory();
        pending_node_directory_ = default_node_directory();
        pending_node_source_ = dsh::InstallSource::mirror;
        const auto environment = service_.environment();
        initial_environment_ = environment;
        append_action(L"一键安装：默认 DSH 目录、国内镜像与默认 Node.js 目录");
        start_install(pending_dsh_directory_, !(environment.has_node && environment.has_npm));
    }

    void check_for_updates(bool automatic, bool passive_initial = false) {
        const auto status = service_.detect();
        const auto environment = service_.environment();
        std::optional<std::string> latest;
        if (status.installed) latest = service_.latest_version(&cancel_requested_);
        // Do not begin the launcher-manifest network chain after DSH metadata
        // was cancelled.  This was one of the paths that made “取消” look as
        // though it had been ignored.
        if (cancel_requested_.load()) {
            post([this, passive_initial] {
                if (!passive_initial) show_cancelled(L"更新检查已取消，未修改任何本机文件");
            });
            return;
        }
        const auto launcher = service_.latest_launcher_update(&cancel_requested_);
        if (cancel_requested_.load()) {
            post([this, passive_initial] {
                if (!passive_initial) {
                    show_home(last_status_);
                    show_notice(L"检查已取消", L"没有修改任何本机文件");
                }
            });
            return;
        }
        const bool dsh_update = status.installed && latest && !status.version.empty() &&
                                dsh::version::is_newer(*latest, status.version);
        const bool launcher_update_available = launcher &&
                                                dsh::version::is_newer(launcher->version, "0.1.1-beta.10");
        const auto dsh_choice = dsh_update ? latest : std::optional<std::string>{};
        const auto launcher_choice = launcher_update_available ? launcher
                                                                : std::optional<dsh::platform::LauncherUpdate>{};
        post([this, status, environment, latest, launcher, dsh_choice, launcher_choice, dsh_update,
              launcher_update_available, automatic, passive_initial] {
            if (passive_initial) {
                node_version_ = environment.node_version;
                npm_version_ = environment.npm_version;
                latest_version_ = latest.value_or("");
                latest_launcher_version_ = launcher ? launcher->version : "";
                dsh_version_checked_ = !status.installed || latest.has_value();
                launcher_version_checked_ = launcher.has_value();
                dsh_update_available_ = dsh_update;
                launcher_update_ = launcher_choice;
                initial_dsh_choice_ = dsh_choice.value_or("");
                initial_launcher_choice_ = launcher_choice;
                initial_update_available_ = dsh_update || launcher_update_available;
                initial_update_check_failed_ = (status.installed && !dsh_version_checked_) ||
                                               !launcher_version_checked_;
                initial_update_finished_ = true;
                if (initial_update_available_) append_action(L"后台检查发现可用更新");
                else if (initial_update_check_failed_) append_action(L"后台检查未能确认全部版本");
                else append_action(L"后台检查完成，当前没有可用更新");
                finish_initial_launch();
                return;
            }
            busy_.store(false);
            cancellable_.store(false);
            KillTimer(hwnd_, id_elapsed);
            KillTimer(hwnd_, id_progress);
            last_status_ = status;
            node_version_ = environment.node_version;
            npm_version_ = environment.npm_version;
            latest_version_ = latest.value_or("");
            latest_launcher_version_ = launcher ? launcher->version : "";
            dsh_version_checked_ = !status.installed || latest.has_value();
            launcher_version_checked_ = launcher.has_value();
            dsh_update_available_ = dsh_update;
            launcher_update_ = launcher_choice;
            show_home(status);
            if (dsh_update || launcher_update_available) {
                append_action(L"发现可用更新，等待选择");
                show_choice_popup(dsh_choice.value_or(""), launcher_choice);
                return;
            }
            if ((status.installed && !dsh_version_checked_) || !launcher_version_checked_) {
                append_action(status.installed && !dsh_version_checked_
                                  ? L"未能确认 DSH 最新版本，请稍后重试"
                                  : L"未能确认启动器最新版本，请稍后重试");
                show_notice(L"更新检查未完成",
                            status.installed && !dsh_version_checked_
                                ? L"未能确认 DSH 最新版本；没有执行更新"
                                : L"未能确认启动器最新版本；没有执行更新");
            } else {
                append_action(L"当前未发现可用更新");
                show_notice(L"已完成更新检查",
                            status.installed ? L"DSH 与启动器均已是最新版本" : L"尚未安装 DSH，可点击启动进行安装");
            }
            if (automatic && status.running) {
                append_action(L"服务已在运行，不重复打开网页");
                set_text(footer_label_, service_.minimize_to_tray()
                                               ? L"DSH 正在后台运行 · 30 秒后最小化到系统托盘"
                                               : L"DSH 正在后台运行 · 30 秒后关闭窗口");
                SetTimer(hwnd_, id_auto_close, 30000, nullptr);
            }
        });
    }

    void finish_initial_launch() {
        if (!initial_launch_ || !initial_service_ready_ || !initial_update_finished_) return;
        if (initial_update_available_) {
            KillTimer(hwnd_, id_auto_close);
            append_action(L"发现可用更新，请选择更新内容");
            show_choice_popup(initial_dsh_choice_, initial_launcher_choice_);
            return;
        }
        if (initial_update_check_failed_) {
            // Do not silently disappear when the program cannot verify a
            // version. The user needs the visible diagnostic rather than a
            // false “latest” result.
            show_notice(L"更新检查未完成", L"无法确认全部版本，启动器将保持打开");
            return;
        }
        append_action(service_.minimize_to_tray()
                          ? L"没有可用更新，30 秒后最小化到系统托盘"
                          : L"没有可用更新，30 秒后自动关闭启动器");
        set_text(footer_label_, service_.minimize_to_tray()
                                       ? L"DSH 已在后台运行 · 30 秒后最小化到系统托盘"
                                       : L"DSH 已在后台运行 · 30 秒后关闭窗口");
        KillTimer(hwnd_, id_auto_close);
        SetTimer(hwnd_, id_auto_close, 30000, nullptr);
    }

    void start_update_check() {
        append_action(L"开始手动检查 DSH 和启动器更新");
        set_busy(L"正在检查更新…", L"正在检查 DSH、启动器与当前下载源", true);
        background([this] { check_for_updates(false); });
    }

    void show_choice_popup(const std::string& latest,
                           const std::optional<dsh::platform::LauncherUpdate>& launcher_update) {
        choice_return_page_ = page_;
        choice_return_settings_section_ = settings_section_;
        choice_visible_ = true;
        choice_mode_ = ChoiceMode::update;
        std::wstring description;
        if (!latest.empty()) description += utf8_to_wide("DSH " + last_status_.version + " → " + latest);
        if (launcher_update) {
            if (!description.empty()) description += L"\n";
            description += std::wstring(L"启动器 ") + launcher_version + L" → " + utf8_to_wide(launcher_update->version);
        }
        if (!ChoiceDialog::show(instance_, hwnd_, dpi_, L"发现可用更新",
                                description.empty() ? L"检测到可安装的更新" : description,
                                L"查看更新", L"稍后更新", false)) {
            choice_visible_ = false;
            choice_mode_ = ChoiceMode::none;
            show_error("无法显示更新选择窗口。");
        }
    }

    void show_close_popup() {
        if (choice_visible_) return;
        choice_visible_ = true;
        choice_mode_ = ChoiceMode::close;
        if (!ChoiceDialog::show(instance_, hwnd_, dpi_, L"关闭 DSH Launcher？",
                                L"选择最小化到系统托盘，或完全退出启动器。\nDSH 服务将继续在后台运行。",
                                L"退出启动器", L"最小化到托盘", true, true,
                                L"记住我的选择", L"取消", false)) {
            choice_visible_ = false;
            choice_mode_ = ChoiceMode::none;
            show_error("无法显示关闭选择窗口。");
        }
    }

    void hide_choice_popup() {
        choice_visible_ = false;
        choice_mode_ = ChoiceMode::none;
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void handle_choice_result(int result, bool preserve_memory = true) {
        if (!choice_visible_) return;
        const bool accepted = result == 1;
        const auto mode = choice_mode_;
        const auto return_page = choice_return_page_;
        const auto return_section = choice_return_settings_section_;
        hide_choice_popup();
        if (mode == ChoiceMode::update) {
            if (accepted) {
                show_update_page();
            } else {
                append_action(L"已选择稍后更新");
                if (return_page == Page::settings) show_settings_page(return_section);
                else show_home(last_status_);
            }
            return;
        }
        if (mode == ChoiceMode::close) {
            if (result == 2) return;
            const auto action = accepted ? dsh::CloseAction::exit : dsh::CloseAction::tray;
            if (preserve_memory) {
                std::string error;
                if (!service_.set_close_action(action, error)) {
                    show_error(error);
                    return;
                }
            }
            if (action == dsh::CloseAction::exit) DestroyWindow(hwnd_);
            else minimize_to_tray();
            return;
        }
        if (mode == ChoiceMode::uninstall) {
            if (accepted) start_uninstall(pending_uninstall_, preserve_memory);
            else {
                pending_uninstall_ = UninstallTarget::none;
                append_action(L"已取消卸载");
                show_settings_page(SettingsSection::maintenance);
            }
        }
    }

    void show_notice(const wchar_t* title, const wchar_t* detail) {
        ToastWindow::show(instance_, hwnd_, dpi_, title, detail);
    }

    void show_update_page() {
        page_ = Page::updates;
        resize_window(610);
        hide_update_menus();
        update_notes_visible_ = false;
        set_text(page_title_, L"更新");
        set_text(page_subtitle_, L"选择更新内容后开始下载与校验");
        SetWindowPos(page_title_, nullptr, scale(120), scale(20), scale(330), scale(38), SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(page_subtitle_, nullptr, scale(120), scale(58), scale(340), scale(22), SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(settings_button_, SW_HIDE);
        ShowWindow(back_button_, SW_SHOW);
        for (auto control : {status_label_, detail_label_, progress_, activity_heading_, action_list_, footer_label_,
                             primary_button_, stop_button_, check_updates_button_, settings_nav_general_, settings_nav_logs_,
                             settings_nav_maintenance_, settings_nav_about_, settings_content_title_, settings_content_detail_, settings_tray_button_, settings_close_button_, settings_info_list_, settings_hint_, open_logs_button_,
                             uninstall_dsh_button_, uninstall_node_button_, uninstall_all_button_}) {
            ShowWindow(control, SW_HIDE);
        }
        for (auto control : {update_info_heading_, update_notes_button_, source_select_button_, source_menu_button_,
                             update_action_button_, update_action_menu_button_, update_cancel_button_}) {
            ShowWindow(control, SW_SHOW);
        }
        ShowWindow(update_versions_label_, SW_HIDE);
        update_source_selector();
        select_update_action(UpdateAction::all, false);
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void toggle_update_notes() {
        update_notes_visible_ = !update_notes_visible_;
        set_text(update_notes_button_, update_notes_visible_ ? L"收起更新说明" : L"展开更新说明");
        ShowWindow(update_notes_list_, update_notes_visible_ ? SW_SHOW : SW_HIDE);
        if (update_notes_visible_) {
            SendMessageW(update_notes_list_, LB_RESETCONTENT, 0, 0);
            if (dsh_update_available_) {
                const auto text = std::wstring(L"DSH：将从 ") + utf8_to_wide(last_status_.version) + L" 更新到 " +
                                  utf8_to_wide(latest_version_) + L"，更新前会停止当前 DSH 服务。";
                SendMessageW(update_notes_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
            }
            if (launcher_update_) {
                const auto text = L"启动器：下载并完成 SHA-256 校验后，将自动替换并重启启动器。";
                SendMessageW(update_notes_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
            }
        }
    }

    void toggle_update_action_menu() {
        if (source_menu_visible_) toggle_source_menu();
        update_action_menu_visible_ = !update_action_menu_visible_;
        for (auto control : {update_all_button_, update_dsh_button_, update_launcher_button_}) {
            ShowWindow(control, update_action_menu_visible_ ? SW_SHOW : SW_HIDE);
        }
        EnableWindow(update_all_button_, dsh_update_available_ || launcher_update_.has_value());
        EnableWindow(update_dsh_button_, dsh_update_available_);
        EnableWindow(update_launcher_button_, launcher_update_.has_value());
        set_text(update_action_menu_button_, update_action_menu_visible_ ? L"⌃" : L"⌄");
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void select_update_action(UpdateAction action, bool close_menu = true) {
        selected_update_action_ = action;
        const wchar_t* text = action == UpdateAction::all ? L"一键更新"
                              : action == UpdateAction::dsh ? L"仅更新 DSH" : L"仅更新启动器";
        set_text(update_action_button_, text);
        if (close_menu && update_action_menu_visible_) toggle_update_action_menu();
    }

    void start_selected_update() {
        if (selected_update_action_ == UpdateAction::all) start_update_all();
        else if (selected_update_action_ == UpdateAction::dsh) start_update();
        else start_launcher_update();
    }

    void start_update_all() {
        if (launcher_update_ && !dsh_update_available_) {
            start_launcher_update();
            return;
        }
        if (!dsh_update_available_) {
            show_error("当前没有可执行的更新。");
            return;
        }
        update_launcher_after_dsh_ = launcher_update_.has_value();
        start_update();
    }

    void start_launcher_update() {
        if (!launcher_update_) {
            show_error("启动器 Release 清单尚未提供有效的 Windows x64 包，暂不能更新。");
            return;
        }
        action_mode_ = ActionMode::normal;
        append_action(L"开始更新启动器，当前 DSH 服务将先停止");
        set_busy(L"正在更新启动器…", L"正在下载、校验并准备重启启动器，可以取消", true);
        set_text(stop_button_, L"取消更新");
        background([this, update = *launcher_update_, was_running = last_status_.running] {
            std::string error;
            if (last_status_.running) {
                std::string stop_error;
                if (!service_.stop(stop_error)) {
                    post([this, stop_error] { show_error("无法在更新启动器前停止 DSH：" + stop_error); });
                    return;
                }
            }
            const auto restore_service = [&] {
                if (!was_running) return;
                std::string restart_error;
                if (!service_.start(restart_error)) {
                    error += "；更新失败后重新启动 DSH 也失败：" + restart_error;
                } else {
                    error += "；更新失败，已重新启动 DSH 服务。";
                }
            };
            if (!service_.update_launcher(update,
                    [this](const std::string& step) { post([this, step] { append_action(utf8_to_wide(step)); }); },
                    error, &cancel_requested_)) {
                restore_service();
                if (cancel_requested_.load()) post([this] { show_cancelled(L"启动器更新已取消，当前文件未替换"); });
                else post([this, error] { show_error(error); });
                return;
            }
            post([this] {
                append_action(L"启动器更新已校验，正在关闭旧进程并重启");
                busy_.store(false);
                DestroyWindow(hwnd_);
            });
        });
    }

    void start_update() {
        action_mode_ = ActionMode::normal;
        append_action(L"开始更新 DSH，原安装目录将保持不变");
        set_busy(L"正在更新 DSH…", L"完成版本校验后才会启动服务，可以随时取消", true);
        set_text(stop_button_, L"取消更新");
        background([this] {
            std::string error;
            if (!service_.update(
                    [this](const std::string& step) { post([this, step] { append_action(utf8_to_wide(step)); }); },
                    error, &cancel_requested_)) {
                if (cancel_requested_.load()) post([this] { show_cancelled(L"更新已取消，原安装目录未继续处理"); });
                else post([this, error] { show_error(error); });
                return;
            }
            const auto verified = service_.detect();
            post([this, verified] {
                const auto version = verified.version.empty() ? L"已完成版本校验" :
                    utf8_to_wide("已更新至 " + verified.version + "，版本校验通过");
                append_action(L"DSH 更新完成：" + version);
                if (update_launcher_after_dsh_ && launcher_update_) {
                    update_launcher_after_dsh_ = false;
                    append_action(L"一键更新：DSH 已完成，继续更新启动器");
                    show_notice(L"DSH 更新完成", (version + L"。即将继续更新启动器。").c_str());
                    start_launcher_update();
                    return;
                }
                show_notice(L"DSH 更新完成", (version + L"。正在启动服务并打开网页。").c_str());
                start_service_direct();
            });
        });
    }

    void request_cancel() {
        if (!busy_.load() || !cancellable_.load()) return;
        cancel_requested_.store(true);
        cancellable_.store(false);
        set_text(detail_label_, L"正在终止下载和相关子进程…");
        if (page_ == Page::updates) {
            set_text(update_info_heading_, L"正在取消更新…");
            set_text(update_versions_label_, L"已停止继续尝试更新源，正在退出当前请求");
        }
        set_text(stop_button_, L"正在取消…");
        EnableWindow(primary_button_, FALSE);
        EnableWindow(stop_button_, FALSE);
        EnableWindow(check_updates_button_, FALSE);
        EnableWindow(source_select_button_, FALSE);
        EnableWindow(source_menu_button_, FALSE);
        EnableWindow(update_cancel_button_, FALSE);
        append_action(L"收到取消请求，正在安全终止当前任务");
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void show_cancelled(const wchar_t* detail) {
        KillTimer(hwnd_, id_elapsed);
        KillTimer(hwnd_, id_progress);
        busy_.store(false);
        cancellable_.store(false);
        action_mode_ = ActionMode::normal;
        last_status_ = service_.detect();
        show_home(last_status_);
        status_color_ = stopped_color;
        set_text(status_label_, L"● 操作已取消");
        set_text(detail_label_, detail);
        append_action(L"当前任务已取消");
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void show_ready(const dsh::Status& status, const wchar_t* detail) {
        KillTimer(hwnd_, id_elapsed);
        KillTimer(hwnd_, id_progress);
        busy_.store(false);
        cancellable_.store(false);
        action_mode_ = ActionMode::normal;
        last_status_ = status;
        show_home(status);
        status_color_ = success_color;
        set_text(status_label_, L"● DSH 已准备好");
        set_text(detail_label_, detail);
        if (initial_launch_) {
            set_text(footer_label_, utf8_to_wide("DSH " + status.version) + L" · 正在后台检查更新");
        } else {
            set_text(footer_label_, utf8_to_wide("DSH " + status.version) +
                                        (service_.minimize_to_tray()
                                             ? L" · 30 秒后最小化到系统托盘"
                                             : L" · 30 秒后关闭窗口，服务继续在后台运行"));
            SetTimer(hwnd_, id_auto_close, 30000, nullptr);
        }
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void show_stopped() {
        KillTimer(hwnd_, id_elapsed);
        KillTimer(hwnd_, id_progress);
        busy_.store(false);
        cancellable_.store(false);
        action_mode_ = ActionMode::normal;
        last_status_.running = false;
        show_home(last_status_);
        status_color_ = stopped_color;
        set_text(status_label_, L"● DSH 已停止");
        set_text(detail_label_, L"需要时可以再次一键启动");
        append_action(L"后台服务已停止");
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void show_error(const std::string& error) {
        KillTimer(hwnd_, id_elapsed);
        KillTimer(hwnd_, id_progress);
        busy_.store(false);
        cancellable_.store(false);
        last_status_ = service_.detect();
        show_home(last_status_);
        status_color_ = error_color;
        set_text(status_label_, L"操作没有完成");
        set_text(detail_label_, utf8_to_wide(error));
        set_text(footer_label_, L"日志保存在本机应用数据目录");
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void set_busy(const wchar_t* status, const wchar_t* detail, bool cancellable = false) {
        busy_.store(true);
        cancellable_.store(cancellable);
        cancel_requested_.store(false);
        close_busy_notice_shown_ = false;
        status_color_ = primary_color;
        set_text(status_label_, status);
        set_text(detail_label_, detail);
        set_text(primary_button_, L"请稍候…");
        set_text(footer_label_, L"正在处理 · 请不要重复打开启动器");
        operation_started_ = GetTickCount64();
        SetTimer(hwnd_, id_elapsed, 1000, nullptr);
        progress_position_ = 0;
        SendMessageW(progress_, PBM_SETPOS, progress_position_, 0);
        SetTimer(hwnd_, id_progress, 80, nullptr);
        ShowWindow(progress_, SW_SHOW);
        if (page_ == Page::updates) {
            set_text(update_info_heading_, status);
            set_text(update_versions_label_, detail);
            SetWindowPos(progress_, nullptr, scale(46), scale(144), scale(484), scale(7), SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(activity_heading_, nullptr, scale(46), scale(224), scale(180), scale(24), SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(action_list_, nullptr, scale(46), scale(256), scale(484), scale(116), SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(update_cancel_button_, nullptr, scale(28), scale(410), scale(220), scale(40), SWP_NOZORDER | SWP_NOACTIVATE);
            ShowWindow(update_notes_button_, SW_HIDE);
            ShowWindow(source_select_button_, SW_HIDE);
            ShowWindow(source_menu_button_, SW_HIDE);
            ShowWindow(source_mirror_button_, SW_HIDE);
            ShowWindow(source_official_button_, SW_HIDE);
            ShowWindow(update_action_button_, SW_HIDE);
            ShowWindow(update_action_menu_button_, SW_HIDE);
            ShowWindow(update_all_button_, SW_HIDE);
            ShowWindow(update_dsh_button_, SW_HIDE);
            ShowWindow(update_launcher_button_, SW_HIDE);
            set_text(update_cancel_button_, cancellable ? L"取消更新" : L"正在处理…");
            ShowWindow(update_cancel_button_, SW_SHOW);
            EnableWindow(update_cancel_button_, cancellable);
            ShowWindow(activity_heading_, SW_SHOW);
            ShowWindow(action_list_, SW_SHOW);
        } else {
            ShowWindow(primary_button_, SW_SHOW);
            ShowWindow(check_updates_button_, SW_SHOW);
            ShowWindow(stop_button_, cancellable ? SW_SHOW : SW_HIDE);
            EnableWindow(primary_button_, FALSE);
            set_text(stop_button_, cancellable ? L"取消操作" : L"停止服务");
            EnableWindow(stop_button_, cancellable);
            EnableWindow(check_updates_button_, FALSE);
        }
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void start_existing() {
        start_service_direct();
    }

    void start_service_direct(bool initial_launch = false) {
        append_action(L"开始启动 DSH Web 服务");
        set_busy(L"正在启动 DSH…", L"启动操作不会再次检查更新");
        background([this, initial_launch] {
            std::string error;
            const bool was_already_running = service_.is_running();
            if (!service_.start(error)) {
                if (error.find("未检测到 dsh") != std::string::npos || error.find("安装不完整") != std::string::npos) {
                    const auto status = service_.detect();
                    const auto environment = service_.environment();
                    post([this, status, environment, initial_launch] {
                        if (initial_launch) initial_launch_ = false;
                        show_install_prompt(status, environment);
                    });
                } else {
                    post([this, error, initial_launch] {
                        if (initial_launch) initial_launch_ = false;
                        show_error(error);
                    });
                }
                return;
            }
            dsh::Status current;
            for (int index = 0; index < 120 && alive_.load(); ++index) {
                if (service_.is_running()) {
                    // A listener alone can be a process that is about to
                    // exit. Require a second HTTP response before promising
                    // that the browser can connect.
                    Sleep(500);
                    if (!service_.is_running()) continue;
                    // Obtaining DSH's version starts a separate CLI process.
                    // Do that once after the health probe succeeds, not on
                    // every polling tick.
                    current = service_.detect();
                    break;
                }
                Sleep(250);
            }
            if (!current.running) {
                post([this, initial_launch] {
                    if (initial_launch) initial_launch_ = false;
                    show_error("DSH 启动超时，请查看日志后重试。");
                });
                return;
            }
            post([this, current, was_already_running, initial_launch] {
                append_action(was_already_running ? L"服务已在运行，未重复打开网页"
                                                  : L"DSH 已启动，并由 DSH 自身请求打开网页");
                show_ready(current, was_already_running ? L"服务已在运行，未重复打开网页"
                                                        : L"服务已启动，DSH 已请求打开网页");
                if (initial_launch) {
                    initial_service_ready_ = true;
                    finish_initial_launch();
                }
            });
        });
    }

    void start_install(const std::filesystem::path& directory, bool install_system_node = false) {
        action_mode_ = ActionMode::normal;
        // Initial setup follows the BAT's proven default: npmmirror for DSH.
        // The user's persisted source setting remains owned by the update page.
        const auto source = dsh::InstallSource::mirror;
        dsh::NodeInstallOptions node_options;
        node_options.install_system_node = install_system_node;
        node_options.source = pending_node_source_;
        node_options.directory = pending_node_directory_;
        append_action(L"安装目录：" + directory.wstring());
        set_busy(L"正在准备安装…", install_system_node
                                       ? L"将先请求管理员权限安装 Node.js，再安装 DSH"
                                       : L"将使用现有 Node.js 安装 DSH", true);
        background([this, directory, source, node_options] {
            std::string error;
            const bool installed = service_.install_at(
                directory, source,
                [this](const std::string& step) {
                    post([this, step] { append_action(utf8_to_wide(step)); });
                }, error, &cancel_requested_, &node_options);
            if (!installed) {
                if (cancel_requested_.load()) post([this] { show_cancelled(L"安装已取消，未继续处理 DSH 文件"); });
                else post([this, error] { show_error(error); });
                return;
            }
            const auto verified = service_.detect();
            post([this, verified] {
                const auto version = verified.version.empty() ? L"已完成版本校验" :
                    utf8_to_wide("已安装 " + verified.version + "，版本校验通过");
                append_action(L"DSH 安装完成：" + version);
                show_notice(L"DSH 安装完成", (version + L"。现在将启动 DSH Web 服务。").c_str());
                start_service_direct();
            });
        });
    }

    std::optional<std::filesystem::path> choose_folder(const wchar_t* title) {
        IFileDialog* dialog{};
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dialog)))) {
            show_error("无法打开系统文件夹选择器。");
            return std::nullopt;
        }
        DWORD options{};
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(title);
        if (dialog->Show(hwnd_) != S_OK) {
            dialog->Release();
            return std::nullopt;
        }
        IShellItem* item{};
        std::optional<std::filesystem::path> selected;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            PWSTR path{};
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                selected = std::filesystem::path(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
        dialog->Release();
        return selected;
    }

    void update_source_selector() {
        if (!source_select_button_) return;
        set_text(source_select_button_, service_.install_source() == dsh::InstallSource::mirror ? L"国内镜像" : L"官方源");
    }

    // Dropdowns are page-local overlays. Always remove their child windows
    // before a page/state transition so they can never leak over another page.
    void hide_update_menus() {
        source_menu_visible_ = false;
        update_action_menu_visible_ = false;
        ShowWindow(source_mirror_button_, SW_HIDE);
        ShowWindow(source_official_button_, SW_HIDE);
        ShowWindow(update_all_button_, SW_HIDE);
        ShowWindow(update_dsh_button_, SW_HIDE);
        ShowWindow(update_launcher_button_, SW_HIDE);
        if (source_menu_button_) set_text(source_menu_button_, L"⌄");
        if (update_action_menu_button_) set_text(update_action_menu_button_, L"⌄");
    }

    void toggle_source_menu() {
        if (update_action_menu_visible_) toggle_update_action_menu();
        source_menu_visible_ = !source_menu_visible_;
        ShowWindow(source_mirror_button_, source_menu_visible_ ? SW_SHOW : SW_HIDE);
        ShowWindow(source_official_button_, source_menu_visible_ ? SW_SHOW : SW_HIDE);
        set_text(source_menu_button_, source_menu_visible_ ? L"⌃" : L"⌄");
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void select_source(dsh::InstallSource next) {
        if (next != service_.install_source()) {
            std::string error;
            if (!service_.set_install_source(next, error)) {
                show_error(error);
                return;
            }
            append_action(next == dsh::InstallSource::mirror ? L"下载源已切换为国内镜像"
                                                             : L"下载源已切换为官方源");
        }
        if (source_menu_visible_) toggle_source_menu();
        update_source_selector();
    }

    void show_settings_page(SettingsSection section) {
        page_ = Page::settings;
        resize_window(570);
        settings_section_ = section;
        set_text(page_title_, L"设置");
        set_text(page_subtitle_, L"日志、诊断与卸载维护");
        SetWindowPos(page_title_, nullptr, scale(86), scale(20), scale(260), scale(38), SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(page_subtitle_, nullptr, scale(88), scale(58), scale(300), scale(22), SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(settings_button_, SW_HIDE);
        ShowWindow(back_button_, SW_SHOW);
        for (auto control : {status_label_, detail_label_, progress_, activity_heading_, action_list_, footer_label_,
                             primary_button_, stop_button_, check_updates_button_, update_info_heading_, update_versions_label_,
                             update_notes_button_, update_notes_list_, source_select_button_, source_menu_button_, source_mirror_button_, source_official_button_, update_action_button_,
                             update_action_menu_button_, update_all_button_, update_dsh_button_, update_launcher_button_,
                             update_cancel_button_}) {
            ShowWindow(control, SW_HIDE);
        }
        hide_update_menus();
        for (auto control : {settings_nav_general_, settings_nav_logs_, settings_nav_maintenance_, settings_nav_about_,
                             settings_content_title_, settings_content_detail_}) {
            ShowWindow(control, SW_SHOW);
        }
        ShowWindow(settings_info_list_, SW_HIDE);
        ShowWindow(settings_hint_, SW_HIDE);
        ShowWindow(settings_tray_button_, SW_HIDE);
        ShowWindow(settings_close_button_, SW_HIDE);
        ShowWindow(open_logs_button_, SW_HIDE);
        ShowWindow(uninstall_dsh_button_, SW_HIDE);
        ShowWindow(uninstall_node_button_, SW_HIDE);
        ShowWindow(uninstall_all_button_, SW_HIDE);
        for (auto control : {settings_nav_general_, settings_nav_logs_, settings_nav_maintenance_, settings_nav_about_}) {
            EnableWindow(control, TRUE);
        }
        if (section == SettingsSection::general) {
            set_text(settings_content_title_, L"通用");
            set_text(settings_content_detail_, L"设置启动完成后的倒计时行为，\n以及点击右上角 × 时如何处理。");
            update_tray_setting_button();
            update_close_setting_button();
            ShowWindow(settings_tray_button_, SW_SHOW);
            ShowWindow(settings_close_button_, SW_SHOW);
            set_text(settings_hint_, L"单击上方按钮即可切换设置。托盘菜单可恢复窗口、打开网页、停止服务或退出。");
            SetWindowPos(settings_hint_, nullptr, scale(230), scale(342), scale(286), scale(46), SWP_NOZORDER | SWP_NOACTIVATE);
            ShowWindow(settings_hint_, SW_SHOW);
        } else if (section == SettingsSection::logs) {
            set_text(settings_content_title_, L"日志与诊断");
            set_text(settings_content_detail_, L"完整日志保存在本机应用数据目录。\n主日志不可写入时使用会话日志（带进程号）。");
            SendMessageW(settings_info_list_, LB_RESETCONTENT, 0, 0);
            const auto add_info = [this](const std::wstring& row) {
                const auto padded = L"  " + row;
                SendMessageW(settings_info_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(padded.c_str()));
            };
            add_info(L"启动器日志：" + service_.log_path().wstring());
            add_info(L"会话日志：" + service_.fallback_log_path().wstring());
            add_info(L"DSH 服务日志：" + service_.service_log_path().wstring());
            ShowWindow(settings_info_list_, SW_SHOW);
            // The info list occupies y 220-296; place the button below it so
            // the two controls never overlap.
            SetWindowPos(open_logs_button_, nullptr, scale(230), scale(308), scale(220), scale(38),
                         SWP_NOZORDER | SWP_NOACTIVATE);
            ShowWindow(open_logs_button_, SW_SHOW);
        } else if (section == SettingsSection::maintenance) {
            set_text(settings_content_title_, L"安装与管理");
            set_text(settings_content_detail_, L"仅处理启动器记录的目录与运行环境。\n配置、会话、DSH_HOME 与日志默认保留。");
            SendMessageW(settings_info_list_, LB_RESETCONTENT, 0, 0);
            const auto add_info = [this](const std::wstring& row) {
                const auto padded = L"  " + row;
                SendMessageW(settings_info_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(padded.c_str()));
            };
            add_info(L"DSH 安装：" + utf8_to_wide(last_status_.executable.empty() ? "未检测到" : last_status_.executable));
            add_info(L"Node.js：" + utf8_to_wide(node_version_.empty() ? "未检测到" : node_version_));
            add_info(L"npm：" + utf8_to_wide(npm_version_.empty() ? "未检测到" : npm_version_));
            ShowWindow(settings_info_list_, SW_SHOW);
            ShowWindow(uninstall_dsh_button_, SW_SHOW);
            ShowWindow(uninstall_node_button_, SW_HIDE);
            ShowWindow(uninstall_all_button_, SW_SHOW);
            const bool can_manage = !busy_.load() && action_mode_ == ActionMode::normal;
            const bool launcher_owned_node = service_.launcher_owned_node_installed();
            set_text(settings_hint_, launcher_owned_node
                                          ? L"第二项会一并移除启动器安装的 Node.js。"
                                          : L"当前 Node.js 并非由启动器安装，第二项为保护系统而不可用。");
            // The general page moves this shared hint upward. Restore the
            // maintenance layout before showing it so wrapped text cannot
            // cover the second uninstall button.
            SetWindowPos(settings_hint_, nullptr, scale(230), scale(404), scale(286), scale(44), SWP_NOZORDER | SWP_NOACTIVATE);
            ShowWindow(settings_hint_, SW_SHOW);
            EnableWindow(uninstall_dsh_button_, can_manage && last_status_.installed);
            EnableWindow(uninstall_all_button_, can_manage && last_status_.installed &&
                                                  launcher_owned_node);
        } else {
            set_text(settings_content_title_, L"关于 DSH Launcher");
            set_text(settings_content_detail_, L"版本与运行环境信息");
            SendMessageW(settings_info_list_, LB_RESETCONTENT, 0, 0);
            const auto add_info = [this](const std::wstring& row) {
                const auto padded = L"  " + row;
                SendMessageW(settings_info_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(padded.c_str()));
            };
            add_info(L"启动器版本：" + std::wstring(launcher_version));
            add_info(L"DSH 当前版本：" + utf8_to_wide(last_status_.version.empty() ? "未安装" : last_status_.version));
            add_info(L"DSH 最新版本：" + utf8_to_wide(latest_version_.empty() ? "尚未检查" : latest_version_));
            add_info(L"启动器最新版本：" + utf8_to_wide(latest_launcher_version_.empty() ? "尚未检查" : latest_launcher_version_));
            add_info(L"Node.js：" + utf8_to_wide(node_version_.empty() ? "未检测到" : node_version_));
            add_info(L"npm：" + utf8_to_wide(npm_version_.empty() ? "未检测到" : npm_version_));
            ShowWindow(settings_info_list_, SW_SHOW);
        }
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

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

    void update_tray_setting_button() {
        set_text(settings_tray_button_, service_.minimize_to_tray()
                                            ? L"已开启：30 秒后最小化到托盘（单击切换）"
                                            : L"已关闭：30 秒后自动退出（单击切换）");
    }

    void toggle_tray_setting() {
        const bool enabled = !service_.minimize_to_tray();
        std::string error;
        if (!service_.set_minimize_to_tray(enabled, error)) {
            show_error(error);
            return;
        }
        update_tray_setting_button();
        append_action(enabled ? L"已开启启动后自动最小化到托盘"
                              : L"已关闭启动后自动最小化到托盘");
    }

    void update_close_setting_button() {
        const auto action = service_.close_action();
        set_text(settings_close_button_, action == dsh::CloseAction::tray
                                             ? L"关闭按钮：最小化到托盘（单击切换）"
                                             : action == dsh::CloseAction::exit
                                                   ? L"关闭按钮：直接退出启动器（单击切换）"
                                                   : L"关闭按钮：每次询问（点击切换）");
    }

    void cycle_close_setting() {
        const auto current = service_.close_action();
        const auto next = current == dsh::CloseAction::ask ? dsh::CloseAction::tray
                         : current == dsh::CloseAction::tray ? dsh::CloseAction::exit
                                                            : dsh::CloseAction::ask;
        std::string error;
        if (!service_.set_close_action(next, error)) {
            show_error(error);
            return;
        }
        update_close_setting_button();
        append_action(next == dsh::CloseAction::tray ? L"点击 × 将最小化到托盘"
                      : next == dsh::CloseAction::exit ? L"点击 × 将直接退出启动器"
                                                      : L"点击 × 时将每次询问");
    }

    bool ensure_tray_icon() {
        if (tray_icon_visible_) return true;
        tray_icon_ = {};
        tray_icon_.cbSize = sizeof(tray_icon_);
        tray_icon_.hWnd = hwnd_;
        tray_icon_.uID = 1;
        tray_icon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        tray_icon_.uCallbackMessage = message_tray;
        tray_icon_.hIcon = small_icon_;
        wcscpy_s(tray_icon_.szTip, L"DSH Launcher");
        if (!Shell_NotifyIconW(NIM_ADD, &tray_icon_)) return false;
        tray_icon_visible_ = true;
        tray_icon_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &tray_icon_);
        return true;
    }

    void remove_tray_icon() {
        if (!tray_icon_visible_) return;
        Shell_NotifyIconW(NIM_DELETE, &tray_icon_);
        tray_icon_visible_ = false;
    }

    void minimize_to_tray() {
        if (!ensure_tray_icon()) {
            show_notice(L"无法进入系统托盘", L"系统未能创建托盘图标，启动器将保持打开");
            return;
        }
        ShowWindow(hwnd_, SW_HIDE);
    }

    void restore_from_tray() {
        ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
        remove_tray_icon();
    }

    void show_tray_menu() {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING, id_tray_restore, L"恢复 DSH Launcher");
        AppendMenuW(menu, MF_STRING, id_tray_open_web, L"打开 DSH 网页");
        AppendMenuW(menu, MF_STRING | ((!last_status_.running || busy_.load()) ? MF_GRAYED : 0),
                    id_tray_stop, L"停止 DSH 服务");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, id_tray_exit, L"退出启动器");
        POINT point{};
        GetCursorPos(&point);
        SetForegroundWindow(hwnd_);
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                            point.x, point.y, 0, hwnd_, nullptr);
        DestroyMenu(menu);
        if (command == id_tray_restore) restore_from_tray();
        else if (command == id_tray_open_web) { restore_from_tray(); open_web(); }
        else if (command == id_tray_stop) { restore_from_tray(); stop(); }
        else if (command == id_tray_exit) {
            if (busy_.load()) {
                restore_from_tray();
                show_notice(L"操作正在进行", L"请等待完成，或使用红色取消按钮");
            } else DestroyWindow(hwnd_);
        }
    }

    void request_uninstall(UninstallTarget target) {
        if (target == UninstallTarget::none) return;
        pending_uninstall_ = target;
        choice_return_page_ = page_;
        choice_return_settings_section_ = settings_section_;
        choice_visible_ = true;
        choice_mode_ = ChoiceMode::uninstall;
        const auto title = target == UninstallTarget::both ? L"确认卸载 DSH 与 Node.js" : L"确认卸载 DSH";
        const auto detail = target == UninstallTarget::both
                                ? L"将删除当前记录的 DSH，以及启动器托管的 Node.js。\n取消勾选将清除 DSH 的会话与本地存储；设置、凭据和日志始终保留。"
                                : L"将删除当前记录的 DSH 程序与依赖。\n取消勾选将清除 DSH 的会话与本地存储；设置、凭据和日志始终保留。";
        const auto primary = target == UninstallTarget::both ? L"确认卸载" : L"确认卸载 DSH";
        if (!ChoiceDialog::show(instance_, hwnd_, dpi_, title, detail, primary, L"取消", true, true)) {
            choice_visible_ = false;
            choice_mode_ = ChoiceMode::none;
            pending_uninstall_ = UninstallTarget::none;
            show_error("无法显示卸载确认窗口。");
            return;
        }
        append_action(L"等待确认卸载操作");
    }

    void cancel_uninstall() {
        pending_uninstall_ = UninstallTarget::none;
        action_mode_ = ActionMode::normal;
        append_action(L"已取消卸载");
        show_settings_page(SettingsSection::maintenance);
    }

    void start_uninstall(UninstallTarget target, bool preserve_memory) {
        const bool remove_dsh = target == UninstallTarget::dsh || target == UninstallTarget::both;
        const bool remove_node = target == UninstallTarget::node || target == UninstallTarget::both;
        pending_uninstall_ = UninstallTarget::none;
        action_mode_ = ActionMode::normal;
        append_action(remove_dsh ? (remove_node ? L"开始卸载 DSH 和托管 Node.js" : L"开始卸载 DSH")
                                 : L"开始卸载托管 Node.js");
        if (remove_dsh) append_action(preserve_memory ? L"将保留 DSH 对话记忆" : L"将清除 DSH 对话记忆");
        set_busy(L"正在卸载…", preserve_memory
                                      ? L"将保留对话记忆、设置、凭据与日志"
                                      : L"将清除对话记忆；设置、凭据与日志仍保留");
        background([this, remove_dsh, remove_node, preserve_memory] {
            std::string error;
            const bool success = service_.uninstall(
                remove_dsh, remove_node, preserve_memory,
                [this](const std::string& step) { post([this, step] { append_action(utf8_to_wide(step)); }); },
                error);
            if (!success) {
                post([this, error] { show_error(error); });
                return;
            }
            post([this] {
                last_status_ = service_.detect();
                const auto environment = service_.environment();
                node_version_ = environment.node_version;
                npm_version_ = environment.npm_version;
                append_action(L"卸载完成");
                show_home(last_status_);
            });
        });
    }

    void open_logs() {
        // Open the directory that actually holds this process's log: the
        // session log under %TEMP% when the primary %LOCALAPPDATA% log could
        // not be appended (e.g. redirected or locked profile).
        const auto directory = service_.used_log_path().parent_path();
        const auto result = reinterpret_cast<std::intptr_t>(
            ShellExecuteW(hwnd_, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) show_error("无法打开日志目录。");
        else append_action(L"已打开日志目录");
    }

    void refresh_runtime_details() {
        if (!action_list_) return;
        if (SendMessageW(action_list_, LB_GETCOUNT, 0, 0) == 0) {
            const auto initial = last_status_.running ? L"当前 DSH 服务正在运行" : L"当前没有运行中的后台任务";
            append_action(initial);
        }
        HDC dc = GetDC(action_list_);
        const auto old_font = SelectObject(dc, small_font_);
        int extent = scale(440);
        const int count = static_cast<int>(SendMessageW(action_list_, LB_GETCOUNT, 0, 0));
        for (int index = 0; index < count; ++index) {
            const int length = static_cast<int>(SendMessageW(action_list_, LB_GETTEXTLEN, index, 0));
            std::wstring row(static_cast<std::size_t>(length) + 1, L'\0');
            SendMessageW(action_list_, LB_GETTEXT, index, reinterpret_cast<LPARAM>(row.data()));
            SIZE size{};
            GetTextExtentPoint32W(dc, row.c_str(), length, &size);
            extent = (std::max)(extent, static_cast<int>(size.cx) + scale(24));
        }
        SelectObject(dc, old_font);
        ReleaseDC(action_list_, dc);
        SendMessageW(action_list_, LB_SETHORIZONTALEXTENT, extent, 0);
    }

    int scale(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }

    void resize_window(int height) {
        SetWindowPos(hwnd_, nullptr, 0, 0, scale(collapsed_width), scale(height),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void append_action(const std::wstring& action) {
        if (action.empty()) return;
        const auto row = L"   • " + action;
        if (SendMessageW(action_list_, LB_GETCOUNT, 0, 0) >= 100) {
            SendMessageW(action_list_, LB_DELETESTRING, 0, 0);
        }
        SendMessageW(action_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.c_str()));
        const auto count = SendMessageW(action_list_, LB_GETCOUNT, 0, 0);
        if (count > 0) SendMessageW(action_list_, LB_SETTOPINDEX, count - 1, 0);
        HDC dc = GetDC(action_list_);
        const auto old_font = SelectObject(dc, normal_font_);
        SIZE size{};
        GetTextExtentPoint32W(dc, row.c_str(), static_cast<int>(row.size()), &size);
        SelectObject(dc, old_font);
        ReleaseDC(action_list_, dc);
        const auto previous_extent = static_cast<int>(SendMessageW(action_list_, LB_GETHORIZONTALEXTENT, 0, 0));
        SendMessageW(action_list_, LB_SETHORIZONTALEXTENT,
                     (std::max)(previous_extent, static_cast<int>(size.cx) + scale(24)), 0);
    }

    void set_text(HWND control, const std::wstring& text) { SetWindowTextW(control, text.c_str()); }
    void background(std::function<void()> action) {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        workers_.emplace_back([this, action = std::move(action)] {
            if (alive_.load()) action();
        });
    }
    void post(std::function<void()> action) {
        if (!alive_.load()) return;
        auto* task = new PostedTask{std::move(action)};
        if (!PostMessageW(hwnd_, message_task, 0, reinterpret_cast<LPARAM>(task))) delete task;
    }

    HINSTANCE instance_{};
    HWND hwnd_{};
    HWND page_title_{};
    HWND page_subtitle_{};
    HWND version_label_{};
    HWND settings_button_{};
    HWND back_button_{};
    HWND status_label_{};
    HWND detail_label_{};
    HWND action_list_{};
    HWND activity_heading_{};
    HWND footer_label_{};
    HWND progress_{};
    HWND primary_button_{};
    HWND stop_button_{};
    HWND open_logs_button_{};
    HWND check_updates_button_{};
    HWND source_select_button_{};
    HWND source_menu_button_{};
    HWND source_mirror_button_{};
    HWND source_official_button_{};
    HWND update_info_heading_{};
    HWND update_versions_label_{};
    HWND update_notes_button_{};
    HWND update_notes_list_{};
    HWND update_action_button_{};
    HWND update_action_menu_button_{};
    HWND update_all_button_{};
    HWND update_launcher_button_{};
    HWND update_dsh_button_{};
    HWND update_cancel_button_{};
    HWND settings_nav_general_{};
    HWND settings_nav_logs_{};
    HWND settings_nav_maintenance_{};
    HWND settings_nav_about_{};
    HWND settings_content_title_{};
    HWND settings_content_detail_{};
    HWND settings_tray_button_{};
    HWND settings_close_button_{};
    HWND settings_info_list_{};
    HWND settings_hint_{};
    HWND uninstall_dsh_button_{};
    HWND uninstall_node_button_{};
    HWND uninstall_all_button_{};
    HFONT title_font_{};
    HFONT status_font_{};
    HFONT section_font_{};
    HFONT normal_font_{};
    HFONT small_font_{};
    HFONT settings_icon_font_{};
    HBRUSH background_brush_{};
    HBRUSH surface_brush_{};
    HBRUSH class_brush_{};
    HICON small_icon_{};
    NOTIFYICONDATAW tray_icon_{};
    bool tray_icon_visible_{};
    bool class_registered_{};
    UINT dpi_{96};
    COLORREF status_color_{primary_color};
    dsh::Service service_;
    dsh::Status last_status_;
    dsh::EnvironmentStatus initial_environment_;
    std::filesystem::path pending_dsh_directory_;
    std::filesystem::path pending_node_directory_;
    dsh::InstallSource pending_node_source_{dsh::InstallSource::mirror};
    std::string node_version_;
    std::string npm_version_;
    std::string latest_version_;
    std::string latest_launcher_version_;
    std::optional<dsh::platform::LauncherUpdate> launcher_update_;
    bool dsh_update_available_{};
    std::atomic_bool alive_{true};
    std::atomic_bool busy_{false};
    std::atomic_bool cancellable_{false};
    std::atomic_bool cancel_requested_{false};
    int progress_position_{};
    std::mutex workers_mutex_;
    std::vector<std::thread> workers_;
    bool running_{};
    ActionMode action_mode_{ActionMode::normal};
    UninstallTarget pending_uninstall_{UninstallTarget::none};
    Page page_{Page::home};
    SettingsSection settings_section_{SettingsSection::general};
    UpdateAction selected_update_action_{UpdateAction::all};
    bool update_notes_visible_{};
    bool update_action_menu_visible_{};
    bool source_menu_visible_{};
    bool choice_visible_{};
    ChoiceMode choice_mode_{ChoiceMode::none};
    Page choice_return_page_{Page::home};
    SettingsSection choice_return_settings_section_{SettingsSection::general};
    bool close_busy_notice_shown_{};
    bool update_launcher_after_dsh_{};
    bool dsh_version_checked_{};
    bool launcher_version_checked_{};
    bool initial_launch_{};
    bool initial_service_ready_{};
    bool initial_update_finished_{};
    bool initial_update_available_{};
    bool initial_update_check_failed_{};
    std::string initial_dsh_choice_;
    std::optional<dsh::platform::LauncherUpdate> initial_launcher_choice_;
    ULONGLONG operation_started_{};
};

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show_command) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // A scheduled task and a Startup-folder shortcut can otherwise launch two
    // copies at login; each copy would open the DSH page independently.
    const auto single_instance = CreateMutexW(nullptr, TRUE, L"Local\\DshLauncher.SingleInstance");
    if (!single_instance || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (single_instance) CloseHandle(single_instance);
        return 0;
    }
    const auto com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    LauncherWindow window(instance);
    if (!window.create()) {
        if (SUCCEEDED(com_result)) CoUninitialize();
        return 1;
    }
    const auto result = window.run(show_command);
    if (SUCCEEDED(com_result)) CoUninitialize();
    CloseHandle(single_instance);
    return result;
}

#endif
