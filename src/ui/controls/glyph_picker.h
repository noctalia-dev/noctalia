#pragma once

#include "ui/controls/flex.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

class Button;
class GlyphGridAdapter;
class InputArea;
class Input;
class Label;
class Select;
class VirtualGridView;

struct GlyphPickerResult {
  std::string name;
  char32_t codepoint = 0;
};

// Title + search/category filters + virtualized grid of every Tabler glyph +
// Cancel/Apply row. Reusable chrome for dialogs or embedded UIs.
class GlyphPicker : public Flex {
public:
  explicit GlyphPicker(float chromeScale);
  ~GlyphPicker() override;

  void setTitle(std::string_view title);
  // Pre-select and scroll to a specific glyph name on first layout.
  void setInitialGlyph(std::optional<std::string> name);

  [[nodiscard]] InputArea* initialFocusArea() const noexcept;
  [[nodiscard]] std::optional<GlyphPickerResult> currentResult() const;

  void setOnCancel(std::function<void()> callback) { m_onCancel = std::move(callback); }
  void setOnApply(std::function<void(const GlyphPickerResult&)> callback) { m_onApply = std::move(callback); }

  void setEnabled(bool enabled);
  [[nodiscard]] bool enabled() const noexcept { return m_enabled; }

  [[nodiscard]] static float preferredDialogWidth(float scale);
  [[nodiscard]] static float preferredDialogHeight(float scale);

protected:
  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;

private:
  void applyFilter(const std::string& filter);
  void applySelectionToButton();

  float m_chromeScale = 1.0f;
  Label* m_title = nullptr;
  Input* m_searchInput = nullptr;
  Select* m_categorySelect = nullptr;
  VirtualGridView* m_grid = nullptr;
  Button* m_applyButton = nullptr;

  std::unique_ptr<GlyphGridAdapter> m_adapter;
  std::vector<std::string> m_categoryOptions;
  std::function<void()> m_onCancel;
  std::function<void(const GlyphPickerResult&)> m_onApply;

  std::optional<std::string> m_pendingInitialGlyph;
  bool m_pendingInitialApplied = false;
  bool m_enabled = true;
};
