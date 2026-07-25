#include "i18n/i18n_service.h"
#include "launcher/math_provider.h"
#include "net/http_client.h"
#include "wayland/clipboard_service.h"

#include <cstdio>
#include <filesystem>
#include <print>
#include <string>
#include <string_view>

namespace i18n {

  Service& Service::instance() {
    static Service service;
    return service;
  }

  std::string_view Service::lookup(std::string_view) const { return {}; }

} // namespace i18n

void HttpClient::download(std::string_view, const std::filesystem::path&, CompletionCallback) {}

bool ClipboardService::copyText(std::string) { return false; }

namespace {

  bool expect(bool condition, std::string_view message) {
    if (!condition) {
      std::println(stderr, "math_provider_test: {}", message);
    }
    return condition;
  }

} // namespace

int main() {
  MathProvider provider(nullptr, nullptr, nullptr);
  provider.initialize();

  bool ok = true;
  ok = expect(provider.query("EUR").empty(), "digit-free global query should be filtered") && ok;
  ok = expect(!provider.queryPrefixed("EUR").empty(), "digit-free prefixed query should be evaluated") && ok;
  ok = expect(!provider.query("2 + 2").empty(), "numeric global query should be evaluated") && ok;

  return ok ? 0 : 1;
}
