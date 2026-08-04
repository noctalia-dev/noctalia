#include "ui/controls/markdown_view.h"

#include "ui/builders.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/separator.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <md4c.h>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

namespace {

  struct MdContext {
    MarkdownView* view = nullptr;
    float scale = 1.0F;
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

  constexpr int kWrapUnlimited = 500;

  std::unique_ptr<Label> makeMarkdownLabel(
      const std::string& text, float fontSize, float scale, ColorRole color, FontWeight weight = FontWeight::Normal,
      bool markup = false, int maxLines = kWrapUnlimited, std::optional<float> maxWidth = std::nullopt,
      std::optional<float> flexGrow = std::nullopt
  ) {
    return ui::label({
        .text = text,
        .fontSize = fontSize * scale,
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
    ctx.view->addChild(
        makeMarkdownLabel(ctx.textBuf, fontSize, ctx.scale, ColorRole::Primary, FontWeight::Bold, true, 1)
    );
    ctx.view->addChild(ui::separator({.spacing = Style::spaceXs * ctx.scale * 0.5F}));
  }

  void emitParagraph(MdContext& ctx) {
    if (ctx.textBuf.empty()) {
      return;
    }
    auto label =
        makeMarkdownLabel(ctx.textBuf, Style::fontSizeBody, ctx.scale, ColorRole::OnSurface, FontWeight::Normal, true);
    ctx.view->trackWrappableLabel(label.get());
    ctx.view->addChild(std::move(label));
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
        .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.5F),
        .radius = Style::scaledRadiusSm(ctx.scale),
        .fillWidth = true,
    });
    block->addChild(
        ui::label({
            .text = ctx.textBuf,
            .fontSize = Style::fontSizeCaption * ctx.scale,
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
    const float characterWidth = Style::fontSizeCaption * ctx.scale * 0.62F;
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
        .fill = colorSpecFromRole(ColorRole::Outline),
        .radius = Style::scaledRadiusSm(ctx.scale),
        .minWidth = totalWidth,
        .clipChildren = true,
    });

    std::size_t bodyRowIndex = 0;
    for (std::size_t rowIndex = 0; rowIndex < ctx.tableRows.size(); ++rowIndex) {
      const auto& [cells, isHeader] = ctx.tableRows[rowIndex];
      const ColorSpec background = isHeader
          ? colorSpecFromRole(ColorRole::SurfaceVariant)
          : (bodyRowIndex % 2 == 0 ? colorSpecFromRole(ColorRole::Surface)
                                   : colorSpecFromRole(ColorRole::SurfaceVariant, 0.5F));
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
                  .color = colorSpecFromRole(ColorRole::Outline),
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
            i < cells.size() ? cells[i] : std::string{}, Style::fontSizeCaption, ctx.scale,
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
    row->addChild(
        makeMarkdownLabel(bullet, Style::fontSizeBody, ctx.scale, ColorRole::OnSurfaceVariant, FontWeight::Normal)
    );
    auto textLabel = makeMarkdownLabel(
        ctx.textBuf, Style::fontSizeBody, ctx.scale, ColorRole::OnSurface, FontWeight::Normal, true, kWrapUnlimited,
        std::nullopt, 1.0F
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
      emitParagraph(*ctx);
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

  int onEnterSpan(MD_SPANTYPE type, void* /*detail*/, void* userdata) {
    auto* ctx = static_cast<MdContext*>(userdata);
    if (ctx->inCodeBlock) {
      return 0;
    }
    auto& buf = ctx->inTable ? ctx->tableCellBuf : ctx->textBuf;
    switch (type) {
    case MD_SPAN_STRONG:
      buf += "<b>";
      break;
    case MD_SPAN_EM:
      buf += "<i>";
      break;
    case MD_SPAN_CODE:
      buf += "<tt>";
      break;
    case MD_SPAN_A:
      buf += "<u>";
      break;
    case MD_SPAN_DEL:
      buf += "<s>";
      break;
    default:
      break;
    }
    return 0;
  }

  int onLeaveSpan(MD_SPANTYPE type, void* /*detail*/, void* userdata) {
    auto* ctx = static_cast<MdContext*>(userdata);
    if (ctx->inCodeBlock) {
      return 0;
    }
    auto& buf = ctx->inTable ? ctx->tableCellBuf : ctx->textBuf;
    switch (type) {
    case MD_SPAN_STRONG:
      buf += "</b>";
      break;
    case MD_SPAN_EM:
      buf += "</i>";
      break;
    case MD_SPAN_CODE:
      buf += "</tt>";
      break;
    case MD_SPAN_A:
      buf += "</u>";
      break;
    case MD_SPAN_DEL:
      buf += "</s>";
      break;
    default:
      break;
    }
    return 0;
  }

  int onText(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    auto* ctx = static_cast<MdContext*>(userdata);
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
      if (ctx->inCodeBlock) {
        appendRaw();
      } else {
        appendEscaped();
      }
      break;
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

} // namespace

void MarkdownView::setMarkdown(const std::string& markdown, float scale) {
  clear();
  m_scale = scale;
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(Style::spaceSm * scale);
  setFillWidth(true);

  MdContext ctx;
  ctx.view = this;
  ctx.scale = scale;

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
  while (!children().empty()) {
    removeChild(children().back().get());
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
    for (Label* label : m_wrappableLabels) {
      label->setMaxWidth(w);
    }
  }
  return Flex::doMeasure(renderer, constraints);
}

void MarkdownView::doLayout(Renderer& renderer) {
  const float w = width();
  if (w > 0.0F) {
    for (Label* label : m_wrappableLabels) {
      label->setMaxWidth(w);
    }
  }
  Flex::doLayout(renderer);
}
