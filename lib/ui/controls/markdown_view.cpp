#include "ui/controls/markdown_view.h"

#include "ui/builders.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/separator.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <md4c.h>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

  struct MdContext {
    MarkdownView* view = nullptr;
    float scale = 1.0F;
    float fontScale = 1.0F;
    std::string textBuf;
    int headingLevel = 0;
    bool inCodeBlock = false;
    bool inOrderedList = false;
    int listItemNumber = 0;
    std::vector<bool> listOrderedStack;
    std::vector<int> listNumberStack;
    bool inTable = false;
    bool inTableHeader = false;
    std::vector<std::string> tableRow;
    std::vector<std::size_t> tableRowWidths;
    std::string tableCellBuf;
    std::size_t tableCellWidth = 0;
    std::vector<std::pair<std::vector<std::string>, bool>> tableRows;
    std::vector<std::size_t> tableColumnWidths;
    bool inParagraph = false;
    // Whether the paragraph segment in textBuf shows anything; block images split a paragraph into
    // segments, and a segment of bare whitespace or empty markup is dropped.
    bool paragraphHasText = false;
    // Markup spans open at the current position, reopened after a block image splits the paragraph.
    std::vector<MD_SPANTYPE> openSpans;
    int imageDepth = 0;
    std::string imageSrc;
    std::string imageAlt; // Pango-escaped
    std::size_t imageAltWidth = 0;
  };

  void escapeForPango(std::string& out, const char* text, unsigned size) {
    for (unsigned i = 0; i < size; ++i) {
      switch (text[i]) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      default:
        out += text[i];
        break;
      }
    }
  }

  const char* spanOpenTag(MD_SPANTYPE type) {
    switch (type) {
    case MD_SPAN_STRONG:
      return "<b>";
    case MD_SPAN_EM:
      return "<i>";
    case MD_SPAN_CODE:
      return "<tt>";
    case MD_SPAN_A:
      return "<u>";
    case MD_SPAN_DEL:
      return "<s>";
    default:
      return nullptr;
    }
  }

  const char* spanCloseTag(MD_SPANTYPE type) {
    switch (type) {
    case MD_SPAN_STRONG:
      return "</b>";
    case MD_SPAN_EM:
      return "</i>";
    case MD_SPAN_CODE:
      return "</tt>";
    case MD_SPAN_A:
      return "</u>";
    case MD_SPAN_DEL:
      return "</s>";
    default:
      return nullptr;
    }
  }

  constexpr int kWrapUnlimited = 500;

  std::unique_ptr<Label> makeMarkdownLabel(
      const std::string& text, float fontSize, float textScale, ColorRole color, FontWeight weight = FontWeight::Normal,
      bool markup = false, int maxLines = kWrapUnlimited, std::optional<float> maxWidth = std::nullopt,
      std::optional<float> flexGrow = std::nullopt
  ) {
    return ui::label({
        .text = text,
        .fontSize = fontSize * textScale,
        .fontWeight = weight,
        .color = colorSpecFromRole(color),
        .maxWidth = maxWidth,
        .maxLines = maxLines,
        .flexGrow = flexGrow,
        .configure = [markup](Label& label) { label.setUseMarkup(markup); },
    });
  }

  void emitHeading(MdContext& ctx) {
    float fontSize = Style::fontSizeBody;
    switch (ctx.headingLevel) {
    case 1:
      fontSize = Style::fontSizeHeader;
      break;
    case 2:
      fontSize = Style::fontSizeTitle;
      break;
    case 3:
      fontSize = Style::fontSizeBody * 1.1F;
      break;
    default:
      break;
    }
    ctx.view->addChild(ui::row({.height = Style::spaceSm * ctx.scale}));
    ctx.view->addChild(makeMarkdownLabel(
        ctx.textBuf, fontSize, ctx.scale * ctx.fontScale, ColorRole::Primary, FontWeight::Bold, true, 1
    ));
    ctx.view->addChild(ui::separator({.spacing = Style::spaceXs * ctx.scale * 0.5F}));
  }

  void emitParagraph(MdContext& ctx) {
    if (ctx.textBuf.empty()) {
      return;
    }
    auto label = makeMarkdownLabel(
        ctx.textBuf, Style::fontSizeBody, ctx.scale * ctx.fontScale, ColorRole::OnSurface, FontWeight::Normal, true
    );
    ctx.view->trackWrappableLabel(label.get());
    ctx.view->addChild(std::move(label));
  }

  // Emits the paragraph text before a block image and restarts the buffer with the markup spans
  // still open, so both segments stay balanced Pango markup.
  void flushParagraphSegment(MdContext& ctx) {
    if (ctx.paragraphHasText) {
      for (auto it = ctx.openSpans.rbegin(); it != ctx.openSpans.rend(); ++it) {
        ctx.textBuf += spanCloseTag(*it);
      }
      emitParagraph(ctx);
    }
    ctx.textBuf.clear();
    for (const MD_SPANTYPE type : ctx.openSpans) {
      ctx.textBuf += spanOpenTag(type);
    }
    ctx.paragraphHasText = false;
  }

  // Images directly in a paragraph become blocks of their own; anywhere else (headings, list items,
  // table cells) or when the view cannot load the reference, the alt text stands in inline.
  void emitImage(MdContext& ctx) {
    std::optional<std::string> path;
    if (ctx.inParagraph && !ctx.inTable) {
      path = ctx.view->resolveImage(ctx.imageSrc);
    }
    if (!path.has_value()) {
      if (ctx.inTable) {
        ctx.tableCellBuf += ctx.imageAlt;
        ctx.tableCellWidth += ctx.imageAltWidth;
      } else {
        ctx.textBuf += ctx.imageAlt;
        ctx.paragraphHasText = ctx.paragraphHasText || ctx.imageAltWidth > 0;
      }
      return;
    }
    flushParagraphSegment(ctx);
    ctx.view->addImage(std::move(ctx.imageSrc), *path);
  }

  void emitCodeBlock(MdContext& ctx) {
    while (!ctx.textBuf.empty() && ctx.textBuf.back() == '\n') {
      ctx.textBuf.pop_back();
    }
    if (ctx.textBuf.empty()) {
      return;
    }
    const float pad = Style::spaceSm * ctx.scale;
    auto block = ui::column({
        .align = FlexAlign::Start,
        .padding = pad,
        .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow),
        .radius = Style::scaledRadiusSm(ctx.scale),
        .fillWidth = true,
    });
    block->addChild(
        ui::label({
            .text = ctx.textBuf,
            .fontSize = Style::fontSizeCaption * ctx.scale * ctx.fontScale,
            .fontFamily = std::string("monospace"),
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = kWrapUnlimited,
            .textAlign = TextAlign::Start,
        })
    );
    ctx.view->addChild(std::move(block));
  }

  std::size_t codepointCount(const char* text, unsigned size) {
    return static_cast<std::size_t>(std::count_if(text, text + size, [](char byte) {
      return (static_cast<unsigned char>(byte) & 0xC0U) != 0x80U;
    }));
  }

  void emitTableRow(MdContext& ctx) {
    if (ctx.tableRow.empty()) {
      return;
    }
    if (ctx.tableColumnWidths.size() < ctx.tableRow.size()) {
      ctx.tableColumnWidths.resize(ctx.tableRow.size(), 0);
    }
    for (std::size_t i = 0; i < ctx.tableRow.size(); ++i) {
      const std::size_t width = i < ctx.tableRowWidths.size() ? ctx.tableRowWidths[i] : 0;
      ctx.tableColumnWidths[i] = std::max(ctx.tableColumnWidths[i], width);
    }
    ctx.tableRows.emplace_back(ctx.tableRow, ctx.inTableHeader);
    ctx.tableRow.clear();
    ctx.tableRowWidths.clear();
  }

  void flushTable(MdContext& ctx) {
    if (ctx.tableRows.empty()) {
      return;
    }
    const float borderWidth = std::max(1.0F, Style::borderWidth * ctx.scale);
    const float paddingH = Style::spaceSm * ctx.scale;
    const float paddingV = Style::spaceXs * ctx.scale;
    const float characterWidth = Style::fontSizeCaption * ctx.scale * ctx.fontScale * 0.62F;
    std::vector<float> columnWidths;
    columnWidths.reserve(ctx.tableColumnWidths.size());
    for (const std::size_t characters : ctx.tableColumnWidths) {
      columnWidths.push_back(
          std::clamp(
              static_cast<float>(characters) * characterWidth + paddingH * 2.0F, 88.0F * ctx.scale, 360.0F * ctx.scale
          )
      );
    }

    auto scroll = ui::scrollView({
        .contentScale = ctx.scale,
        .viewportPaddingH = 0.0F,
        .viewportPaddingV = 0.0F,
        .fillWidth = true,
        .configure = [](ScrollView& view) {
          view.setOrientation(ScrollOrientation::Horizontal);
          view.content()->setAlign(FlexAlign::Stretch);
        },
    });

    const float totalWidth = std::max(
        0.0F,
        std::accumulate(columnWidths.begin(), columnWidths.end(), 0.0F)
            + borderWidth * static_cast<float>(columnWidths.size() + 1)
    );
    auto table = ui::column({
        .align = FlexAlign::Stretch,
        .gap = borderWidth,
        .padding = borderWidth,
        .fill = colorSpecFromRole(ColorRole::OutlineVariant),
        .radius = Style::scaledRadiusSm(ctx.scale),
        .minWidth = totalWidth,
        .clipChildren = true,
    });

    std::size_t bodyRowIndex = 0;
    for (std::size_t rowIndex = 0; rowIndex < ctx.tableRows.size(); ++rowIndex) {
      const auto& [cells, isHeader] = ctx.tableRows[rowIndex];
      const ColorSpec background = isHeader
          ? colorSpecFromRole(ColorRole::SurfaceContainer)
          : (bodyRowIndex % 2 == 0 ? colorSpecFromRole(ColorRole::Surface)
                                   : colorSpecFromRole(ColorRole::SurfaceContainerLow));
      auto row = ui::row({
          .align = FlexAlign::Stretch,
          .gap = 0.0F,
          .fill = colorSpecFromRole(ColorRole::Surface),
          .fillWidth = true,
      });
      for (std::size_t i = 0; i < columnWidths.size(); ++i) {
        if (i > 0) {
          row->addChild(
              ui::separator({
                  .color = colorSpecFromRole(ColorRole::OutlineVariant),
                  .thickness = borderWidth,
                  .orientation = SeparatorOrientation::VerticalRule,
                  .gradientEdges = false,
              })
          );
        }
        auto cell = ui::column({
            .align = FlexAlign::Stretch,
            .paddingV = paddingV,
            .paddingH = paddingH,
            .fill = background,
            .minWidth = columnWidths[i],
            .flexGrow = columnWidths[i],
        });

        auto label = makeMarkdownLabel(
            i < cells.size() ? cells[i] : std::string{}, Style::fontSizeCaption, ctx.scale * ctx.fontScale,
            isHeader ? ColorRole::OnSurfaceVariant : ColorRole::OnSurface,
            isHeader ? FontWeight::Bold : FontWeight::Normal, true, kWrapUnlimited,
            std::max(0.0F, columnWidths[i] - paddingH * 2.0F)
        );
        cell->addChild(std::move(label));
        row->addChild(std::move(cell));
      }
      table->addChild(std::move(row));
      if (!isHeader) {
        ++bodyRowIndex;
      }
    }

    scroll->content()->addChild(std::move(table));
    ctx.view->addChild(std::move(scroll));
    ctx.tableRows.clear();
    ctx.tableColumnWidths.clear();
  }

  void emitListItem(MdContext& ctx) {
    if (ctx.textBuf.empty()) {
      return;
    }
    auto row = ui::row({
        .align = FlexAlign::Start,
        .gap = Style::spaceXs * ctx.scale,
        .fillWidth = true,
    });
    std::string bullet;
    if (!ctx.listOrderedStack.empty() && ctx.listOrderedStack.back()) {
      bullet = std::to_string(ctx.listItemNumber) + ".";
    } else {
      bullet = "•";
    }
    row->addChild(makeMarkdownLabel(
        bullet, Style::fontSizeBody, ctx.scale * ctx.fontScale, ColorRole::OnSurfaceVariant, FontWeight::Normal
    ));
    auto textLabel = makeMarkdownLabel(
        ctx.textBuf, Style::fontSizeBody, ctx.scale * ctx.fontScale, ColorRole::OnSurface, FontWeight::Normal, true,
        kWrapUnlimited, std::nullopt, 1.0F
    );
    ctx.view->trackWrappableLabel(textLabel.get());
    row->addChild(std::move(textLabel));
    const float indent = Style::spaceMd * ctx.scale * static_cast<float>(ctx.listOrderedStack.size() - 1);
    if (indent > 0.0F) {
      row->setPadding(0.0F, 0.0F, 0.0F, indent);
    }
    ctx.view->addChild(std::move(row));
  }

  int onEnterBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
    auto* ctx = static_cast<MdContext*>(userdata);
    ctx->textBuf.clear();
    switch (type) {
    case MD_BLOCK_P:
      ctx->inParagraph = true;
      ctx->paragraphHasText = false;
      ctx->openSpans.clear();
      break;
    case MD_BLOCK_H: {
      const auto* hd = static_cast<const MD_BLOCK_H_DETAIL*>(detail);
      ctx->headingLevel = static_cast<int>(hd->level);
      break;
    }
    case MD_BLOCK_CODE:
      ctx->inCodeBlock = true;
      break;
    case MD_BLOCK_UL:
      ctx->listOrderedStack.push_back(false);
      ctx->listNumberStack.push_back(0);
      break;
    case MD_BLOCK_OL: {
      const auto* od = static_cast<const MD_BLOCK_OL_DETAIL*>(detail);
      ctx->listOrderedStack.push_back(true);
      ctx->listNumberStack.push_back(static_cast<int>(od->start) - 1);
      break;
    }
    case MD_BLOCK_LI:
      if (!ctx->listNumberStack.empty()) {
        ctx->listNumberStack.back()++;
        ctx->listItemNumber = ctx->listNumberStack.back();
      }
      ctx->textBuf.clear();
      break;
    case MD_BLOCK_TABLE:
      ctx->inTable = true;
      break;
    case MD_BLOCK_THEAD:
      ctx->inTableHeader = true;
      break;
    case MD_BLOCK_TBODY:
      ctx->inTableHeader = false;
      break;
    case MD_BLOCK_TD:
    case MD_BLOCK_TH:
      ctx->tableCellBuf.clear();
      ctx->tableCellWidth = 0;
      break;
    default:
      break;
    }
    return 0;
  }

  int onLeaveBlock(MD_BLOCKTYPE type, void* /*detail*/, void* userdata) {
    auto* ctx = static_cast<MdContext*>(userdata);
    switch (type) {
    case MD_BLOCK_H:
      emitHeading(*ctx);
      ctx->headingLevel = 0;
      break;
    case MD_BLOCK_P:
      if (ctx->paragraphHasText) {
        emitParagraph(*ctx);
      }
      ctx->inParagraph = false;
      break;
    case MD_BLOCK_CODE:
      emitCodeBlock(*ctx);
      ctx->inCodeBlock = false;
      break;
    case MD_BLOCK_HR:
      ctx->view->addChild(ui::separator({.spacing = Style::spaceXs * ctx->scale}));
      break;
    case MD_BLOCK_LI:
      emitListItem(*ctx);
      break;
    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
      if (!ctx->listOrderedStack.empty()) {
        ctx->listOrderedStack.pop_back();
      }
      if (!ctx->listNumberStack.empty()) {
        ctx->listNumberStack.pop_back();
      }
      break;
    case MD_BLOCK_TABLE:
      flushTable(*ctx);
      ctx->inTable = false;
      break;
    case MD_BLOCK_THEAD:
      ctx->inTableHeader = false;
      break;
    case MD_BLOCK_TR:
      emitTableRow(*ctx);
      break;
    case MD_BLOCK_TD:
    case MD_BLOCK_TH:
      ctx->tableRow.push_back(ctx->tableCellBuf);
      ctx->tableRowWidths.push_back(ctx->tableCellWidth);
      ctx->tableCellBuf.clear();
      ctx->tableCellWidth = 0;
      break;
    default:
      break;
    }
    if (!ctx->inTable) {
      ctx->textBuf.clear();
    }
    return 0;
  }

  int onEnterSpan(MD_SPANTYPE type, void* detail, void* userdata) {
    auto* ctx = static_cast<MdContext*>(userdata);
    if (ctx->inCodeBlock) {
      return 0;
    }
    if (type == MD_SPAN_IMG) {
      // Nested images only contribute to the outer image's alt text.
      if (ctx->imageDepth++ == 0) {
        const auto* img = static_cast<const MD_SPAN_IMG_DETAIL*>(detail);
        ctx->imageSrc.assign(img->src.text, img->src.size);
        ctx->imageAlt.clear();
        ctx->imageAltWidth = 0;
      }
      return 0;
    }
    if (ctx->imageDepth > 0) {
      return 0;
    }
    const char* tag = spanOpenTag(type);
    if (tag == nullptr) {
      return 0;
    }
    (ctx->inTable ? ctx->tableCellBuf : ctx->textBuf) += tag;
    ctx->openSpans.push_back(type);
    return 0;
  }

  int onLeaveSpan(MD_SPANTYPE type, void* /*detail*/, void* userdata) {
    auto* ctx = static_cast<MdContext*>(userdata);
    if (ctx->inCodeBlock) {
      return 0;
    }
    if (type == MD_SPAN_IMG) {
      if (--ctx->imageDepth == 0) {
        emitImage(*ctx);
      }
      return 0;
    }
    if (ctx->imageDepth > 0) {
      return 0;
    }
    const char* tag = spanCloseTag(type);
    if (tag == nullptr) {
      return 0;
    }
    (ctx->inTable ? ctx->tableCellBuf : ctx->textBuf) += tag;
    if (!ctx->openSpans.empty()) {
      ctx->openSpans.pop_back();
    }
    return 0;
  }

  int onText(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    auto* ctx = static_cast<MdContext*>(userdata);
    if (ctx->imageDepth > 0) {
      switch (type) {
      case MD_TEXT_SOFTBR:
      case MD_TEXT_BR:
        ctx->imageAlt += ' ';
        ++ctx->imageAltWidth;
        break;
      case MD_TEXT_ENTITY:
        ctx->imageAlt.append(text, size);
        ++ctx->imageAltWidth;
        break;
      default:
        escapeForPango(ctx->imageAlt, text, size);
        ctx->imageAltWidth += codepointCount(text, size);
        break;
      }
      return 0;
    }
    if (ctx->inParagraph && !ctx->paragraphHasText) {
      // A segment following a block image starts at its first visible character.
      if (type == MD_TEXT_SOFTBR || type == MD_TEXT_BR) {
        return 0;
      }
      while (size > 0 && (*text == ' ' || *text == '\t')) {
        ++text;
        --size;
      }
      if (size == 0) {
        return 0;
      }
      ctx->paragraphHasText = true;
    }
    auto& buf = ctx->inTable ? ctx->tableCellBuf : ctx->textBuf;
    const auto appendEscaped = [&] {
      escapeForPango(buf, text, size);
      if (ctx->inTable) {
        ctx->tableCellWidth += codepointCount(text, size);
      }
    };
    const auto appendRaw = [&] {
      buf.append(text, size);
      if (ctx->inTable) {
        ctx->tableCellWidth += codepointCount(text, size);
      }
    };
    switch (type) {
    case MD_TEXT_NORMAL:
    case MD_TEXT_CODE:
      if (ctx->inCodeBlock) {
        appendRaw();
      } else {
        appendEscaped();
      }
      break;
    case MD_TEXT_SOFTBR:
      buf += ' ';
      if (ctx->inTable) {
        ++ctx->tableCellWidth;
      }
      break;
    case MD_TEXT_BR:
      buf += '\n';
      break;
    case MD_TEXT_ENTITY:
      buf.append(text, size);
      if (ctx->inTable) {
        ++ctx->tableCellWidth;
      }
      break;
    default:
      if (ctx->inCodeBlock) {
        appendRaw();
      } else {
        appendEscaped();
      }
      break;
    }
    return 0;
  }

  // Images show at their pixel size times the UI scale, shrunk to the view width and this height.
  constexpr float kMaxImageHeight = 480.0F;
  // Decode bound for the longer image side, in logical pixels before the UI scale.
  constexpr float kImageTargetSize = 1024.0F;

} // namespace

void MarkdownView::setImageSource(MarkdownImageSource source) { m_imageSource = std::move(source); }

void MarkdownView::setMarkdown(const std::string& markdown, float scale, float fontScale) {
  clear();
  m_scale = scale;
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(Style::spaceSm * scale);
  setFillWidth(true);

  MdContext ctx;
  ctx.view = this;
  ctx.scale = scale;
  ctx.fontScale = fontScale;

  MD_PARSER parser = {};
  parser.abi_version = 0;
  parser.flags = MD_DIALECT_GITHUB | MD_FLAG_NOHTML;
  parser.enter_block = onEnterBlock;
  parser.leave_block = onLeaveBlock;
  parser.enter_span = onEnterSpan;
  parser.leave_span = onLeaveSpan;
  parser.text = onText;

  md_parse(markdown.c_str(), static_cast<MD_SIZE>(markdown.size()), &parser, &ctx);
}

void MarkdownView::clear() {
  m_wrappableLabels.clear();
  m_images.clear();
  while (!children().empty()) {
    removeChild(children().back().get());
  }
}

std::optional<std::string> MarkdownView::resolveImage(const std::string& src) const {
  if (m_imageSource.renderer == nullptr || !m_imageSource.resolve || src.empty()) {
    return std::nullopt;
  }
  return m_imageSource.resolve(src);
}

void MarkdownView::addImage(std::string src, const std::string& path) {
  Flex* row = nullptr;
  Image* image = nullptr;
  addChild(
      ui::row(
          {.out = &row, .align = FlexAlign::Start, .fillWidth = true},
          ui::image({.out = &image, .fit = ImageFit::Contain, .radius = Style::scaledRadiusMd(m_scale)})
      )
  );
  // Hidden until the texture arrives, so a pending or undecodable image takes no gap in the column.
  row->setVisible(false);
  auto& entry = m_images.emplace_back(ImageEntry{.row = row, .image = image, .src = std::move(src)});
  loadImage(entry, path);
}

void MarkdownView::refreshPendingImages() {
  bool loaded = false;
  for (auto& entry : m_images) {
    if (!entry.pending) {
      continue;
    }
    const auto path = resolveImage(entry.src);
    if (!path.has_value() || path->empty()) {
      continue;
    }
    loadImage(entry, *path);
    loaded = loaded || entry.row->visible();
  }
  if (loaded) {
    markLayoutDirty();
    if (m_imageSource.onLayoutNeeded) {
      m_imageSource.onLayoutNeeded();
    }
  }
}

void MarkdownView::loadImage(ImageEntry& entry, const std::string& path) {
  entry.pending = path.empty();
  if (entry.pending) {
    return;
  }
  Renderer& renderer = *m_imageSource.renderer;
  const int targetSize = static_cast<int>(std::ceil(kImageTargetSize * m_scale));
  bool loaded = false;
  if (m_imageSource.textureCache != nullptr) {
    loaded = entry.image->setSourceFileAsync(renderer, *m_imageSource.textureCache, path, targetSize, true);
    if (!loaded) {
      // The image is a child of this view, so the callback cannot outlive it.
      entry.image->setAsyncReadyCallback([this, row = entry.row] {
        row->setVisible(true);
        markLayoutDirty();
        if (m_imageSource.onLayoutNeeded) {
          m_imageSource.onLayoutNeeded();
        }
      });
    }
  } else {
    loaded = entry.image->setSourceFile(renderer, path, targetSize, true);
  }
  entry.row->setVisible(loaded);
}

void MarkdownView::applyWidth(float width) {
  for (Label* label : m_wrappableLabels) {
    label->setMaxWidth(width);
  }
  const float maxHeight = kMaxImageHeight * m_scale;
  for (const auto& entry : m_images) {
    Image& image = *entry.image;
    if (!image.hasImage()) {
      continue;
    }
    const float aspect = image.aspectRatio();
    float imageWidth = width;
    if (image.sourceWidth() > 0) {
      imageWidth = std::min(width, static_cast<float>(image.sourceWidth()) * m_scale);
    }
    float imageHeight = imageWidth / aspect;
    if (imageHeight > maxHeight) {
      imageHeight = maxHeight;
      imageWidth = imageHeight * aspect;
    }
    image.setSize(imageWidth, imageHeight);
  }
}

LayoutSize MarkdownView::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  // Apply the wrap width before measuring: labels otherwise report single-line
  // sizes, the parent allocates too little height for the view, and sibling
  // rows overlap it. doLayout re-applies the final arranged width.
  float w = width();
  if (constraints.hasMaxWidth && constraints.maxWidth > 0.0F) {
    w = constraints.maxWidth;
  }
  if (w > 0.0F) {
    applyWidth(w);
  }
  return Flex::doMeasure(renderer, constraints);
}

void MarkdownView::doLayout(Renderer& renderer) {
  const float w = width();
  if (w > 0.0F) {
    applyWidth(w);
  }
  Flex::doLayout(renderer);
}
