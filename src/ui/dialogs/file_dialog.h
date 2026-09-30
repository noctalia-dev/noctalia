#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

enum class FileDialogMode : std::uint8_t {
  Open,
  Save,
  SelectFolder,
};

enum class FileDialogViewMode : std::uint8_t {
  List,
  Grid,
};

struct FileDialogOptions {
  FileDialogMode mode = FileDialogMode::Open;
  FileDialogViewMode defaultViewMode = FileDialogViewMode::List;
  std::filesystem::path startDirectory;
  std::vector<std::string> extensions;
  std::string defaultFilename;
  std::string title;
  bool showHiddenFiles = false;

  FileDialogOptions& withHiddenFiles(bool show = true) {
    showHiddenFiles = show;
    return *this;
  }
};

class FileDialogPresenter {
public:
  virtual ~FileDialogPresenter() = default;

  [[nodiscard]] virtual bool openFileDialog() = 0;
  virtual void closeFileDialogWithoutResult() = 0;
};

class FileDialog {
public:
  using CompletionCallback = std::function<void(std::optional<std::filesystem::path>)>;

  static void setPresenter(FileDialogPresenter* presenter) noexcept;
  [[nodiscard]] static bool open(FileDialogOptions options, CompletionCallback callback);
  static void complete(std::optional<std::filesystem::path> result);
  static void cancelIfPending();
  static void setLastBrowsedDirectory(std::filesystem::path directory) noexcept;

  [[nodiscard]] static const FileDialogOptions& currentOptions();
};
