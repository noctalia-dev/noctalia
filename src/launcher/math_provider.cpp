#include "launcher/math_provider.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "net/http_client.h"
#include "wayland/clipboard_service.h"

#include <cctype>
#include <filesystem>
#include <libqalculate/Calculator.h>
#include <memory>
#include <string>

namespace {

  // Cheap pre-filter: the provider participates in global search on every keystroke,
  // so reject anything without a digit to avoid evaluating plain search text
  // (e.g. "firefox"). Letters/spaces are kept so unit and currency conversions
  // like "10 cm to in" or "5 USD to EUR" still reach libqalculate.
  bool looksLikeMath(std::string_view text) {
    for (char c : text) {
      if (c >= '0' && c <= '9') {
        return true;
      }
    }
    return false;
  }

  std::string trimmed(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
      ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
      --end;
    }
    return std::string(text.substr(begin, end - begin));
  }

  bool shouldRefreshExchangeRateSource(std::string_view url) { return !url.contains("nbrb.by"); }

} // namespace

MathProvider::MathProvider(ClipboardService* clipboard, ConfigService* config, HttpClient* httpClient)
    : m_clipboard(clipboard), m_config(config), m_httpClient(httpClient) {}

MathProvider::~MathProvider() = default;

std::string MathProvider::displayName() const { return i18n::tr("launcher.providers.calculator.title"); }

void MathProvider::initialize() {
  m_calc = std::make_unique<Calculator>();
  // Load any cached rates before definitions so currency units pick them up.
  m_calc->loadExchangeRates();
  m_calc->loadGlobalDefinitions();
  // Pre-warm internal state (GMP randstate) so the destructor path is safe.
  m_calc->calculate("0");
  m_calc->clearMessages();
  refreshExchangeRates();
}

void MathProvider::refreshExchangeRates() {
  if (m_httpClient == nullptr || m_config == nullptr || !m_calc) {
    return;
  }
  if (m_config->config().shell.offlineMode
      || !m_config->config().shell.launcher.fetchExchangeRates
      || !m_calc->canFetch()) {
    return;
  }

  std::vector<std::pair<std::string, std::filesystem::path>> sources;
  for (int i = 1;; ++i) {
    std::string url = m_calc->getExchangeRatesUrl(i);
    std::string file = m_calc->getExchangeRatesFileName(i);
    if (url.empty() || file.empty()) {
      break;
    }
    if (!shouldRefreshExchangeRateSource(url)) {
      continue;
    }
    sources.emplace_back(std::move(url), std::filesystem::path(std::move(file)));
  }
  if (sources.empty()) {
    return;
  }

  auto remaining = std::make_shared<int>(static_cast<int>(sources.size()));
  for (auto& [url, path] : sources) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    m_httpClient->download(url, path, [this, remaining](bool) {
      if (--(*remaining) == 0) {
        // All sources settled; reload whatever landed on disc.
        m_calc->loadExchangeRates();
      }
    });
  }
}

std::vector<LauncherResult> MathProvider::query(std::string_view text) const {
  if (!looksLikeMath(text)) {
    return {};
  }
  return evaluate(text);
}

std::vector<LauncherResult> MathProvider::queryPrefixed(std::string_view text) const { return evaluate(text); }

std::vector<LauncherResult> MathProvider::evaluate(std::string_view text) const {
  std::string input = trimmed(text);
  if (!m_calc || input.empty()) {
    return {};
  }

  // Drop any messages left over from a previous evaluation.
  m_calc->clearMessages();

  EvaluationOptions eo = default_user_evaluation_options;
  PrintOptions po = default_print_options;
  po.use_unicode_signs = false;
  // Collapse interval arithmetic to a single rounded value instead of "interval(a, b)".
  po.interval_display = INTERVAL_DISPLAY_SIGNIFICANT_DIGITS;

  std::string output = m_calc->calculateAndPrint(input, /*msecs=*/200, eo, po);

  bool hadError = false;
  for (CalculatorMessage* m = m_calc->message(); m != nullptr; m = m_calc->nextMessage()) {
    if (m->type() == MESSAGE_ERROR) {
      hadError = true;
    }
  }

  // Reject errors and no-ops (e.g. the user just typed a bare number).
  if (hadError || output.empty() || output == input) {
    return {};
  }

  LauncherResult r;
  r.id = "math";
  r.title = "= " + output;
  r.subtitle = std::string(text);
  r.glyphName = "calculator";
  r.score = 10000;

  return {std::move(r)};
}

bool MathProvider::activate(const LauncherResult& result) {
  if (result.id != "math") {
    return false;
  }

  std::string value = result.title.substr(2);
  return m_clipboard != nullptr && m_clipboard->copyText(std::move(value));
}
