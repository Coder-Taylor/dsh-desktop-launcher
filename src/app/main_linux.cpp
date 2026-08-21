#ifndef _WIN32

#include <gtk/gtk.h>

#include "core/service.h"

#include <memory>
#include <string>
#include <thread>

namespace {
struct App {
    dsh::Service service;
    GtkWidget* window{};
    GtkWidget* status{};
    GtkWidget* version{};
    GtkWidget* path{};
};

void refresh(App* app) {
    std::thread([app] {
        const auto state = app->service.detect();
        g_idle_add([](gpointer data) -> gboolean {
            auto* pair = static_cast<std::pair<App*, dsh::Status>*>(data);
            gtk_label_set_text(GTK_LABEL(pair->first->status), pair->second.running ? "● 正在运行" : (pair->second.installed ? "○ 已停止" : "未安装 DSH"));
            gtk_label_set_text(GTK_LABEL(pair->first->version), pair->second.version.c_str());
            gtk_label_set_text(GTK_LABEL(pair->first->path), pair->second.executable.c_str());
            delete pair;
            return G_SOURCE_REMOVE;
        }, new std::pair<App*, dsh::Status>(app, state));
    }).detach();
}
}  // namespace

int main(int argc, char** argv) {
    gtk_init(&argc, &argv);
    App app;
    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "DSH Launcher");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 640, 380);
    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_container_set_border_width(GTK_CONTAINER(box), 24);
    gtk_container_add(GTK_CONTAINER(app.window), box);
    auto* title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(title), "<span size='xx-large' weight='bold'>DeepSeek Harness</span>");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);
    app.status = gtk_label_new("正在读取本地状态…");
    app.version = gtk_label_new("—");
    app.path = gtk_label_new("—");
    for (auto* widget : {app.status, app.version, app.path}) {
        gtk_widget_set_halign(widget, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0);
    }
    auto* buttons = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    auto* start = gtk_button_new_with_label("启动 DSH");
    auto* open = gtk_button_new_with_label("打开网页");
    auto* stop = gtk_button_new_with_label("停止 DSH");
    auto* update = gtk_button_new_with_label("刷新状态");
    for (auto* button : {start, open, stop, update}) gtk_container_add(GTK_CONTAINER(buttons), button);
    gtk_box_pack_end(GTK_BOX(box), buttons, FALSE, FALSE, 0);
    g_signal_connect_swapped(update, "clicked", G_CALLBACK(+[](App* value) { refresh(value); }), &app);
    g_signal_connect_swapped(start, "clicked", G_CALLBACK(+[](App* value) { std::string error; value->service.start(error); refresh(value); }), &app);
    g_signal_connect_swapped(stop, "clicked", G_CALLBACK(+[](App* value) { std::string error; value->service.stop(error); refresh(value); }), &app);
    g_signal_connect_swapped(open, "clicked", G_CALLBACK(+[](App* value) { std::string error; value->service.open_web(error); }), &app);
    gtk_widget_show_all(app.window);
    refresh(&app);
    gtk_main();
    return 0;
}

#endif

