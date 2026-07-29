#pragma once

#include "ui/controls/flex.h"
#include "ui/style.h"

#include <cstdint>
#include <functional>
#include <memory>

class Glyph;
class InputArea;

class Collapsible : public Flex {
public:
  Collapsible();

  void setHeader(std::unique_ptr<Node> header);
  void setBody(std::unique_ptr<Node> body);
  void setExpanded(bool expanded);
  void setExpandedImmediate(bool expanded);
  void setScale(float scale);
  // Header row padding, scaled with setScale(). Defaults to the card-style inset; set it to 0 when
  // the header has to line up with flush-left content around it.
  void setHeaderPadding(float vertical, float horizontal);
  // Fires when the header toggles the section, not when the state is set programmatically.
  void setOnToggle(std::function<void(bool expanded)> callback);

  [[nodiscard]] bool expanded() const noexcept { return m_expanded; }

private:
  void applyExpandedProgress(float t);
  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;

  Flex* m_headerRow = nullptr;
  Node* m_userHeader = nullptr;
  Glyph* m_chevron = nullptr;
  InputArea* m_headerInput = nullptr;
  Node* m_clipContainer = nullptr;
  Node* m_bodyNode = nullptr;

  std::function<void(bool)> m_onToggle;
  std::uint32_t m_animId = 0;
  float m_expandProgress = 0.0f;
  float m_clipHeight = 0.0f;
  float m_bodyNaturalHeight = 0.0f;
  float m_scale = 1.0f;
  float m_headerPaddingV = 0.0f;
  float m_headerPaddingH = Style::spaceMd;
  bool m_expanded = false;
};
