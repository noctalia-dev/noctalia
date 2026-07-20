#include "ui/controls/drag_source.h"

#include "cursor-shape-v1-client-protocol.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "ui/drag_drop_controller.h"

#include <algorithm>
#include <linux/input-event-codes.h>
#include <memory>
#include <utility>

DragSource::DragSource(DragDropController* controller) : m_controller(controller) {
  if (m_controller != nullptr) {
    m_controller->registerSource(this);
  }
  auto area = std::make_unique<InputArea>();
  area->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
  area->setPropagateEvents(false);
  area->setFocusable(false);
  area->setTabStop(false);
  area->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB);
  area->setOnPress([this](const InputArea::PointerData& data) {
    if (m_controller == nullptr || !m_enabled) {
      return;
    }
    if (data.pressed) {
      m_controller->arm(*this, data.localX, data.localY);
    } else {
      m_controller->release(*this, data.localX, data.localY);
    }
  });
  area->setOnMotion([this](const InputArea::PointerData& data) {
    if (m_controller != nullptr && m_enabled) {
      m_controller->motion(*this, data.localX, data.localY);
    }
  });
  area->setOnCancel([this]() {
    if (m_controller != nullptr) {
      m_controller->cancel();
    }
  });
  m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));
  m_inputArea->setParticipatesInLayout(false);
  m_inputArea->setZIndex(1);
  updateInputArea();
}

void DragSource::detachController(DragDropController* controller) noexcept {
  if (m_controller == controller) {
    m_controller = nullptr;
  }
}

DragSource::~DragSource() {
  if (m_controller != nullptr) {
    m_controller->unregisterSource(this);
  }
}

void DragSource::setDragType(std::string value) {
  if (m_dragType == value) {
    return;
  }
  if (m_controller != nullptr) {
    m_controller->sourceChanged(this);
  }
  m_dragType = std::move(value);
}

void DragSource::setPayload(std::string value) {
  if (m_payload == value) {
    return;
  }
  if (m_controller != nullptr) {
    m_controller->sourceChanged(this);
  }
  m_payload = std::move(value);
}

void DragSource::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  if (!enabled && m_controller != nullptr) {
    m_controller->sourceChanged(this);
  }
  m_enabled = enabled;
  if (m_inputArea != nullptr) {
    m_inputArea->setEnabled(enabled);
  }
}

void DragSource::setTooltip(std::string_view text) {
  m_tooltip = text;
  applyTooltip();
}

void DragSource::applyTooltip() {
  if (m_inputArea == nullptr) {
    return;
  }
  // No tooltip while dragging: hover stays on the captured grip for the whole
  // drag, so an applied tooltip would sit next to the moving ghost.
  if (m_tooltip.empty() || m_dragging) {
    m_inputArea->clearTooltip();
  } else {
    m_inputArea->setTooltip(m_tooltip);
  }
}

void DragSource::setSourceOpacity(float opacity) {
  m_sourceOpacity = std::clamp(opacity, 0.0f, 1.0f);
  applyVisualState();
}

void DragSource::setPreviewAncestor(std::size_t levels) {
  if (m_previewAncestor == levels) {
    return;
  }
  if (m_controller != nullptr) {
    m_controller->sourceChanged(this);
  }
  m_previewAncestor = levels;
  applyVisualState();
}

void DragSource::setLiftFromLayout(bool enabled) {
  if (m_liftFromLayout == enabled) {
    return;
  }
  if (m_controller != nullptr) {
    m_controller->sourceChanged(this);
  }
  m_liftFromLayout = enabled;
}

Node* DragSource::previewTarget() noexcept {
  Node* target = this;
  for (std::size_t level = 0; level < m_previewAncestor && target->parent() != nullptr; ++level) {
    target = target->parent();
  }
  return target;
}

void DragSource::setDragging(bool dragging) {
  if (m_dragging == dragging) {
    return;
  }
  m_dragging = dragging;
  applyVisualState();
  applyTooltip();
  if (m_inputArea != nullptr) {
    m_inputArea->setCursorShape(
        dragging ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB
    );
  }
}

void DragSource::setSize(float width, float height) {
  Flex::setSize(width, height);
  updateInputArea();
}

void DragSource::applyVisualState() {
  setOpacity(m_dragging && m_previewAncestor == 0 ? m_sourceOpacity * 0.55f : m_sourceOpacity);
}

void DragSource::updateInputArea() {
  if (m_inputArea != nullptr) {
    m_inputArea->setPosition(0.0f, 0.0f);
    m_inputArea->setFrameSize(width(), height());
  }
}

void DragSource::doLayout(Renderer& renderer) {
  Flex::doLayout(renderer);
  updateInputArea();
}
