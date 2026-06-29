#include "ui/controls/markdown_view.h"

#include "ui/builders.h"
#include "ui/controls/label.h"
#include "ui/controls/separator.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <cstdint>
#include <md4c.h>
#include <memory>
#include <string>
#include <vector>

namespace {

  struct MdContext {
    MarkdownView* view = nullptr;
    float scale = 1.0f;
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
    std::string tableCellBuf;
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
      bool markup = false, int maxLines = kWrapUnlimited
  ) {
    auto label = ui::label({
        .text = text,
        .fontSize = fontSize * scale,
        .color = colorSpecFromRole(color),
        .maxLines = maxLines,
        .fontWeight = weight,
    });
    if (markup) {
      label->setUseMarkup(true);
    }
    return label;
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
      fontSize = Style::fontSizeBody * 1.1f;
      break;
    default:
      break;
    }
    ctx.view->addChild(ui::row({.height = Style::spaceSm * ctx.scale}));
    ctx.view->addChild(
        makeMarkdownLabel(ctx.textBuf, fontSize, ctx.scale, ColorRole::OnSurface, FontWeight::Bold, false, 1)
    );
    ctx.view->addChild(ui::separator({.spacing = Style::spaceXs * ctx.scale * 0.5f}));
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
        .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.5f),
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

  void emitTableRow(MdContext& ctx) {
    if (ctx.tableRow.empty()) {
      return;
    }
    if (ctx.tableColumnWidths.empty()) {
      ctx.tableColumnWidths.resize(ctx.tableRow.size(), 0);
    }
    for (std::size_t i = 0; i < ctx.tableRow.size() && i < ctx.tableColumnWidths.size(); ++i) {
      ctx.tableColumnWidths[i] = std::max(ctx.tableColumnWidths[i], ctx.tableRow[i].size());
    }
    ctx.tableRows.emplace_back(ctx.tableRow, ctx.inTableHeader);
    ctx.tableRow.clear();
  }

  void flushTable(MdContext& ctx) {
    if (ctx.tableRows.empty()) {
      return;
    }
    std::string block;
    for (const auto& [cells, isHeader] : ctx.tableRows) {
      std::string line;
      for (std::size_t i = 0; i < cells.size(); ++i) {
        if (i > 0) {
          line += "  ";
        }
        line += cells[i];
        if (i < ctx.tableColumnWidths.size() && i + 1 < cells.size()) {
          const auto pad = ctx.tableColumnWidths[i] - cells[i].size();
          line.append(pad, ' ');
        }
      }
      if (!block.empty()) {
        block += '\n';
      }
      block += line;
    }
    auto label = ui::label({
        .text = block,
        .fontSize = Style::fontSizeCaption * ctx.scale,
        .fontFamily = std::string("monospace"),
        .color = colorSpecFromRole(ColorRole::OnSurface),
        .maxLines = kWrapUnlimited,
        .textAlign = TextAlign::Start,
    });
    label->setFlexGrow(1.0f);
    ctx.view->addChild(std::move(label));
    ctx.tableRows.clear();
    ctx.tableColumnWidths.clear();
  }

  void emitListItem(MdContext& ctx) {
    if (ctx.textBuf.empty()) {
      return;
    }
    auto row = ui::row({.align = FlexAlign::Start, .gap = Style::spaceXs * ctx.scale});
    std::string bullet;
    if (!ctx.listOrderedStack.empty() && ctx.listOrderedStack.back()) {
      bullet = std::to_string(ctx.listItemNumber) + ".";
    } else {
      bullet = "•";
    }
    row->addChild(
        makeMarkdownLabel(bullet, Style::fontSizeBody, ctx.scale, ColorRole::OnSurfaceVariant, FontWeight::Normal)
    );
    auto textLabel =
        makeMarkdownLabel(ctx.textBuf, Style::fontSizeBody, ctx.scale, ColorRole::OnSurface, FontWeight::Normal, true);
    ctx.view->trackWrappableLabel(textLabel.get());
    textLabel->setFlexGrow(1.0f);
    row->addChild(std::move(textLabel));
    row->setFillWidth(true);
    const float indent = Style::spaceMd * ctx.scale * static_cast<float>(ctx.listOrderedStack.size() - 1);
    if (indent > 0.0f) {
      row->setPadding(0.0f, 0.0f, 0.0f, indent);
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
      ctx->tableCellBuf.clear();
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
    switch (type) {
    case MD_SPAN_STRONG:
      ctx->textBuf += "<b>";
      break;
    case MD_SPAN_EM:
      ctx->textBuf += "<i>";
      break;
    case MD_SPAN_CODE:
      ctx->textBuf += "<tt>";
      break;
    case MD_SPAN_A:
      ctx->textBuf += "<u>";
      break;
    case MD_SPAN_DEL:
      ctx->textBuf += "<s>";
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
    switch (type) {
    case MD_SPAN_STRONG:
      ctx->textBuf += "</b>";
      break;
    case MD_SPAN_EM:
      ctx->textBuf += "</i>";
      break;
    case MD_SPAN_CODE:
      ctx->textBuf += "</tt>";
      break;
    case MD_SPAN_A:
      ctx->textBuf += "</u>";
      break;
    case MD_SPAN_DEL:
      ctx->textBuf += "</s>";
      break;
    default:
      break;
    }
    return 0;
  }

  int onText(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    auto* ctx = static_cast<MdContext*>(userdata);
    auto& buf = ctx->inTable ? ctx->tableCellBuf : ctx->textBuf;
    switch (type) {
    case MD_TEXT_NORMAL:
      if (ctx->inCodeBlock) {
        buf.append(text, size);
      } else if (ctx->inTable) {
        buf.append(text, size);
      } else {
        escapeForPango(buf, text, size);
      }
      break;
    case MD_TEXT_CODE:
      buf.append(text, size);
      break;
    case MD_TEXT_SOFTBR:
      buf += ' ';
      break;
    case MD_TEXT_BR:
      buf += '\n';
      break;
    case MD_TEXT_ENTITY:
      buf.append(text, size);
      break;
    default:
      buf.append(text, size);
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

void MarkdownView::doLayout(Renderer& renderer) {
  const float w = width();
  if (w > 0.0f) {
    for (Label* label : m_wrappableLabels) {
      label->setMaxWidth(w);
    }
  }
  Flex::doLayout(renderer);
}
