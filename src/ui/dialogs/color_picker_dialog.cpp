#include "ui/dialogs/color_picker_dialog.h"

#include "core/deferred_call.h"
#include "i18n/i18n.h"

#include <utility>

namespace {

  ColorPickerDialogOptions s_options;
  ColorPickerDialog::CompletionCallback s_callback;
  ColorPickerDialogPresenter* s_presenter = nullptr;
  std::optional<Color> s_lastResult;
  bool s_hasPendingCallback = false;

} // namespace

void ColorPickerDialog::setPresenter(ColorPickerDialogPresenter* presenter) noexcept { s_presenter = presenter; }

bool ColorPickerDialog::open(ColorPickerDialogOptions options, CompletionCallback callback) {
  if (s_hasPendingCallback && s_callback) {
    auto previous = std::move(s_callback);
    s_hasPendingCallback = false;
    previous(std::nullopt);
  }

  if (options.title.empty()) {
    options.title = i18n::tr("ui.dialogs.color-picker.title");
  }

  s_options = std::move(options);
  s_callback = std::move(callback);
  s_hasPendingCallback = static_cast<bool>(s_callback);

  if (s_presenter == nullptr || !s_presenter->openColorPicker()) {
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

void ColorPickerDialog::complete(const Color& result) {
  s_lastResult = result;
  auto callback = std::move(s_callback);
  s_callback = {};
  s_hasPendingCallback = false;
  s_options = {};
  if (callback) {
    DeferredCall::callLater([callback = std::move(callback), result]() { callback(result); });
  }
}

void ColorPickerDialog::cancelIfPending() {
  if (!s_hasPendingCallback) {
    return;
  }
  auto callback = std::move(s_callback);
  s_callback = {};
  s_hasPendingCallback = false;
  s_options = {};
  if (callback) {
    DeferredCall::callLater([callback = std::move(callback)]() { callback(std::nullopt); });
  }
}

const ColorPickerDialogOptions& ColorPickerDialog::currentOptions() { return s_options; }

std::optional<Color> ColorPickerDialog::lastResult() noexcept { return s_lastResult; }
