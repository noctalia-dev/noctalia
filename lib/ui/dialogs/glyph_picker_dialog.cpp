#include "ui/dialogs/glyph_picker_dialog.h"

#include "i18n/i18n.h"

#include <utility>

namespace {

  GlyphPickerDialogOptions s_options;
  GlyphPickerDialog::CompletionCallback s_callback;
  GlyphPickerDialogPresenter* s_presenter = nullptr;
  std::optional<GlyphPickerResult> s_lastResult;
  bool s_hasPendingCallback = false;

} // namespace

void GlyphPickerDialog::setPresenter(GlyphPickerDialogPresenter* presenter) noexcept { s_presenter = presenter; }

bool GlyphPickerDialog::open(GlyphPickerDialogOptions options, CompletionCallback callback) {
  if (s_hasPendingCallback && s_callback) {
    auto previous = std::move(s_callback);
    s_hasPendingCallback = false;
    previous(std::nullopt);
  }

  if (options.title.empty()) {
    options.title = i18n::tr("ui.dialogs.glyph-picker.title");
  }

  s_options = std::move(options);
  s_callback = std::move(callback);
  s_hasPendingCallback = static_cast<bool>(s_callback);

  if (s_presenter == nullptr || !s_presenter->openGlyphPicker()) {
    auto pending = std::move(s_callback);
    s_callback = {};
    s_hasPendingCallback = false;
    s_options = {};
    if (pending) {
      pending(std::nullopt);
    }
    return false;
  }

  return true;
}

void GlyphPickerDialog::complete(const GlyphPickerResult& result) {
  s_lastResult = result;
  auto callback = std::move(s_callback);
  s_callback = {};
  s_hasPendingCallback = false;
  s_options = {};
  if (callback) {
    callback(result);
  }
}

void GlyphPickerDialog::cancelIfPending() {
  if (!s_hasPendingCallback) {
    return;
  }
  auto callback = std::move(s_callback);
  s_callback = {};
  s_hasPendingCallback = false;
  s_options = {};
  if (callback) {
    callback(std::nullopt);
  }
}

const GlyphPickerDialogOptions& GlyphPickerDialog::currentOptions() { return s_options; }

std::optional<GlyphPickerResult> GlyphPickerDialog::lastResult() noexcept { return s_lastResult; }
