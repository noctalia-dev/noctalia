#include "shell/desktop/widgets/desktop_lyrics_widget.h"

#include "core/ui_phase.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "render/animation/animation_manager.h"
#include "render/core/renderer.h"
#include "shell/desktop/widgets/karaoke_label.h"
#include "system/lyrics_catalog.h"
#include "system/lyrics_service.h"
#include "ui/builders.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
  // Layout latency is a few frames; anything longer is a stall and is not used to switch lines early.
  constexpr float kMaxLineLeadMs = 250.0F;
  // A line taking longer than this many frames to appear was delayed by a hidden surface, not by layout.
  constexpr float kMaxLeadFrames = 4.0F;
  constexpr float kWidth = 480.0F;
  constexpr float kFontSize = 22.0F;
  constexpr float kRowHeight = 38.0F;
  constexpr float kLinePadding = 4.0F;
  constexpr float kShadowAlpha = 0.6F;
  constexpr float kShadowOffset = 1.5F;
  // The text is sized so this share of the lines fits the widget; the few wider lines shrink individually.
  constexpr float kTypicalLineShare = 0.8F;
  // How far before the shown line a position may fall before it counts as a seek; smaller moves are jitter.
  constexpr std::int64_t kBackwardSlackMs = 500;
} // namespace

DesktopLyricsWidget::DesktopLyricsWidget(MprisService& mpris, LyricsService& lyrics, Options options)
    : m_mpris(mpris), m_lyrics(lyrics), m_options(options) {}

void DesktopLyricsWidget::create() {
  auto content = ui::node({});
  content->setClipChildren(true);
  setRoot(std::move(content));
}

void DesktopLyricsWidget::rebuildLabels() {
  uiAssertNotRendering("DesktopLyricsWidget::rebuildLabels");
  for (auto* label : m_labels) {
    root()->removeChild(label);
  }
  m_labels.clear();
  m_labels.reserve(labelCount());
  for (std::size_t i = 0; i < labelCount(); ++i) {
    auto label = std::make_unique<KaraokeLabel>();
    label->label().setMaxLines(1);
    label->label().setTextAlign(TextAlign::Center);
    label->label().setFontFamily(m_fontFamily);
    m_labels.push_back(static_cast<KaraokeLabel*>(root()->addChild(std::move(label))));
  }
}

void DesktopLyricsWidget::doUpdate(Renderer& /*renderer*/) {
  const auto player = m_mpris.activePlayer();
  const auto now = std::chrono::steady_clock::now();
  const bool offline = m_lyrics.offline();
  const bool hasTrack = player
      && player->playbackStatus != "Stopped"
      && !player->title.empty()
      && !joinedArtists(player->artists).empty();
  const std::string playerKey = hasTrack ? LyricsService::trackKey(*player) : std::string{};
  std::optional<std::int64_t> expectedPositionUs;
  if (player && m_lastSeen && m_lastSeen->busName == player->busName) {
    expectedPositionUs = m_lastSeen->positionUs;
    if (m_lastSeen->playing) {
      const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(now - m_lastSeen->at).count();
      *expectedPositionUs += static_cast<std::int64_t>(static_cast<double>(elapsedUs) * m_lastSeen->rate);
    }
  }
  if (player) {
    m_lastSeen = LastSeen{
        .busName = player->busName,
        .positionUs = player->positionUs,
        .rate = player->rate,
        .playing = player->playbackStatus == "Playing",
        .at = now,
    };
  } else {
    m_lastSeen.reset();
  }
  // Players such as Chromium report other metadata for the same track (a scraped page title, or a hover-preview video
  // while the real one keeps playing or is paused). Keep the lyrics only while that holds; it is re-checked on every
  // update, so a real change is looked up as soon as the evidence stops matching.
  const bool holding = hasTrack
      && playerKey != m_requestKey
      && m_result
      && m_result->status == LyricsStatus::READY
      && player->busName == m_lookupBus
      && LyricsService::continuesTrack(*player, m_lookupTrackId, m_lookupTitle, m_lookupDurationUs, expectedPositionUs);
  m_holding = holding;
  const std::string key = holding ? m_requestKey : playerKey;
  const bool sameTrack = !key.empty() && key == m_requestKey;
  const bool metadataImproved = hasTrack
      && ((m_lookupAlbum.empty() && !player->album.empty())
          || (m_lookupDurationUs <= 0 && player->lengthUs > 0)
          || std::abs(static_cast<double>(player->lengthUs) - static_cast<double>(m_lookupDurationUs)) >= 1000000.0);
  const bool missingLyrics = !m_result || m_result->status != LyricsStatus::READY;
  const bool retry = m_result && missingLyrics && !offline && now >= m_retryAt;
  if (key != m_requestKey
      || offline != m_offline
      || retry
      || (sameTrack && metadataImproved && missingLyrics && !offline)) {
    m_requestKey = key;
    m_offline = offline;
    if (!sameTrack || missingLyrics) {
      m_result.reset();
    }
    if (hasTrack) {
      m_lookupAlbum = player->album;
      m_lookupTitle = player->title;
      m_lookupBus = player->busName;
      m_lookupTrackId = player->trackId;
      m_lookupDurationUs = player->lengthUs;
    }
    const auto generation = ++m_requestGeneration;
    if (hasTrack) {
      const std::weak_ptr<void> guard = m_aliveGuard;
      m_lyrics.request(*player, [this, guard, generation](std::shared_ptr<const LyricsResult> result) {
        if (guard.expired() || generation != m_requestGeneration) {
          return;
        }
        // Keep a usable snapshot while an unchanged track is refreshed or taken offline.
        if (!m_result || m_result->status != LyricsStatus::READY || result->status == LyricsStatus::READY) {
          m_result = std::move(result);
        }
        m_retryAt = std::chrono::steady_clock::now()
            + (m_result->status == LyricsStatus::NOT_FOUND ? std::chrono::seconds{300} : std::chrono::seconds{5});
        requestUpdate();
      });
    }
  }

  // One hidden row above and below the visible window lets lines scroll in and out.
  std::vector<std::string> rows(labelCount());
  const auto middle = static_cast<std::size_t>((m_options.lineCount - 1) / 2) + 1;
  std::optional<std::size_t> activeRow;
  std::optional<std::size_t> activeLine;
  m_animateHighlight = false;
  auto delay = std::chrono::milliseconds{1000};
  if (!hasTrack) {
    rows[middle] = i18n::tr("desktop-widgets.lyrics.nothing-playing");
  } else if (!m_result) {
    rows[middle] = i18n::tr("desktop-widgets.lyrics.loading");
  } else if (m_result->status != LyricsStatus::READY) {
    switch (m_result->status) {
    case LyricsStatus::NOT_FOUND:
      rows[middle] = i18n::tr("desktop-widgets.lyrics.not-found");
      break;
    case LyricsStatus::OFFLINE:
      rows[middle] = i18n::tr("desktop-widgets.lyrics.offline");
      break;
    case LyricsStatus::ERROR:
      rows[middle] = i18n::tr("desktop-widgets.lyrics.error");
      break;
    case LyricsStatus::READY:
      break;
    }
  } else if (m_result->document.instrumental) {
    rows[middle] = i18n::tr("desktop-widgets.lyrics.instrumental");
  } else {
    const auto& document = m_result->document;
    const auto positionMs = player->positionUs / 1000 + m_options.syncOffsetMs;
    const auto lineMs = positionMs + lineLeadMs();
    const auto active = selectLine(document, positionMs);
    auto window = document.windowAround(active, rows.size());
    rows = std::move(window.lines);
    if (document.lines.empty()) {
      std::ranges::shift_right(rows, 1);
      rows.front().clear();
    }
    activeRow = window.activeRow;
    activeLine = active;
    if (active && player->playbackStatus == "Playing") {
      const auto& line = document.lines[*active];
      const auto end = line.endMs               ? line.endMs
          : *active + 1 < document.lines.size() ? std::optional{document.lines[*active + 1].timeMs}
                                                : std::nullopt;
      m_animateHighlight = !line.text.empty()
          && ((end && positionMs < *end) || (!line.words.empty() && positionMs < line.words.back().endMs));
    }
    const auto next = active ? *active + 1 : 0;
    if (next < document.lines.size() && player->rate > 0.0) {
      auto remaining = static_cast<double>(document.lines[next].timeMs - lineMs);
      // Wake when the current line finishes so the early switch is not delayed until the next line.
      if (active && m_animateHighlight) {
        const auto& current = document.lines[*active];
        if (!current.words.empty() && current.words.back().endMs + lineHoldMs() > positionMs) {
          remaining = std::min(remaining, static_cast<double>(current.words.back().endMs + lineHoldMs() - positionMs));
        }
      }
      remaining /= player->rate;
      delay = std::chrono::milliseconds{static_cast<std::int64_t>(std::clamp(std::ceil(remaining), 1.0, 1000.0))};
    }
  }

  if (activeLine
      && m_activeLine
      && m_result
      && *activeLine == *m_activeLine + 1
      && m_laidOutGeneration == m_requestGeneration) {
    const auto& lines = m_result->document.lines;
    const auto animation = std::chrono::milliseconds{Style::animNormal};
    // Fast lines get a shorter scroll so one finishes before the next begins.
    const auto gap = *activeLine + 1 < lines.size()
        ? std::chrono::milliseconds{(lines[*activeLine + 1].timeMs - lines[*activeLine].timeMs) / 2}
        : animation;
    m_lineMeasureStart = m_lineTriggerAt.value_or(now);
    startScroll(std::clamp(gap, std::chrono::milliseconds{1}, animation));
  } else if (activeLine != m_activeLine) {
    stopScroll();
  }
  m_lineTriggerAt.reset();
  if (rows != m_rows
      || activeRow != m_activeRow
      || activeLine != m_activeLine
      || m_laidOutGeneration != m_requestGeneration) {
    m_rows = std::move(rows);
    m_activeRow = activeRow;
    m_activeLine = activeLine;
    requestLayout();
  }

  syncHighlight();
  if (m_animateHighlight) {
    requestFrameTick();
  }

  m_updateTimer.stop();
  if (hasTrack && player->playbackStatus == "Playing") {
    m_updateTimer.start(delay, [this] { triggerLineUpdate(); });
  } else if (hasTrack && m_result && m_result->status != LyricsStatus::READY && !offline) {
    m_updateTimer.start(
        std::max(std::chrono::milliseconds{1}, std::chrono::ceil<std::chrono::milliseconds>(m_retryAt - now)),
        [this] { requestUpdate(); }
    );
  }
}

void DesktopLyricsWidget::doLayout(Renderer& renderer) {
  if (root() == nullptr) {
    return;
  }
  if (m_labels.size() != labelCount()) {
    rebuildLabels();
  }
  refreshWidthCache(renderer);
  float scale = contentScale();
  float width = songWidthAtScale(renderer, scale);
  const float availableWidth = boxInnerWidth();
  if (scale != m_baseScale && availableWidth > 0.0F) {
    if (!m_fittedMeasurement
        || m_fittedMeasurement->requestedScale != scale
        || m_fittedMeasurement->availableWidth != availableWidth) {
      const float requestedScale = scale;
      // Text hinting at the fitted size can differ from a linear scale of its natural width.
      for (int pass = 0; pass < 12 && width > availableWidth; ++pass) {
        scale = std::max(std::numeric_limits<float>::epsilon(), scale * availableWidth / width * 0.995F);
        width = songWidthAtScale(renderer, scale);
      }
      m_fittedMeasurement = FittedMeasurement{
          .requestedScale = requestedScale,
          .availableWidth = availableWidth,
          .measured = {.scale = scale, .width = width},
      };
    }
    scale = m_fittedMeasurement->measured.scale;
    width = m_fittedMeasurement->measured.width;
  }
  const float rowHeight = kRowHeight * scale;
  m_rowHeight = rowHeight;
  m_labelBaseY.assign(m_labels.size(), 0.0F);
  for (std::size_t i = 0; i < m_labels.size(); ++i) {
    auto* label = m_labels[i];
    auto& text = label->label();
    text.setFontSize(kFontSize * scale);
    text.setMaxWidth(0.0F);
    text.setText(i < m_rows.size() ? m_rows[i] : std::string{});
    text.setColor(m_options.color);
    std::vector<KaraokeRange> ranges;
    if (m_activeRow == i && m_activeLine && m_result) {
      const auto& line = m_result->document.lines[*m_activeLine];
      if (line.words.empty() && !line.text.empty()) {
        ranges.push_back({.endByte = line.text.size()});
      } else {
        for (const auto& word : line.words) {
          ranges.push_back({.startByte = word.startByte, .endByte = word.endByte});
        }
      }
    }
    label->setRanges(std::move(ranges));
    label->setHighlightColor(m_options.activeColor);
    if (m_options.shadow) {
      text.setShadow(colorSpecFromRole(ColorRole::Shadow, kShadowAlpha), kShadowOffset * scale, kShadowOffset * scale);
    } else {
      text.clearShadow();
    }
    label->measure(renderer);
    // A line wider than the widget shrinks on its own instead of shrinking every line.
    const float rowWidth = width - 2.0F * kLinePadding * scale;
    for (int pass = 0; pass < 4 && rowWidth > 0.0F && label->width() > rowWidth; ++pass) {
      text.setFontSize(text.fontSize() * rowWidth / label->width() * 0.995F);
      label->measure(renderer);
    }
    m_labelBaseY[i] = (static_cast<float>(i) - 1.0F) * rowHeight + (rowHeight - label->height()) * 0.5F;
    label->setPosition((width - label->width()) * 0.5F, m_labelBaseY[i]);
  }
  m_laidOutGeneration = m_requestGeneration;
  m_laidOutLine = m_activeLine;
  m_laidOutRow = m_activeRow;
  syncHighlight();
  applyScroll();
  if (m_lineMeasureStart && m_laidOutLine == m_activeLine) {
    const float sample =
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - *m_lineMeasureStart).count()
        + m_frameMs;
    // Follow the slowest recent change so a slow frame never starts the highlight late. Longer delays are stalls (an
    // occluded surface or startup), not layout latency, and would otherwise make every later line switch early.
    const float stallMs = m_frameMs > 0.0F ? std::min(kMaxLineLeadMs, m_frameMs * kMaxLeadFrames) : kMaxLineLeadMs;
    if (sample <= stallMs) {
      m_lineLatencyMs =
          !m_latencyMeasured || sample > m_lineLatencyMs ? sample : m_lineLatencyMs * 0.9F + sample * 0.1F;
      m_latencyMeasured = true;
    }
    m_lineMeasureStart.reset();
  }
  root()->setSize(width, rowHeight * static_cast<float>(m_options.lineCount));
}

void DesktopLyricsWidget::refreshWidthCache(Renderer& renderer) {
  const bool ready = m_result && m_result->status == LyricsStatus::READY;
  WidthCacheKey key{
      .result = m_result,
      .statusRows = ready ? std::vector<std::string>{} : m_rows,
      .fontFamily = m_fontFamily,
      .baseScale = m_baseScale,
      .renderScale = renderer.renderScale(),
      .fontGeneration = renderer.textMetricsGeneration(),
      .lineCount = m_options.lineCount,
  };
  if (m_widthCacheKey == key) {
    return;
  }
  m_widthCacheKey = std::move(key);
  m_widthMeasurements.clear();
  m_fittedMeasurement.reset();
  m_maxNaturalWidth = 0.0F;
  m_maxNaturalHeight = 0.0F;
}

float DesktopLyricsWidget::songWidthAtScale(Renderer& renderer, float scale) {
  if (const auto existing = std::ranges::find(m_widthMeasurements, scale, &WidthMeasurement::scale);
      existing != m_widthMeasurements.end()) {
    return existing->width;
  }
  std::vector<float> widths;
  const TextFont font{.family = m_fontFamily};
  const auto include = [&](std::string_view text) {
    const auto metrics = renderer.measureText(text, kFontSize * scale, font, 0.0F, 1);
    widths.push_back(std::ceil(metrics.width) + 2.0F * kLinePadding * scale);
  };
  if (m_result && m_result->status == LyricsStatus::READY && !m_result->document.lines.empty()) {
    for (const auto& line : m_result->document.lines) {
      include(line.text);
    }
  } else if (m_result && m_result->status == LyricsStatus::READY && !m_result->document.plainLines.empty()) {
    for (const auto& line : m_result->document.plainLines) {
      include(line);
    }
  } else {
    for (const auto& row : m_rows) {
      include(row);
    }
  }
  float width = kWidth * scale;
  if (!widths.empty()) {
    std::ranges::sort(widths);
    const auto typical = static_cast<std::size_t>(std::ceil(kTypicalLineShare * static_cast<float>(widths.size())));
    width = std::max(width, widths[std::min(widths.size(), std::max<std::size_t>(typical, 1)) - 1]);
  }
  constexpr std::size_t kMaxWidthMeasurements = 8;
  if (m_widthMeasurements.size() == kMaxWidthMeasurements) {
    m_widthMeasurements.erase(m_widthMeasurements.begin() + 1);
  }
  m_widthMeasurements.push_back({.scale = scale, .width = width});
  return width;
}

void DesktopLyricsWidget::onFontFamilyChanged(const std::string& family, Renderer& /*renderer*/) {
  for (auto* label : m_labels) {
    label->label().setFontFamily(family);
  }
}

void DesktopLyricsWidget::syncHighlight() {
  if (!m_activeRow
      || !m_activeLine
      || !m_result
      || *m_activeRow >= m_labels.size()
      || m_laidOutGeneration != m_requestGeneration
      || m_laidOutLine != m_activeLine
      || m_laidOutRow != m_activeRow
      || m_labels.size() != labelCount()) {
    return;
  }
  const auto player = m_mpris.activePlayer();
  if (!player || (!m_holding && LyricsService::trackKey(*player) != m_requestKey)) {
    return;
  }
  const auto& document = m_result->document;
  const auto positionMs = player->positionUs / 1000 + m_options.syncOffsetMs;
  if (selectLine(document, positionMs) != m_activeLine) {
    triggerLineUpdate();
    return;
  }
  const auto next = *m_activeLine + 1;
  const auto progress = document.lines[*m_activeLine].progress(
      positionMs, next < document.lines.size() ? std::optional{document.lines[next].timeMs} : std::nullopt,
      lineLeadMs() + lineHoldMs()
  );
  m_labels[*m_activeRow]->setProgress(progress.segment, progress.fraction);
}

std::size_t DesktopLyricsWidget::labelCount() const { return static_cast<std::size_t>(m_options.lineCount) + 2; }

// The line to show: the one due `lineLeadMs()` from now, but the word-timed line already on screen stays until its last
// word has been sung and shown filled for `lineHoldMs()`, even when the next line is already due.
std::optional<std::size_t>
DesktopLyricsWidget::selectLine(const LyricsDocument& document, std::int64_t positionMs) const {
  const auto target = document.activeLine(positionMs + lineLeadMs());
  // Position corrections and a shrinking lead must not bounce the view back a line; only a seek moves it backwards.
  if (m_activeLine && *m_activeLine < document.lines.size()) {
    const bool backwards = !target || *target < *m_activeLine;
    if (backwards && positionMs + lineLeadMs() >= document.lines[*m_activeLine].timeMs - kBackwardSlackMs) {
      return m_activeLine;
    }
  }
  if (m_activeLine && target && *target > *m_activeLine && *m_activeLine < document.lines.size()) {
    const auto& shown = document.lines[*m_activeLine];
    if (!shown.words.empty() && positionMs < shown.words.back().endMs + lineHoldMs()) {
      return m_activeLine;
    }
  }
  return target;
}

std::int64_t DesktopLyricsWidget::lineHoldMs() const { return std::llround(m_frameMs * 2.0F); }

std::int64_t DesktopLyricsWidget::lineLeadMs() const {
  return std::clamp(std::llround(m_lineLatencyMs + m_frameMs), 0LL, static_cast<long long>(kMaxLineLeadMs));
}

void DesktopLyricsWidget::triggerLineUpdate() {
  if (!m_lineTriggerAt) {
    m_lineTriggerAt = std::chrono::steady_clock::now();
  }
  requestUpdate();
}

void DesktopLyricsWidget::startScroll(std::chrono::milliseconds duration) {
  if (m_animations == nullptr) {
    return;
  }
  // A scroll interrupted by the next line continues from where it is.
  const float from = std::min(m_scrollRows + 1.0F, 2.0F);
  if (m_scrollAnimId != 0) {
    m_animations->cancel(m_scrollAnimId);
  }
  m_scrollRows = from;
  m_scrollAnimId = m_animations->animate(
      from, 0.0F, static_cast<float>(duration.count()), Easing::EaseOutCubic,
      [this](float rows) {
        m_scrollRows = rows;
        applyScroll();
        requestRedraw();
      },
      [this] {
        m_scrollAnimId = 0;
        m_scrollRows = 0.0F;
        applyScroll();
        requestRedraw();
      },
      this
  );
  requestFrameTick();
}

void DesktopLyricsWidget::stopScroll() {
  if (m_scrollAnimId != 0 && m_animations != nullptr) {
    m_animations->cancel(m_scrollAnimId);
  }
  m_scrollAnimId = 0;
  m_scrollRows = 0.0F;
  applyScroll();
}

void DesktopLyricsWidget::applyScroll() {
  for (std::size_t i = 0; i < m_labels.size() && i < m_labelBaseY.size(); ++i) {
    m_labels[i]->setPosition(m_labels[i]->x(), m_labelBaseY[i] + m_scrollRows * m_rowHeight);
  }
}

void DesktopLyricsWidget::onFrameTick(float deltaMs, Renderer& /*renderer*/) {
  if (deltaMs > 2.0F && deltaMs < 100.0F) {
    m_frameMs = m_frameMs > 0.0F ? m_frameMs * 0.9F + deltaMs * 0.1F : deltaMs;
  }
  syncHighlight();
  if (m_animateHighlight) {
    // The surface only schedules another frame after a render; keep it running through gaps between words.
    requestRedraw();
  }
  if (m_animateHighlight || m_scrollAnimId != 0) {
    requestFrameTick();
  }
}

bool DesktopLyricsWidget::applySetting(
    const std::string& key, const WidgetSettingValue& value,
    const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
) {
  if (key == "line_count") {
    const auto* count = std::get_if<std::int64_t>(&value);
    if (count == nullptr || *count < 1 || *count > 15) {
      return false;
    }
    m_options.lineCount = static_cast<int>(*count);
    requestUpdate();
    requestLayout();
    return true;
  }
  if (key == "sync_offset_ms") {
    const auto* offset = std::get_if<std::int64_t>(&value);
    if (offset == nullptr || *offset < -10000 || *offset > 10000) {
      return false;
    }
    m_options.syncOffsetMs = static_cast<int>(*offset);
    requestUpdate();
    return true;
  }
  if (key == "color" || key == "active_color") {
    if (const auto* color = std::get_if<std::string>(&value)) {
      (key == "color" ? m_options.color : m_options.activeColor) = colorSpecFromConfigString(*color, key);
      requestLayout();
      return true;
    }
    return false;
  }
  if (key == "shadow") {
    if (const auto* shadow = std::get_if<bool>(&value)) {
      m_options.shadow = *shadow;
      requestLayout();
      return true;
    }
    return false;
  }
  return DesktopWidget::applySetting(key, value, allSettings, renderer);
}
