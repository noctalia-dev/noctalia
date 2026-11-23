#pragma once

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <gio/gdesktopappinfo.h>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace filemanager::external_tools {

  enum class Tool { Terminal, DiskUsage, Compare };

  using App = std::shared_ptr<GDesktopAppInfo>;

  inline App takeApp(GDesktopAppInfo* owned) { return owned ? App(owned, g_object_unref) : App{}; }

  struct Choice {
    App app;
    std::string desktopAction;
  };

  struct Applications {
    std::vector<Choice> choices;
    App preferred;
  };

  inline constexpr auto group = G_KEY_FILE_DESKTOP_GROUP;

  inline std::vector<std::filesystem::path>
  desktopConfigPaths(std::span<const std::filesystem::path> roots, const char* filename) {
    const char* currentDesktop = g_getenv("XDG_CURRENT_DESKTOP");
    const std::unique_ptr<char*, decltype(&g_strfreev)> desktops(
        g_strsplit(currentDesktop ? currentDesktop : "", ":", -1), g_strfreev
    );
    std::vector<std::filesystem::path> paths;
    for (const auto& root : roots) {
      for (auto desktop = desktops.get(); *desktop; ++desktop) {
        const std::unique_ptr<char, decltype(&g_free)> lower(g_ascii_strdown(*desktop, -1), g_free);
        paths.push_back(root / (std::string(lower.get()) + "-" + filename));
      }
      paths.push_back(root / filename);
    }
    return paths;
  }

  inline std::vector<std::filesystem::path> configRoots() {
    std::vector<std::filesystem::path> roots{g_get_user_config_dir()};
    for (auto dir = g_get_system_config_dirs(); *dir; ++dir)
      roots.emplace_back(*dir);
    return roots;
  }

  struct TerminalPreferences {
    std::vector<std::string> candidates;
    std::unordered_map<std::string, bool> excluded;
  };

  inline TerminalPreferences terminalPreferences() {
    auto roots = configRoots();
    for (auto dir = g_get_system_data_dirs(); *dir; ++dir)
      roots.push_back(std::filesystem::path(*dir) / "xdg-terminal-exec");
    TerminalPreferences result;
    for (const auto& path : desktopConfigPaths(roots, "xdg-terminals.list")) {
      std::ifstream input(path);
      std::string line;
      while (std::getline(input, line)) {
        g_strstrip(line.data());
        line.resize(std::char_traits<char>::length(line.c_str()));
        if (line.empty() || line.front() == '#' || line.front() == '/')
          continue;
        if (line.front() == '+' || line.front() == '-') {
          const auto id = line.substr(1, std::string_view(line).substr(1).find(':'));
          result.excluded.try_emplace(id, line.front() == '-');
        } else {
          result.candidates.push_back(line);
        }
      }
    }
    return result;
  }

  inline bool hasCategory(GDesktopAppInfo* app, std::string_view category) {
    const char* categories = g_desktop_app_info_get_categories(app);
    return categories && (";" + std::string(categories) + ";").contains(";" + std::string(category) + ";");
  }

  inline bool execTakesInputs(std::string_view exec, bool multiple) {
    for (std::size_t i = 0; i + 1 < exec.size(); ++i) {
      if (exec[i] != '%')
        continue;
      const char code = exec[++i];
      if (code == 'F' || code == 'U' || (!multiple && (code == 'f' || code == 'u')))
        return true;
    }
    return false;
  }

  inline bool supports(GDesktopAppInfo* app, Tool tool) {
    if (g_desktop_app_info_get_is_hidden(app) || g_desktop_app_info_get_boolean(app, G_KEY_FILE_DESKTOP_KEY_TERMINAL))
      return false;
    const char* command = g_app_info_get_commandline(G_APP_INFO(app));
    if (!command || !*command)
      return false;
    if (tool == Tool::Terminal)
      return hasCategory(app, "TerminalEmulator");

    // XDG has no dedicated categories for these roles. Match desktop IDs with known
    // positional argument interfaces rather than guessing from localized names.
    constexpr std::array<std::string_view, 5> analysers{
        "org.kde.filelight.desktop", "filelight.desktop", "org.gnome.baobab.desktop", "baobab.desktop",
        "qdirstat.desktop",
    };
    constexpr std::array<std::string_view, 6> comparers{
        "org.gnome.Meld.desktop",  "meld.desktop",    "org.kde.kdiff3.desktop", "kdiff3.desktop",
        "org.kde.kompare.desktop", "kompare.desktop",
    };
    const char* id = g_app_info_get_id(G_APP_INFO(app));
    const std::span<const std::string_view> supported = tool == Tool::DiskUsage
        ? std::span<const std::string_view>(analysers)
        : std::span<const std::string_view>(comparers);
    return id
        && std::ranges::find(supported, id) != supported.end()
        && (tool != Tool::Compare || !execTakesInputs(command, false) || execTakesInputs(command, true));
  }

  inline bool hasExplicitMimeDefault(GAppInfo* app, const char* mime) {
    auto roots = configRoots();
    roots.push_back(std::filesystem::path(g_get_user_data_dir()) / "applications");
    for (auto dir = g_get_system_data_dirs(); *dir; ++dir)
      roots.push_back(std::filesystem::path(*dir) / "applications");
    for (const auto& path : desktopConfigPaths(roots, "mimeapps.list")) {
      const std::unique_ptr<GKeyFile, decltype(&g_key_file_unref)> file(g_key_file_new(), g_key_file_unref);
      if (!g_key_file_load_from_file(file.get(), path.c_str(), G_KEY_FILE_NONE, nullptr))
        continue;
      const std::array keys{mime, g_str_equal(mime, "text/x-patch") ? "text/x-diff" : nullptr};
      for (const char* key : keys) {
        if (!key)
          continue;
        const std::unique_ptr<char*, decltype(&g_strfreev)> ids(
            g_key_file_get_string_list(file.get(), "Default Applications", key, nullptr, nullptr), g_strfreev
        );
        for (auto id = ids.get(); id && *id; ++id)
          if (g_strcmp0(*id, g_app_info_get_id(app)) == 0)
            return true;
      }
    }
    return false;
  }

  inline bool hasDesktopAction(GDesktopAppInfo* app, const std::string& action) {
    for (auto id = g_desktop_app_info_list_actions(app); id && *id; ++id)
      if (action == *id)
        return true;
    return false;
  }

  inline std::array<Applications, 3> discover() {
    const auto terminal = terminalPreferences();
    std::array<Applications, 3> result;
    auto& terminals = result[0];
    for (const auto& candidate : terminal.candidates) {
      const auto colon = candidate.find(':');
      const auto id = candidate.substr(0, colon);
      auto app = takeApp(g_desktop_app_info_new(id.c_str()));
      if (!app || !supports(app.get(), Tool::Terminal))
        continue;
      const auto action = colon == std::string::npos ? std::string{} : candidate.substr(colon + 1);
      if (!action.empty() && !hasDesktopAction(app.get(), action))
        continue;
      terminals.preferred = app;
      terminals.choices.push_back({std::move(app), action});
      break;
    }

    // g_app_info_get_default_for_type can also pick an arbitrary registered
    // handler. Only treat an explicit MIME Apps association as a user preference.
    const std::array mimeTypes{"", "inode/directory", "text/x-patch"};
    for (std::size_t i = 1; i < result.size(); ++i) {
      auto* app = g_app_info_get_default_for_type(mimeTypes[i], FALSE);
      if (!app)
        continue;
      if (!G_IS_DESKTOP_APP_INFO(app)) {
        g_object_unref(app);
        continue;
      }
      App desktop(G_DESKTOP_APP_INFO(app), g_object_unref);
      if (supports(desktop.get(), static_cast<Tool>(i)) && hasExplicitMimeDefault(app, mimeTypes[i])) {
        result[i].preferred = desktop;
        result[i].choices.push_back({std::move(desktop), {}});
      }
    }

    GList* installed = g_app_info_get_all();
    for (auto item = installed; item; item = item->next) {
      auto* info = G_APP_INFO(item->data);
      if (!G_IS_DESKTOP_APP_INFO(info) || !g_app_info_should_show(info)) {
        g_object_unref(info);
        continue;
      }
      App app(G_DESKTOP_APP_INFO(info), g_object_unref);
      const char* id = g_app_info_get_id(info);
      for (std::size_t i = 0; id && i < result.size(); ++i) {
        auto& applications = result[i];
        if (applications.preferred && g_app_info_equal(info, G_APP_INFO(applications.preferred.get())))
          continue;
        if (i == 0 && terminal.excluded.contains(id) && terminal.excluded.at(id))
          continue;
        if (supports(app.get(), static_cast<Tool>(i)))
          applications.choices.push_back({app, {}});
      }
    }
    g_list_free(installed);
    for (auto& applications : result) {
      std::ranges::sort(applications.choices, [](const Choice& left, const Choice& right) {
        return g_utf8_collate(
                   g_app_info_get_display_name(G_APP_INFO(left.app.get())),
                   g_app_info_get_display_name(G_APP_INFO(right.app.get()))
               )
            < 0;
      });
    }
    // Disk analysers often have no MIME default. Use the first available choice
    // so the main button still launches an analyser and shows its icon.
    auto& diskUsage = result[static_cast<std::size_t>(Tool::DiskUsage)];
    if (!diskUsage.preferred && !diskUsage.choices.empty())
      diskUsage.preferred = diskUsage.choices.front().app;
    return result;
  }

  inline bool validTargets(Tool tool, std::span<const std::string> paths) {
    const bool compare = tool == Tool::Compare;
    return paths.size() == (compare ? 2 : 1)
        && (!compare || paths[0] != paths[1])
        && std::ranges::all_of(paths, [compare](const std::filesystem::path& path) {
             if (!path.is_absolute())
               return false;
             std::error_code error;
             return compare ? std::filesystem::is_regular_file(path, error)
                            : std::filesystem::is_directory(path, error);
           });
  }

  inline std::string quoteExecArgument(std::string_view argument) {
    std::string escaped;
    for (char ch : argument) {
      escaped += ch;
      if (ch == '%')
        escaped += '%';
    }
    const std::unique_ptr<char, decltype(&g_free)> quoted(g_shell_quote(escaped.c_str()), g_free);
    return quoted.get();
  }

  inline std::string quoteRawExecToken(std::string_view token) {
    const std::unique_ptr<char, decltype(&g_free)> quoted(g_shell_quote(std::string(token).c_str()), g_free);
    return quoted.get();
  }

  inline bool isFileFieldCodeToken(std::string_view token) {
    return token == "%f" || token == "%F" || token == "%u" || token == "%U";
  }

  inline std::string joinExecTokens(const std::vector<std::string>& tokens) {
    std::string joined;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
      if (i > 0)
        joined += ' ';
      joined += tokens[i];
    }
    return joined;
  }

  inline std::string terminalOption(GKeyFile* file, const std::string& actionGroup, const char* name) {
    const std::string legacy = std::string("X-") + name;
    for (const auto* section : {actionGroup.c_str(), group}) {
      for (const auto* key : {name, legacy.c_str()}) {
        const std::unique_ptr<char, decltype(&g_free)> value(
            g_key_file_get_string(file, section, key, nullptr), g_free
        );
        if (value)
          return value.get();
      }
      if (actionGroup == group)
        break;
    }
    return {};
  }

  inline bool ghosttyForcesSingleInstance(GDesktopAppInfo* app, const char* exec) {
    if (g_strcmp0(g_app_info_get_id(G_APP_INFO(app)), "com.mitchellh.ghostty.desktop") != 0)
      return false;
    gchar** argv = nullptr;
    if (!g_shell_parse_argv(exec, nullptr, &argv, nullptr))
      return false;
    const std::unique_ptr<char*, decltype(&g_strfreev)> args(argv, g_strfreev);
    return g_strv_contains(args.get(), "--gtk-single-instance=true");
  }

  inline App launchEntry(const Choice& choice, Tool tool, const std::string& directory, GError** error) {
    const bool terminal = tool == Tool::Terminal;
    const bool multiple = tool == Tool::Compare;
    const char* command = g_app_info_get_commandline(G_APP_INFO(choice.app.get()));
    if (!terminal && command && execTakesInputs(command, multiple))
      return choice.app;

    const char* filename = g_desktop_app_info_get_filename(choice.app.get());
    if (!filename || !*filename) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Application desktop entry file is unavailable");
      return {};
    }
    const std::unique_ptr<GKeyFile, decltype(&g_key_file_unref)> file(g_key_file_new(), g_key_file_unref);
    if (!g_key_file_load_from_file(file.get(), filename, G_KEY_FILE_NONE, error))
      return {};
    const std::string actionGroup = choice.desktopAction.empty() ? group : "Desktop Action " + choice.desktopAction;
    const std::unique_ptr<char, decltype(&g_free)> exec(
        g_key_file_get_string(file.get(), actionGroup.c_str(), G_KEY_FILE_DESKTOP_KEY_EXEC, error), g_free
    );
    if (!exec)
      return {};

    gint argc = 0;
    gchar** rawArgv = nullptr;
    if (!g_shell_parse_argv(exec.get(), &argc, &rawArgv, error) || argc <= 0) {
      g_strfreev(rawArgv);
      if (error && *error == nullptr)
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Desktop entry Exec command is empty");
      return {};
    }
    const std::unique_ptr<char*, decltype(&g_strfreev)> parsedArgv(rawArgv, g_strfreev);
    std::vector<std::string> tokens;
    tokens.reserve(static_cast<std::size_t>(argc) + 8);
    for (gint i = 0; i < argc; ++i) {
      const std::string_view token(parsedArgv.get()[i]);
      if (terminal && isFileFieldCodeToken(token))
        continue;
      tokens.push_back(quoteRawExecToken(token));
    }

    if (terminal) {
      // Insert terminal flags before any "--" or "-e" / TerminalArgExec separator
      // so options are never forwarded to a subcommand instead of the terminal.
      const auto execute = terminalOption(file.get(), actionGroup, "TerminalArgExec");
      std::size_t insertAt = tokens.size();
      bool hasCommandSeparator = false;
      for (gint i = 1; i < argc; ++i) {
        const std::string_view raw(parsedArgv.get()[i]);
        if (isFileFieldCodeToken(raw))
          continue;
        if (raw == "--" || raw == "-e" || (!execute.empty() && raw == execute)) {
          hasCommandSeparator = true;
          // Find corresponding index in `tokens` (skipping stripped file field codes).
          std::size_t mapped = 0;
          for (gint j = 0; j < i; ++j)
            if (!isFileFieldCodeToken(parsedArgv.get()[j]))
              ++mapped;
          insertAt = mapped;
          break;
        }
      }

      std::vector<std::string> inserted;
      // Ghostty's single-instance forwarding discards the child directory and
      // command. Keep the registered launcher and let this invocation start
      // its own instance with the advertised working-directory option.
      const bool separateInstance = ghosttyForcesSingleInstance(choice.app.get(), exec.get());
      if (separateInstance)
        inserted.push_back(quoteExecArgument("--gtk-single-instance=false"));
      const auto dirOption = terminalOption(file.get(), actionGroup, "TerminalArgDir");
      if (!dirOption.empty()) {
        if (dirOption.ends_with('='))
          inserted.push_back(quoteExecArgument(dirOption + directory));
        else {
          inserted.push_back(quoteExecArgument(dirOption));
          inserted.push_back(quoteExecArgument(directory));
        }
      }
      tokens.insert(tokens.begin() + static_cast<std::ptrdiff_t>(insertAt), inserted.begin(), inserted.end());

      if (!separateInstance
          && !hasCommandSeparator
          && g_key_file_get_boolean(file.get(), group, G_KEY_FILE_DESKTOP_KEY_DBUS_ACTIVATABLE, nullptr)
          && !execute.empty()) {
        // A running terminal may create the new shell in its own process. Give
        // that shell an explicit chdir when the entry advertises command mode.
        tokens.push_back(quoteExecArgument(execute));
        tokens.push_back(quoteExecArgument("/bin/sh"));
        tokens.push_back(quoteExecArgument("-c"));
        tokens.push_back(quoteExecArgument("cd -- \"$1\" || exit; exec \"${SHELL:-/bin/sh}\""));
        tokens.push_back(quoteExecArgument("sh"));
        tokens.push_back(quoteExecArgument(directory));
      }
      g_key_file_set_string(file.get(), group, G_KEY_FILE_DESKTOP_KEY_PATH, directory.c_str());
    } else {
      if (multiple && execTakesInputs(exec.get(), false)) {
        g_set_error_literal(
            error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Application accepts only one file per launch"
        );
        return {};
      }
      // Known positional CLI, but this desktop entry omits the file field code.
      tokens.push_back(multiple ? "%F" : "%f");
    }
    const std::string commandLine = joinExecTokens(tokens);
    g_key_file_set_string(file.get(), group, G_KEY_FILE_DESKTOP_KEY_EXEC, commandLine.c_str());
    g_key_file_set_boolean(file.get(), group, G_KEY_FILE_DESKTOP_KEY_DBUS_ACTIVATABLE, FALSE);
    auto app = takeApp(g_desktop_app_info_new_from_keyfile(file.get()));
    if (!app)
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not prepare the application's desktop entry");
    return app;
  }

  inline bool launch(const Choice& choice, Tool tool, std::span<const std::string> paths, GError** error) {
    if (!validTargets(tool, paths)) {
      g_set_error_literal(
          error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
          tool == Tool::Compare ? "Choose two local files to compare" : "A local directory is required"
      );
      return false;
    }
    const auto app = launchEntry(choice, tool, paths.front(), error);
    if (!app)
      return false;
    GList* files = nullptr;
    if (tool != Tool::Terminal) {
      for (const auto& path : paths)
        files = g_list_prepend(files, g_file_new_for_path(path.c_str()));
      files = g_list_reverse(files);
    }
    const bool launched = g_app_info_launch(G_APP_INFO(app.get()), files, nullptr, error);
    g_list_free_full(files, g_object_unref);
    return launched;
  }

} // namespace filemanager::external_tools
