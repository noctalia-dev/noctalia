#include "ui/dialogs/file_dialog.h"

#include "i18n/i18n.h"

#include <utility>
#include <vector>

namespace {

  FileDialogOptions s_options;
  FileDialog::CompletionCallback s_callback;
  FileDialog::MultiCompletionCallback s_multiCallback;
  FileDialogPresenter* s_presenter = nullptr;
  bool s_hasPendingCallback = false;

  std::string defaultTitle(FileDialogMode mode) {
    switch (mode) {
    case FileDialogMode::Open:
      return i18n::tr("ui.dialogs.file.title.open");
    case FileDialogMode::Save:
      return i18n::tr("ui.dialogs.file.title.save");
    case FileDialogMode::SelectFolder:
      return i18n::tr("ui.dialogs.file.title.select-folder");
    }
    return i18n::tr("ui.dialogs.file.title.default");
  }

  /// The one place a dialog finishes. Both entry points funnel here so a
  /// single-path caller and a multi-path caller cannot drift apart: an empty
  /// vector is a cancel either way, and a single-path caller handed several
  /// paths takes the first rather than silently dropping the answer.
  void finishWith(std::vector<std::filesystem::path> paths) {
    auto single = std::move(s_callback);
    auto multi = std::move(s_multiCallback);
    s_callback = {};
    s_multiCallback = {};
    s_hasPendingCallback = false;
    s_options = {};

    if (multi) {
      multi(std::move(paths));
      return;
    }
    if (single) {
      single(paths.empty() ? std::nullopt : std::optional{std::move(paths.front())});
    }
  }

} // namespace

void FileDialog::setPresenter(FileDialogPresenter* presenter) noexcept { s_presenter = presenter; }

namespace {
  /// Shared prologue: answer whatever is still pending, then install the new
  /// request. Leaving an old callback in place would strand its caller.
  void beginRequest(FileDialogOptions& options) {
    if (s_hasPendingCallback) {
      finishWith({});
    }
    if (options.title.empty()) {
      options.title = defaultTitle(options.mode);
    }
  }
} // namespace

bool FileDialog::open(FileDialogOptions options, CompletionCallback callback) {
  beginRequest(options);

  s_options = std::move(options);
  s_callback = std::move(callback);
  s_multiCallback = {};
  s_hasPendingCallback = static_cast<bool>(s_callback);

  if (s_presenter == nullptr || !s_presenter->openFileDialog()) {
    finishWith({});
    return false;
  }

  return true;
}

bool FileDialog::openMultiple(FileDialogOptions options, MultiCompletionCallback callback) {
  // Only Open has a meaning for a set; the other modes ignore the flag.
  options.allowMultiple = options.mode == FileDialogMode::Open;
  beginRequest(options);

  s_options = std::move(options);
  s_multiCallback = std::move(callback);
  s_callback = {};
  s_hasPendingCallback = static_cast<bool>(s_multiCallback);

  if (s_presenter == nullptr || !s_presenter->openFileDialog()) {
    finishWith({});
    return false;
  }

  return true;
}

void FileDialog::complete(std::optional<std::filesystem::path> result) {
  std::vector<std::filesystem::path> paths;
  if (result.has_value()) {
    paths.push_back(std::move(*result));
  }
  finishWith(std::move(paths));
}

void FileDialog::completeMultiple(std::vector<std::filesystem::path> results) { finishWith(std::move(results)); }

void FileDialog::cancelIfPending() {
  if (!s_hasPendingCallback) {
    return;
  }
  finishWith({});
}

const FileDialogOptions& FileDialog::currentOptions() { return s_options; }
