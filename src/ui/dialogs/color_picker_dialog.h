#pragma once

#include "render/core/color.h"

#include <functional>
#include <optional>
#include <string>

struct ColorPickerDialogOptions {
  std::optional<Color> initialColor;
  std::string title;
};

class ColorPickerDialogPresenter {
public:
  virtual ~ColorPickerDialogPresenter() = default;

  [[nodiscard]] virtual bool openColorPicker() = 0;
  virtual void closeColorPickerWithoutResult() = 0;
};

class ColorPickerDialog {
public:
  using CompletionCallback = std::function<void(std::optional<Color>)>;

  static void setPresenter(ColorPickerDialogPresenter* presenter) noexcept;
  [[nodiscard]] static bool open(ColorPickerDialogOptions options, CompletionCallback callback);
  static void complete(const Color& result);
  static void cancelIfPending();

  static const ColorPickerDialogOptions& currentOptions();
  static std::optional<Color> lastResult() noexcept;
};
