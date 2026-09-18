/*
 * Xournal++
 *
 * The main application
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2
 */

#include <array>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>

#include <gio/gio.h>
#include <gtk/gtk.h>

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <shellapi.h>
// clang-format on
#endif

#include "util/PathUtil.h"
#include "util/VersionInfo.h"
#include "util/XojMsgBox.h"
#include "util/glib_casts.h"
#include "util/gtk4_helper.h"
#include "util/raii/CStringWrapper.h"
#include "util/raii/GObjectSPtr.h"

#include "config.h"
#include "filesystem.h"

static constexpr const char* UI_RESOURCE = "/org/xournalpp/wrapper/ui/crashDialog.glade";

// See CrashHandler::emergencySave()
static constexpr const char* EMERGENCY_SAVE_MSG_REGEX = "Successfully saved document to \"(.*)\"";

static void activate(GtkApplication* app, std::string* str) {
    xoj::util::GObjectSPtr<GtkBuilder> builder(gtk_builder_new_from_resource(UI_RESOURCE), xoj::util::adopt);
    auto* window = GTK_WIDGET(gtk_builder_get_object(builder.get(), "crashDialog"));
    gtk_application_add_window(app, GTK_WINDOW(window));
    auto* view = GTK_TEXT_VIEW(gtk_builder_get_object(builder.get(), "textViewLog"));
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(view), str->data(), -1);

    auto* entry = GTK_ENTRY(gtk_builder_get_object(builder.get(), "entryPathEmergencySave"));
    std::smatch res;
    std::regex_search(*str, res, std::regex(EMERGENCY_SAVE_MSG_REGEX));
    if (res.empty()) {
        gtk_editable_set_text(GTK_EDITABLE(entry), "FAILED");
        gtk_widget_add_css_class(GTK_WIDGET(entry), "error");
    } else {
        gtk_editable_set_text(GTK_EDITABLE(entry), res[1].str().c_str());
    }

    g_signal_connect_object(gtk_builder_get_object(builder.get(), "closeBtn"), "clicked",
                            G_CALLBACK(+[](GtkButton* btn, gpointer w) { gtk_window_close(GTK_WINDOW(w)); }), window,
                            GConnectFlags(0));
    gtk_window_present(GTK_WINDOW(window));
}

#ifdef __APPLE__
static void configurePrivateRuntimeDirectories(GSubprocessLauncher* launcher) {
    const auto root = fs::path(g_get_home_dir()) / "Library" / "Application Support" / "StudySzn Marker";
    const std::array<std::pair<const char*, fs::path>, 4> directories = {
            std::pair{"XDG_CONFIG_HOME", root / "config"},
            std::pair{"XDG_CACHE_HOME", root / "cache"},
            std::pair{"XDG_STATE_HOME", root / "state"},
            std::pair{"XDG_DATA_HOME", root / "data"},
    };
    for (const auto& [variable, directory]: directories) {
        const auto value = directory.string();
        g_mkdir_with_parents(value.c_str(), 0700);
        g_subprocess_launcher_setenv(launcher, variable, value.c_str(), true);
    }

    const auto resources = Util::getExePath().parent_path() / "Resources";
    const auto bundledData = (resources / "share").string();
    const char* existingData = g_getenv("XDG_DATA_DIRS");
    const std::string dataDirectories =
            existingData && *existingData ? bundledData + ":" + existingData : bundledData;
    g_subprocess_launcher_setenv(launcher, "XDG_DATA_DIRS", dataDirectories.c_str(), true);
    const auto loaderCache = (resources / "lib" / "gdk-pixbuf-2.0" / "2.10.0" / "loaders.cache").string();
    g_subprocess_launcher_setenv(launcher, "GDK_PIXBUF_MODULE_FILE", loaderCache.c_str(), true);
}
#endif

auto main(int argc, char* argv[]) -> int {
    GError* err = nullptr;
    std::stringstream errorlog;
    errorlog.imbue(std::locale::classic());

    auto p = [&]() {
        static constexpr auto FLAGS = GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_PIPE
#ifdef _WIN32  // On Windows, STDERR_MERGE does not work. See https://gitlab.gnome.org/GNOME/glib/-/issues/3723
                                                       | G_SUBPROCESS_FLAGS_STDERR_PIPE
#else
                                                       | G_SUBPROCESS_FLAGS_STDERR_MERGE
#endif
        );

        std::vector<const char*> subargv;
        std::cout << Util::getExePath() << std::endl;

        const std::u8string path = (Util::getExePath() / "studyszn-marker").u8string();
        subargv.emplace_back(char_cast(path.c_str()));  // Data is owned by `path` - Do not delete it
        errorlog << "Executing \"" << char_cast(path);

#ifdef _WIN32
        std::vector<std::string> utf8_args;

        LPWSTR* szArglist;
        int nArgs;
        szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);

        if (szArglist != nullptr) {
            // Start from 1 to skip program name
            for (int i = 1; i < nArgs; i++) {
                // Calculate required buffer size for UTF-8
                int size_needed = WideCharToMultiByte(CP_UTF8, 0, szArglist[i], -1, nullptr, 0, nullptr, nullptr);
                if (size_needed > 0) {
                    std::string utf8_arg(size_needed - 1, 0);  // -1 to exclude null terminator
                    WideCharToMultiByte(CP_UTF8, 0, szArglist[i], -1, &utf8_arg[0], size_needed, nullptr, nullptr);
                    utf8_args.push_back(std::move(utf8_arg));
                }
            }
            LocalFree(szArglist);

            // Add UTF-8 encoded arguments
            for (const auto& arg: utf8_args) {
                subargv.emplace_back(arg.c_str());
                errorlog << " [document]";
            }
        } else {
            // Fallback to original argv if CommandLineToArgvW fails
            for (int i = 1; i < argc; i++) {
                subargv.emplace_back(argv[i]);
                errorlog << " [document]";
            }
        }
#else
        for (int i = 1; i < argc; i++) {
            subargv.emplace_back(argv[i]);
            errorlog << " [document]";
        }
#endif
        subargv.emplace_back(nullptr);
        errorlog << "\"" << std::endl;

        xoj::util::GObjectSPtr<GSubprocessLauncher> launcher(g_subprocess_launcher_new(FLAGS), xoj::util::adopt);
        g_subprocess_launcher_set_environ(launcher.get(), nullptr);  // Copies the host's environment
        g_subprocess_launcher_setenv(launcher.get(), "G_MESSAGES_DEBUG", G_LOG_DOMAIN, false);  // Print xopp debug
#ifdef __APPLE__
        configurePrivateRuntimeDirectories(launcher.get());
#endif

        return xoj::util::GObjectSPtr<GSubprocess>(g_subprocess_launcher_spawnv(launcher.get(), subargv.data(), &err),
                                                   xoj::util::adopt);
    }();

    int exitstatus = -1;

    if (err) {
        printf("Failed to start\n");
        errorlog << "Failed to start: " << err->message << std::endl;
        g_error_free(err);
        err = nullptr;
    } else {
        xoj::util::OwnedCString stdoutBuffer;
#ifdef _WIN32  // On Windows, STDERR_MERGE does not work. See https://gitlab.gnome.org/GNOME/glib/-/issues/3723
        xoj::util::OwnedCString stderrBuffer;
#endif

        std::cout << "StudySzn Marker started with PID: " << g_subprocess_get_identifier(p.get()) << std::endl;
        errorlog << "StudySzn Marker started with PID: " << g_subprocess_get_identifier(p.get()) << std::endl;

        g_subprocess_communicate_utf8(p.get(), nullptr, nullptr, stdoutBuffer.contentReplacer(),
#ifdef _WIN32  // On Windows, STDERR_MERGE does not work. See https://gitlab.gnome.org/GNOME/glib/-/issues/3723
                                      stderrBuffer.contentReplacer(),
#else
                                      nullptr,
#endif
                                      &err);

        if (g_subprocess_get_if_exited(p.get())) {
            exitstatus = g_subprocess_get_exit_status(p.get());
            if (exitstatus == 0) [[likely]] {
                std::cout << "Execution completed normally" << std::endl;
                return 0;
            } else {
                std::cout << "Exited with error status: " << exitstatus << std::endl;
                errorlog << "Exited with error status: " << exitstatus << std::endl;
            }
        }
        if (g_subprocess_get_if_signaled(p.get())) {
            auto sig = g_subprocess_get_term_sig(p.get());
            std::cout << "Crashed with signal: " << sig << std::endl;
            errorlog << "Crashed with signal: " << sig << std::endl;
        }

        if (err) {
            errorlog << "    Error: " << err->message << std::endl;
            g_error_free(err);
            err = nullptr;
        }

        time_t lt = time(nullptr);
        errorlog << "    Date: " << ctime(&lt) << std::endl;

        errorlog << xoj::util::getVersionInfo();
        errorlog << "  (The GDK backend is probably printed out below)\n";

        const std::string childOutput = stdoutBuffer.get() ? stdoutBuffer.get() : "";
        std::smatch emergencySave;
        if (std::regex_search(childOutput, emergencySave, std::regex(EMERGENCY_SAVE_MSG_REGEX))) {
            errorlog << emergencySave[0].str() << "\n";
        }
        errorlog << "\nApplication output was omitted to protect student-document privacy.\n";
    }

    g_set_prgname("com.studyszn.marker.wrapper");
    g_set_application_name("StudySzn Marker");
    GtkApplication* app = gtk_application_new("com.studyszn.marker.wrapper", G_APPLICATION_NON_UNIQUE);
    g_signal_connect_data(app, "activate", xoj::util::wrap_for_g_callback_v<activate>, new std::string(errorlog.str()),
                          xoj::util::closure_notify_cb<std::string>, GConnectFlags(0));

    g_application_run(G_APPLICATION(app), 0, nullptr);
    g_object_unref(app);

    return exitstatus;
}
