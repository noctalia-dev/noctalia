#include "theme/template_engine.h"

#include "core/log.h"
#include "core/process/process.h"
#include "core/toml.h" // IWYU pragma: keep
#include "cpp/cam/hct.h"
#include "cpp/palettes/tones.h"
#include "cpp/scheme/scheme_content.h"
#include "cpp/scheme/scheme_fruit_salad.h"
#include "cpp/scheme/scheme_monochrome.h"
#include "cpp/scheme/scheme_rainbow.h"
#include "cpp/scheme/scheme_tonal_spot.h"
#include "theme/color.h"
#include "theme/firefox_theme/firefox_theme.h"
#include "theme/kde_color_scheme.h"
#include "theme/palette.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace noctalia::theme {

  namespace {

    constexpr Logger kLog("template_engine");

    struct RichColor {
      Color color;
      double alpha = 1.0;
    };

    struct ScopeValue;
    using ScopeArray = std::vector<ScopeValue>;
    using ScopeMap = std::unordered_map<std::string, ScopeValue>;

    struct ScopeValue {
      using Storage = std::variant<std::monostate, bool, double, std::string, RichColor, ScopeArray, ScopeMap>;
      Storage value;

      ScopeValue() = default;
      ScopeValue(bool v) : value(v) {}
      ScopeValue(double v) : value(v) {}
      ScopeValue(int v) : value(static_cast<double>(v)) {}
      ScopeValue(std::string v) : value(std::move(v)) {}
      ScopeValue(const char* v) : value(std::string(v)) {}
      ScopeValue(RichColor v) : value(v) {}
      ScopeValue(ScopeArray v) : value(std::move(v)) {}
      ScopeValue(ScopeMap v) : value(std::move(v)) {}
    };

    struct TextNode {
      std::string text;
    };

    struct ForNode;
    struct IfNode;
    using Node = std::variant<TextNode, ForNode, IfNode>;

    struct ForNode {
      std::vector<std::string> variables;
      std::string iterable;
      std::vector<Node> body;
    };

    struct IfNode {
      std::string conditionExpr;
      bool negated = false;
      std::vector<Node> thenBody;
      std::vector<Node> elseBody;
    };

    class VariableScope {
    public:
      void push(ScopeMap bindings = {}) { m_scopes.push_back(std::move(bindings)); }
      void pop() {
        if (!m_scopes.empty())
          m_scopes.pop_back();
      }
      void set(std::string name, ScopeValue value) {
        if (m_scopes.empty())
          m_scopes.emplace_back();
        m_scopes.back()[std::move(name)] = std::move(value);
      }
      const ScopeValue* get(std::string_view name) const {
        for (const auto& scope : std::views::reverse(m_scopes)) {
          auto found = scope.find(std::string(name));
          if (found != scope.end())
            return &found->second;
        }
        return nullptr;
      }
      bool active() const { return !m_scopes.empty(); }

    private:
      std::vector<ScopeMap> m_scopes;
    };

    constexpr std::string_view kUnknownPrefix = "{{";

    const std::unordered_map<std::string, std::string> kColorAliases = {
        {"hover", "surface_container_high"},
        {"on_hover", "on_surface"},
    };

    const std::unordered_set<std::string> kKnownFormats = {
        "hex", "hex_stripped", "rgb",  "rgb_csv", "rgba", "hsl",        "hsla",
        "red", "green",        "blue", "alpha",   "hue",  "saturation", "lightness",
    };

    const std::unordered_map<std::string, int> kSupportedFilters = {
        {"grayscale", 0},      {"invert", 0},   {"set_alpha", 1},  {"set_lightness", 1},  {"set_hue", 1},
        {"set_saturation", 1}, {"set_red", 1},  {"set_green", 1},  {"set_blue", 1},       {"lighten", 1},
        {"darken", 1},         {"saturate", 1}, {"desaturate", 1}, {"auto_lightness", 1}, {"rotate_hue", 1},
    };

    const std::unordered_set<std::string> kColorArgFilters = {"blend", "harmonize"};

    constexpr std::array<int, 18> kPaletteTones = {0,  5,  10, 15, 20, 25, 30, 35, 40,
                                                   50, 60, 70, 80, 90, 95, 98, 99, 100};
    constexpr std::array<std::string_view, 2> kTemplateModes = {"dark", "light"};

    struct Lab {
      double l = 0.0;
      double a = 0.0;
      double b = 0.0;
    };

    struct CompareColorEntry {
      std::string name;
      std::string color;
    };

    struct InputPathModes {
      std::string dark;
      std::string light;
    };

    struct ParsedTemplateEntry {
      std::string name;
      std::string inputPath;
      std::vector<std::string> outputPaths;
      // Rendered like hooks, then run via sh -lc; non-comment stdout lines become extra output paths.
      std::string outputPathDynamic;
      std::string inputPathDynamic;
      std::string compareTo;
      std::vector<CompareColorEntry> colorsToCompare;
      std::string preHook;
      std::string postHook;
      std::string postAction;
      // When set, skip outputs if this path does not exist (explicit install check).
      std::string requiresPath;
      // When true, skip each output whose inferred client config root is missing.
      bool gateOutputsByClientRoot = false;
      int index = 0;
    };

    std::optional<std::filesystem::path> inferClientConfigRoot(const std::filesystem::path& outputPath) {
      std::vector<std::filesystem::path> parts;
      parts.reserve(16);
      for (const auto& part : outputPath) {
        if (!part.empty() && part != std::filesystem::path("."))
          parts.push_back(part);
      }

      for (std::size_t i = 0; i + 3 < parts.size(); ++i) {
        if (parts[i] == ".var" && parts[i + 1] == "app" && parts[i + 3] == "config") {
          std::filesystem::path root;
          for (std::size_t j = 0; j <= i + 3; ++j)
            root /= parts[j];
          return root;
        }
      }

      std::filesystem::path current = outputPath.parent_path();
      while (!current.empty() && current != current.root_path()) {
        if (current.filename() == "themes") {
          std::filesystem::path parent = current.parent_path();
          const auto grandparent = parent.parent_path();
          if (parent.filename() == "extensions" || grandparent.filename() == "extensions") {
            std::filesystem::path walk = current;
            while (!walk.empty() && walk.filename() != "extensions")
              walk = walk.parent_path();
            if (walk.filename() == "extensions")
              return walk.parent_path();
          }
          return parent;
        }
        current = current.parent_path();
      }
      return std::nullopt;
    }

    bool pathExists(const std::filesystem::path& path) {
      std::error_code ec;
      return std::filesystem::exists(path, ec);
    }

    void markMultiClientGatedEntries(std::vector<ParsedTemplateEntry>& entries) {
      std::unordered_map<std::string, std::unordered_set<std::string>> rootsByInputKey;
      for (const ParsedTemplateEntry& entry : entries) {
        if (entry.inputPath.empty())
          continue;
        for (const std::string& output : entry.outputPaths) {
          if (auto root = inferClientConfigRoot(std::filesystem::path(output)))
            rootsByInputKey[entry.inputPath].insert(root->string());
        }
      }

      for (ParsedTemplateEntry& entry : entries) {
        if (entry.inputPath.empty())
          continue;
        const auto it = rootsByInputKey.find(entry.inputPath);
        if (it != rootsByInputKey.end() && it->second.size() > 1)
          entry.gateOutputsByClientRoot = true;
      }
    }

    const std::regex kBlockRegex(R"(<\*([\s\S]*?)\*>)");
    const std::regex kExprRegex(R"(\{\{([^}\n]+?)\}\})");

    double linearize(int c) {
      const double n = static_cast<double>(c) / 255.0;
      if (n <= 0.04045)
        return n / 12.92;
      return std::pow((n + 0.055) / 1.055, 2.4);
    }

    double labF(double t) {
      if (t > 0.008856)
        return std::cbrt(t);
      return (903.3 * t + 16.0) / 116.0;
    }

    Lab rgbToLab(const Color& c) {
      constexpr double kWhiteX = 95.047;
      constexpr double kWhiteY = 100.0;
      constexpr double kWhiteZ = 108.883;
      const double lr = linearize(c.r);
      const double lg = linearize(c.g);
      const double lb = linearize(c.b);
      const double x = (0.4124564 * lr + 0.3575761 * lg + 0.1804375 * lb) * 100.0;
      const double y = (0.2126729 * lr + 0.7151522 * lg + 0.0721750 * lb) * 100.0;
      const double z = (0.0193339 * lr + 0.1191920 * lg + 0.9503041 * lb) * 100.0;
      const double fx = labF(x / kWhiteX);
      const double fy = labF(y / kWhiteY);
      const double fz = labF(z / kWhiteZ);
      return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
    }

    double labDistance(const Lab& a, const Lab& b) {
      const double dl = a.l - b.l;
      const double da = a.a - b.a;
      const double db = a.b - b.b;
      return std::sqrt(dl * dl + da * da + db * db);
    }

    std::string scopeValueToString(const ScopeValue& value);

    bool isTruthy(const ScopeValue& value) {
      if (std::holds_alternative<std::monostate>(value.value))
        return false;
      if (const auto* b = std::get_if<bool>(&value.value))
        return *b;
      if (const auto* n = std::get_if<double>(&value.value))
        return *n != 0.0;
      if (const auto* s = std::get_if<std::string>(&value.value)) {
        const std::string lowered = StringUtils::toLower(StringUtils::trim(*s));
        return !(lowered.empty() || lowered == "false" || lowered == "0" || lowered == "none");
      }
      if (const auto* arr = std::get_if<ScopeArray>(&value.value))
        return !arr->empty();
      if (const auto* map = std::get_if<ScopeMap>(&value.value))
        return !map->empty();
      return true;
    }

    std::string scopeValueToString(const ScopeValue& value) {
      if (const auto* s = std::get_if<std::string>(&value.value))
        return *s;
      if (const auto* b = std::get_if<bool>(&value.value))
        return *b ? "true" : "false";
      if (const auto* n = std::get_if<double>(&value.value)) {
        const double rounded = std::round(*n);
        if (std::fabs(*n - rounded) < 1.0e-9) {
          std::ostringstream oss;
          oss << static_cast<long long>(rounded);
          return oss.str();
        }
        return StringUtils::formatDotDecimal(*n);
      }
      if (const auto* color = std::get_if<RichColor>(&value.value))
        return color->color.toHex();
      return "";
    }

    RichColor asRichColor(const ScopeValue& value) {
      if (const auto* c = std::get_if<RichColor>(&value.value))
        return *c;
      if (const auto* s = std::get_if<std::string>(&value.value))
        return {Color::fromHex(*s), 1.0};
      throw std::invalid_argument("value is not a color");
    }

    std::string formatColor(const RichColor& color, std::string_view formatType) {
      if (formatType == "hex")
        return color.color.toHex();
      if (formatType == "hex_stripped") {
        std::string out = color.color.toHex();
        if (!out.empty() && out.front() == '#')
          out.erase(out.begin());
        return out;
      }
      if (formatType == "rgb") {
        return "rgb("
            + std::to_string(color.color.r)
            + ", "
            + std::to_string(color.color.g)
            + ", "
            + std::to_string(color.color.b)
            + ")";
      }
      if (formatType == "rgb_csv") {
        return std::to_string(color.color.r)
            + ","
            + std::to_string(color.color.g)
            + ","
            + std::to_string(color.color.b);
      }
      if (formatType == "rgba") {
        return "rgba("
            + std::to_string(color.color.r)
            + ", "
            + std::to_string(color.color.g)
            + ", "
            + std::to_string(color.color.b)
            + ", "
            + StringUtils::formatDotDecimal(color.alpha)
            + ")";
      }
      auto [h, s, l] = color.color.toHsl();
      if (formatType == "hsl")
        return "hsl("
            + std::to_string(static_cast<int>(h))
            + ", "
            + std::to_string(static_cast<int>(s * 100.0))
            + "%, "
            + std::to_string(static_cast<int>(l * 100.0))
            + "%)";
      if (formatType == "hsla") {
        return "hsla("
            + std::to_string(static_cast<int>(h))
            + ", "
            + std::to_string(static_cast<int>(s * 100.0))
            + "%, "
            + std::to_string(static_cast<int>(l * 100.0))
            + "%, "
            + StringUtils::formatDotDecimal(color.alpha)
            + ")";
      }
      if (formatType == "hue")
        return std::to_string(static_cast<int>(h));
      if (formatType == "saturation")
        return std::to_string(static_cast<int>(s * 100.0));
      if (formatType == "lightness")
        return std::to_string(static_cast<int>(l * 100.0));
      if (formatType == "red")
        return std::to_string(color.color.r);
      if (formatType == "green")
        return std::to_string(color.color.g);
      if (formatType == "blue")
        return std::to_string(color.color.b);
      if (formatType == "alpha") {
        return StringUtils::formatDotDecimal(color.alpha);
      }
      return color.color.toHex();
    }

    std::vector<std::string> splitPipes(std::string_view expr) {
      std::vector<std::string> parts;
      std::string current;
      bool inQuotes = false;
      char quoteChar = '\0';
      for (char ch : expr) {
        if ((ch == '"' || ch == '\'') && !inQuotes) {
          inQuotes = true;
          quoteChar = ch;
          current.push_back(ch);
        } else if (ch == quoteChar && inQuotes) {
          inQuotes = false;
          quoteChar = '\0';
          current.push_back(ch);
        } else if (ch == '|' && !inQuotes) {
          parts.push_back(current);
          current.clear();
        } else {
          current.push_back(ch);
        }
      }
      if (!current.empty())
        parts.push_back(current);
      return parts;
    }

    std::pair<std::string, std::optional<std::string>> parseFilter(std::string filterStr) {
      filterStr = StringUtils::trim(filterStr);
      std::smatch match;
      if (std::regex_match(filterStr, match, std::regex(R"(^([a-z_]+)\s*:\s*(.+)$)")))
        return {match[1].str(), StringUtils::trim(match[2].str())};
      const size_t space = filterStr.find_first_of(" \t");
      if (space == std::string::npos)
        return {filterStr, std::nullopt};
      return {StringUtils::trim(filterStr.substr(0, space)), StringUtils::trim(filterStr.substr(space + 1))};
    }

    double parseNumber(const std::optional<std::string>& arg, double fallback = 0.0) {
      if (!arg)
        return fallback;
      if (const auto value = StringUtils::parseDotDecimal<double>(*arg))
        return *value;
      throw std::invalid_argument("invalid numeric filter argument");
    }

    std::vector<std::string> splitWords(std::string s) {
      s = std::regex_replace(s, std::regex(R"(([a-z])([A-Z]))"), "$1_$2");
      std::vector<std::string> out;
      std::string current;
      for (char ch : s) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
          current.push_back(ch);
        } else if (!current.empty()) {
          out.push_back(current);
          current.clear();
        }
      }
      if (!current.empty())
        out.push_back(current);
      return out;
    }

    std::string applyReplace(const std::string& value, const std::optional<std::string>& arg) {
      if (!arg)
        return value;
      std::smatch match;
      if (std::regex_match(*arg, match, std::regex(R"REGEX("([^"]*?)"\s*,\s*"([^"]*?)")REGEX")))
        return StringUtils::replaceAll(value, match[1].str(), match[2].str());
      if (std::regex_match(*arg, match, std::regex(R"REGEX('([^']*?)'\s*,\s*'([^']*?)')REGEX")))
        return StringUtils::replaceAll(value, match[1].str(), match[2].str());
      return value;
    }

    std::string toCamelCase(const std::string& value) {
      auto words = splitWords(value);
      if (words.empty())
        return value;
      std::string out = StringUtils::toLower(words.front());
      for (size_t i = 1; i < words.size(); ++i) {
        std::string word = StringUtils::toLower(words[i]);
        if (!word.empty())
          word[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(word[0])));
        out += word;
      }
      return out;
    }

    std::string toPascalCase(const std::string& value) {
      auto words = splitWords(value);
      if (words.empty())
        return value;
      std::string out;
      for (auto& word : words) {
        word = StringUtils::toLower(word);
        if (!word.empty())
          word[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(word[0])));
        out += word;
      }
      return out;
    }

    std::string joinLower(const std::string& value, std::string_view sep) {
      auto words = splitWords(value);
      std::string out;
      for (size_t i = 0; i < words.size(); ++i) {
        if (i)
          out += sep;
        out += StringUtils::toLower(words[i]);
      }
      return out;
    }

    std::string findClosestColor(std::string_view compareTo, const std::vector<CompareColorEntry>& colors) {
      if (colors.empty())
        return {};
      const Lab target = rgbToLab(Color::fromHex(compareTo));
      std::string closestName;
      double closestDist = std::numeric_limits<double>::infinity();
      for (const auto& entry : colors) {
        try {
          const double dist = labDistance(target, rgbToLab(Color::fromHex(entry.color)));
          if (dist < closestDist) {
            closestDist = dist;
            closestName = entry.name;
          }
        } catch (...) {
        }
      }
      return closestName;
    }

    RichColor applyColorArgFilter(RichColor value, const std::string& name, const std::optional<std::string>& arg) {
      if (!arg)
        return value;
      std::smatch match;
      if (!std::regex_search(*arg, match, std::regex(R"(["']?(#[0-9a-fA-F]{6})["']?\s*(?:,\s*(.+))?)")))
        return value;
      const Color target = Color::fromHex(match[1].str());
      auto [srcHue, srcSat, srcLight] = value.color.toHsl();
      auto [targetHue, _targetSat, _targetLight] = target.toHsl();
      double diff = targetHue - srcHue;
      if (diff > 180.0)
        diff -= 360.0;
      else if (diff < -180.0)
        diff += 360.0;
      double newHue = srcHue;
      if (name == "blend") {
        const std::optional<std::string> amountArg =
            match[2].matched ? std::optional<std::string>(match[2].str()) : std::nullopt;
        const double amount = std::clamp(parseNumber(amountArg), 0.0, 1.0);
        newHue = std::fmod(srcHue + diff * amount + 360.0, 360.0);
      } else if (name == "harmonize") {
        double rotation = std::min(std::fabs(diff) * 0.5, 15.0);
        if (diff < 0.0)
          rotation = -rotation;
        newHue = std::fmod(srcHue + rotation + 360.0, 360.0);
      }
      value.color = Color::fromHsl(newHue, srcSat, srcLight);
      return value;
    }

    RichColor applyColorFilter(RichColor color, const std::string& name, const std::optional<std::string>& arg) {
      auto [h, s, l] = color.color.toHsl();
      if (name == "grayscale") {
        const int gray =
            static_cast<int>(std::lround(0.299 * color.color.r + 0.587 * color.color.g + 0.114 * color.color.b));
        color.color = Color(gray, gray, gray);
        return color;
      }
      if (name == "invert") {
        color.color = Color(255 - color.color.r, 255 - color.color.g, 255 - color.color.b);
        return color;
      }
      const double numArg = parseNumber(arg);
      if (name == "set_alpha") {
        color.alpha = std::clamp(numArg, 0.0, 1.0);
      } else if (name == "set_lightness") {
        color.color = Color::fromHsl(h, s, std::clamp(numArg / 100.0, 0.0, 1.0));
      } else if (name == "set_hue") {
        color.color = Color::fromHsl(std::fmod(numArg + 360.0, 360.0), s, l);
      } else if (name == "rotate_hue") {
        color.color = Color::fromHsl(std::fmod(h + numArg + 360.0, 360.0), s, l);
      } else if (name == "set_saturation") {
        color.color = Color::fromHsl(h, std::clamp(numArg / 100.0, 0.0, 1.0), l);
      } else if (name == "lighten") {
        color.color = Color::fromHsl(h, s, std::clamp(l + numArg / 100.0, 0.0, 1.0));
      } else if (name == "darken") {
        color.color = Color::fromHsl(h, s, std::clamp(l - numArg / 100.0, 0.0, 1.0));
      } else if (name == "saturate") {
        color.color = Color::fromHsl(h, std::clamp(s + numArg / 100.0, 0.0, 1.0), l);
      } else if (name == "desaturate") {
        color.color = Color::fromHsl(h, std::clamp(s - numArg / 100.0, 0.0, 1.0), l);
      } else if (name == "auto_lightness") {
        const double target = l < 0.5 ? l + numArg / 100.0 : l - numArg / 100.0;
        color.color = Color::fromHsl(h, s, std::clamp(target, 0.0, 1.0));
      } else if (name == "set_red") {
        color.color.r = std::clamp(static_cast<int>(std::lround(numArg)), 0, 255);
      } else if (name == "set_green") {
        color.color.g = std::clamp(static_cast<int>(std::lround(numArg)), 0, 255);
      } else if (name == "set_blue") {
        color.color.b = std::clamp(static_cast<int>(std::lround(numArg)), 0, 255);
      }
      return color;
    }

    class EngineImpl {
    public:
      explicit EngineImpl(TemplateEngine::ThemeData themeData, TemplateEngine::Options options)
          : m_themeData(std::move(themeData)), m_options(std::move(options)) {}

      RenderResult render(std::string_view templateText) {
        m_errorCount = 0;
        const auto tokens = tokenize(templateText);
        size_t pos = 0;
        const auto nodes = parseNodes(tokens, pos, {});
        VariableScope scope;
        return {evaluateNodes(nodes, scope), m_errorCount};
      }

      RenderFileResult renderFile(const std::filesystem::path& inputPath, const std::filesystem::path& outputPath) {
        RenderFileResult result;
        std::ifstream in(inputPath);
        if (!in) {
          if (m_options.verbose)
            kLog.warn("failed to open template input {}", inputPath.string());
          return result;
        }
        std::stringstream buffer;
        buffer << in.rdbuf();
        auto rendered = render(buffer.str());
        result.errorCount = rendered.errorCount;
        if (rendered.errorCount > 0) {
          if (m_options.verbose) {
            kLog.warn(
                "failed to render template {} -> {}: {} template error(s); output not written", inputPath.string(),
                outputPath.string(), rendered.errorCount
            );
          }
          return result;
        }
        std::error_code ec;
        std::filesystem::create_directories(outputPath.parent_path(), ec);
        std::string previous;
        {
          std::ifstream existing(outputPath);
          if (existing) {
            std::stringstream prev;
            prev << existing.rdbuf();
            previous = prev.str();
          }
        }
        if (previous == rendered.text) {
          result.success = true;
          return result;
        }
        std::ofstream out(outputPath);
        if (!out) {
          if (m_options.verbose)
            kLog.warn("failed to open template output {}", outputPath.string());
          return result;
        }
        out << rendered.text;
        result.success = true;
        result.wrote = true;
        return result;
      }

    private:
      using Token = std::variant<std::string, std::pair<std::string, std::string>>;

      TemplateEngine::ThemeData m_themeData;
      TemplateEngine::Options m_options;
      int m_errorCount = 0;
      std::optional<ScopeMap> m_colorsMap;

      void logError() { ++m_errorCount; }

      std::vector<Token> tokenize(std::string_view text) const {
        std::vector<Token> tokens;
        std::string input(text);
        size_t lastEnd = 0;
        for (auto it = std::sregex_iterator(input.begin(), input.end(), kBlockRegex); it != std::sregex_iterator();
             ++it) {
          auto start = static_cast<size_t>(it->position());
          size_t end = start + static_cast<size_t>(it->length());
          size_t lineStart = input.rfind('\n', start);
          lineStart = (lineStart == std::string::npos) ? 0 : (lineStart + 1);
          size_t adjustedStart = start;
          size_t adjustedEnd = end;
          if (StringUtils::trim(std::string_view(input).substr(lineStart, start - lineStart)).empty()) {
            size_t afterEnd = end;
            while (afterEnd < input.size() && (input[afterEnd] == ' ' || input[afterEnd] == '\t'))
              ++afterEnd;
            if (afterEnd == input.size() || input[afterEnd] == '\n') {
              adjustedStart = lineStart;
              adjustedEnd = afterEnd == input.size() ? afterEnd : afterEnd + 1;
            }
          }
          if (adjustedStart > lastEnd)
            tokens.emplace_back(input.substr(lastEnd, adjustedStart - lastEnd));
          tokens.emplace_back(std::make_pair(std::string("block"), StringUtils::trim((*it)[1].str())));
          lastEnd = adjustedEnd;
        }
        if (lastEnd < input.size())
          tokens.emplace_back(input.substr(lastEnd));
        return tokens;
      }

      std::vector<Node>
      parseNodes(const std::vector<Token>& tokens, size_t& pos, const std::unordered_set<std::string>& stopKeywords) {
        std::vector<Node> nodes;
        while (pos < tokens.size()) {
          if (const auto* text = std::get_if<std::string>(&tokens[pos])) {
            if (!text->empty())
              nodes.emplace_back(TextNode{*text});
            ++pos;
            continue;
          }
          const auto& cmd = std::get<std::pair<std::string, std::string>>(tokens[pos]).second;
          bool shouldStop = false;
          for (const auto& kw : stopKeywords) {
            if (cmd.starts_with(kw)) {
              shouldStop = true;
              break;
            }
          }
          if (shouldStop)
            return nodes;
          if (cmd.starts_with("for ")) {
            nodes.emplace_back(parseFor(tokens, pos));
          } else if (cmd.starts_with("if ")) {
            nodes.emplace_back(parseIf(tokens, pos));
          } else {
            ++pos;
          }
        }
        return nodes;
      }

      ForNode parseFor(const std::vector<Token>& tokens, size_t& pos) {
        const auto& cmd = std::get<std::pair<std::string, std::string>>(tokens[pos]).second;
        ++pos;
        std::smatch match;
        if (!std::regex_match(cmd, match, std::regex(R"(^for\s+(.+?)\s+in\s+(.+)$)"))) {
          logError();
          return {};
        }
        std::vector<std::string> variables;
        std::stringstream vars(match[1].str());
        std::string item;
        while (std::getline(vars, item, ','))
          variables.push_back(StringUtils::trim(item));
        const std::string iterable = StringUtils::trim(match[2].str());
        auto body = parseNodes(tokens, pos, {"endfor"});
        if (pos < tokens.size())
          ++pos;
        return {variables, iterable, std::move(body)};
      }

      IfNode parseIf(const std::vector<Token>& tokens, size_t& pos) {
        const auto& cmd = std::get<std::pair<std::string, std::string>>(tokens[pos]).second;
        ++pos;
        bool negated = false;
        std::string conditionPart = StringUtils::trim(cmd.substr(3));
        if (conditionPart.starts_with("not ")) {
          negated = true;
          conditionPart = StringUtils::trim(conditionPart.substr(4));
        }
        std::smatch match;
        std::string conditionExpr = conditionPart;
        if (std::regex_match(conditionPart, match, std::regex(R"(^\{\{(.+?)\}\}$)")))
          conditionExpr = StringUtils::trim(match[1].str());
        auto thenBody = parseNodes(tokens, pos, {"else", "endif"});
        std::vector<Node> elseBody;
        if (pos < tokens.size()) {
          const auto& stopCmd = std::get<std::pair<std::string, std::string>>(tokens[pos]).second;
          if (StringUtils::trim(stopCmd) == "else") {
            ++pos;
            elseBody = parseNodes(tokens, pos, {"endif"});
          }
        }
        if (pos < tokens.size())
          ++pos;
        return {conditionExpr, negated, std::move(thenBody), std::move(elseBody)};
      }

      ScopeMap buildColorsMap() {
        if (m_colorsMap)
          return *m_colorsMap;
        ScopeMap colors;
        std::unordered_set<std::string> names;
        for (const auto& [mode, modeData] : m_themeData) {
          (void)mode;
          for (const auto& [name, value] : modeData)
            if (!value.empty())
              names.insert(name);
        }
        std::vector<std::string> sorted(names.begin(), names.end());
        std::ranges::sort(sorted);
        for (const auto& name : sorted) {
          ScopeMap modeMap;
          for (const auto& [mode, modeData] : m_themeData) {
            if (auto it = modeData.find(name); it != modeData.end())
              modeMap[mode] = ScopeValue(it->second);
          }
          auto def = modeMap.find(m_options.defaultMode);
          if (def != modeMap.end())
            modeMap["default"] = def->second;
          colors[name] = ScopeValue(std::move(modeMap));
        }
        m_colorsMap = colors;
        return colors;
      }

      ScopeArray getPaletteEntries(const std::string& paletteName) {
        static const std::unordered_map<std::string, std::string> paletteColorMap = {
            {"primary", "primary"}, {"secondary", "secondary"}, {"tertiary", "tertiary"},
            {"error", "error"},     {"neutral", "surface"},     {"neutral_variant", "surface_variant"},
        };

        const auto mapped = paletteColorMap.find(paletteName);
        if (mapped == paletteColorMap.end())
          return {};

        auto modeIt = m_themeData.find(m_options.defaultMode);
        if (modeIt == m_themeData.end())
          return {};
        auto colorIt = modeIt->second.find(mapped->second);
        if (colorIt == modeIt->second.end())
          return {};

        material_color_utilities::TonalPalette palette(Color::fromHex(colorIt->second).toArgb());
        ScopeArray entries;
        entries.reserve(kPaletteTones.size());
        for (int tone : kPaletteTones) {
          const std::string hex = Color::fromArgb(palette.get(static_cast<double>(tone))).toHex();
          ScopeMap toneEntry;
          toneEntry["default"] = ScopeValue(hex);
          toneEntry["dark"] = ScopeValue(hex);
          toneEntry["light"] = ScopeValue(hex);
          entries.emplace_back(std::move(toneEntry));
        }
        return entries;
      }

      ScopeValue resolveFromScope(const std::string& base, const VariableScope& scope) {
        std::stringstream ss(base);
        std::string segment;
        std::vector<std::string> parts;
        while (std::getline(ss, segment, '.'))
          parts.push_back(segment);
        if (parts.empty())
          return {};
        const ScopeValue* current = scope.get(parts.front());
        if (!current)
          return {};
        ScopeValue value = *current;
        for (size_t i = 1; i < parts.size(); ++i) {
          if (auto* map = std::get_if<ScopeMap>(&value.value)) {
            auto it = map->find(parts[i]);
            if (it == map->end())
              return {};
            value = it->second;
          } else if (auto* str = std::get_if<std::string>(&value.value)) {
            if (str->size() >= 7 && (*str)[0] == '#' && kKnownFormats.contains(parts[i]))
              return ScopeValue(formatColor({Color::fromHex(*str), 1.0}, parts[i]));
            return {};
          } else {
            return {};
          }
        }
        return value;
      }

      ScopeValue resolveExpressionValue(const std::string& expr, const VariableScope& scope) {
        auto parts = splitPipes(expr);
        if (parts.empty())
          return {};
        const std::string base = StringUtils::trim(parts.front());
        std::vector<std::string> filters;
        for (size_t i = 1; i < parts.size(); ++i)
          filters.push_back(StringUtils::trim(parts[i]));

        ScopeValue resolved;
        if (base == "mode") {
          resolved = ScopeValue(m_options.defaultMode);
        } else if (base == "closest_color") {
          resolved = ScopeValue(m_options.closestColor);
        } else if (base == "image") {
          resolved = ScopeValue(m_options.imagePath);
        } else if (base == "config_dir") {
          resolved = ScopeValue(m_options.configDir);
        } else if (base == "config_file") {
          resolved = ScopeValue(m_options.configFile);
        } else if (
            auto fromScope = resolveFromScope(base, scope); !std::holds_alternative<std::monostate>(fromScope.value)
        ) {
          resolved = std::move(fromScope);
        } else if (base.starts_with("colors.")) {
          resolved = ScopeValue(processColorExpression(base, filters));
          return resolved;
        } else {
          return ScopeValue(std::string(kUnknownPrefix) + expr + "}}");
        }

        for (const auto& filter : filters) {
          auto [name, arg] = parseFilter(filter);
          if (name == "replace") {
            resolved = ScopeValue(applyReplace(scopeValueToString(resolved), arg));
          } else if (name == "lower_case") {
            resolved = ScopeValue(StringUtils::toLower(scopeValueToString(resolved)));
          } else if (name == "camel_case") {
            resolved = ScopeValue(toCamelCase(scopeValueToString(resolved)));
          } else if (name == "pascal_case") {
            resolved = ScopeValue(toPascalCase(scopeValueToString(resolved)));
          } else if (name == "snake_case") {
            resolved = ScopeValue(joinLower(scopeValueToString(resolved), "_"));
          } else if (name == "kebab_case") {
            resolved = ScopeValue(joinLower(scopeValueToString(resolved), "-"));
          } else if (name == "to_color") {
            resolved = ScopeValue(scopeValueToString(resolved));
          } else if (kColorArgFilters.contains(name)) {
            try {
              RichColor color = asRichColor(resolved);
              resolved = ScopeValue(applyColorArgFilter(color, name, arg));
            } catch (...) {
              logError();
            }
          } else if (kSupportedFilters.contains(name)) {
            try {
              RichColor color = asRichColor(resolved);
              resolved = ScopeValue(applyColorFilter(color, name, arg));
            } catch (...) {
              logError();
            }
          }
        }
        return resolved;
      }

      std::string processColorExpression(const std::string& base, const std::vector<std::string>& filters) {
        std::smatch match;
        if (!std::regex_match(base, match, std::regex(R"(^colors\.([a-z_0-9]+)\.([a-z_0-9]+)\.([a-z_0-9]+)$)"))) {
          logError();
          return "{{" + base + "}}";
        }
        std::string colorName = match[1].str();
        const std::string mode = match[2].str();
        const std::string formatType = match[3].str();
        if (auto alias = kColorAliases.find(colorName); alias != kColorAliases.end())
          colorName = alias->second;

        auto modeIt = m_themeData.find(mode == "default" ? m_options.defaultMode : mode);
        if (modeIt == m_themeData.end()) {
          logError();
          return "{{UNKNOWN:" + colorName + "." + mode + "}}";
        }
        auto colorIt = modeIt->second.find(colorName);
        if (colorIt == modeIt->second.end()) {
          logError();
          return "{{UNKNOWN:" + colorName + "." + mode + "}}";
        }

        RichColor color{Color::fromHex(colorIt->second), 1.0};
        for (const auto& filterStr : filters) {
          auto [name, arg] = parseFilter(filterStr);
          if (name == "replace")
            return applyReplace(formatColor(color, formatType), arg);
          if (name == "lower_case")
            return StringUtils::toLower(formatColor(color, formatType));
          if (name == "camel_case")
            return toCamelCase(formatColor(color, formatType));
          if (name == "pascal_case")
            return toPascalCase(formatColor(color, formatType));
          if (name == "snake_case")
            return joinLower(formatColor(color, formatType), "_");
          if (name == "kebab_case")
            return joinLower(formatColor(color, formatType), "-");
          if (name == "to_color") {
            continue;
          } else if (kColorArgFilters.contains(name)) {
            try {
              color = applyColorArgFilter(color, name, arg);
            } catch (...) {
              logError();
              return "{{" + base + "}}";
            }
          } else if (kSupportedFilters.contains(name)) {
            try {
              color = applyColorFilter(color, name, arg);
            } catch (...) {
              logError();
              return "{{" + base + "}}";
            }
          }
        }
        return formatColor(color, formatType);
      }

      std::string resolveText(std::string_view text, const VariableScope& scope) {
        std::string input(text);
        std::string output;
        size_t last = 0;
        for (auto it = std::sregex_iterator(input.begin(), input.end(), kExprRegex); it != std::sregex_iterator();
             ++it) {
          output.append(input, last, static_cast<size_t>(it->position()) - last);
          const std::string expr = StringUtils::trim((*it)[1].str());
          output += scopeValueToString(resolveExpressionValue(expr, scope));
          last = static_cast<size_t>(it->position() + it->length());
        }
        output.append(input, last, std::string::npos);
        return output;
      }

      ScopeArray resolveIterable(const std::string& expr, const VariableScope& scope) {
        std::smatch match;
        if (std::regex_match(expr, match, std::regex(R"(^(-?\d+)\.\.(-?\d+)$)"))) {
          ScopeArray out;
          const int start = std::stoi(match[1].str());
          const int end = std::stoi(match[2].str());
          for (int i = start; i < end; ++i)
            out.emplace_back(i);
          return out;
        }
        if (expr == "colors") {
          ScopeArray out;
          auto colors = buildColorsMap();
          std::vector<std::string> keys;
          keys.reserve(colors.size());
          for (const auto& [name, _value] : colors)
            keys.push_back(name);
          std::ranges::sort(keys);
          for (const auto& key : keys) {
            ScopeMap pair;
            pair["key"] = ScopeValue(key);
            pair["value"] = colors[key];
            out.emplace_back(std::move(pair));
          }
          return out;
        }
        if (expr.starts_with("palettes.")) {
          return getPaletteEntries(expr.substr(std::string("palettes.").size()));
        }
        if (const auto* value = scope.get(expr)) {
          if (const auto* arr = std::get_if<ScopeArray>(&value->value))
            return *arr;
          if (const auto* map = std::get_if<ScopeMap>(&value->value)) {
            ScopeArray out;
            for (const auto& [key, val] : *map) {
              ScopeMap pair;
              pair["key"] = ScopeValue(key);
              pair["value"] = val;
              out.emplace_back(std::move(pair));
            }
            return out;
          }
        }
        return {};
      }

      std::string evaluateNodes(const std::vector<Node>& nodes, VariableScope& scope) {
        std::string out;
        for (const auto& node : nodes) {
          if (const auto* text = std::get_if<TextNode>(&node)) {
            out += resolveText(text->text, scope);
          } else if (const auto* loop = std::get_if<ForNode>(&node)) {
            out += evaluateFor(*loop, scope);
          } else if (const auto* cond = std::get_if<IfNode>(&node)) {
            out += evaluateIf(*cond, scope);
          }
        }
        return out;
      }

      std::string evaluateFor(const ForNode& node, VariableScope& scope) {
        auto iterable = resolveIterable(node.iterable, scope);
        if (iterable.empty())
          return "";
        std::string out;
        const int total = static_cast<int>(iterable.size());
        for (int index = 0; index < total; ++index) {
          ScopeMap loopMeta;
          loopMeta["index"] = ScopeValue(index);
          loopMeta["first"] = ScopeValue(index == 0);
          loopMeta["last"] = ScopeValue(index == total - 1);
          scope.push({{"loop", ScopeValue(loopMeta)}});
          const ScopeValue& item = iterable[static_cast<size_t>(index)];
          if (const auto* pair = std::get_if<ScopeMap>(&item.value);
              pair && pair->contains("key") && pair->contains("value")) {
            if (node.variables.size() >= 2) {
              scope.set(node.variables[0], pair->at("key"));
              scope.set(node.variables[1], pair->at("value"));
            } else if (!node.variables.empty()) {
              scope.set(node.variables[0], pair->at("key"));
            }
          } else if (!node.variables.empty()) {
            scope.set(node.variables[0], item);
          }
          out += evaluateNodes(node.body, scope);
          scope.pop();
        }
        return out;
      }

      std::string evaluateIf(const IfNode& node, VariableScope& scope) {
        ScopeValue value = resolveExpressionValue(node.conditionExpr, scope);
        bool truthy = isTruthy(value);
        if (node.negated)
          truthy = !truthy;
        return truthy ? evaluateNodes(node.thenBody, scope) : evaluateNodes(node.elseBody, scope);
      }
    };

  } // namespace

  TemplateEngine::TemplateEngine(ThemeData themeData) : TemplateEngine(std::move(themeData), Options{}) {}

  TemplateEngine::TemplateEngine(ThemeData themeData, Options options)
      : m_themeData(std::move(themeData)), m_options(std::move(options)) {}

  TemplateEngine::ThemeData TemplateEngine::makeThemeData(const GeneratedPalette& palette) {
    ThemeData data;
    auto fill = [](const std::unordered_map<std::string, uint32_t>& src, ModeMap& out) {
      for (const auto& [key, value] : src)
        out[key] = Color::fromArgb(value).toHex();
    };
    fill(palette.dark, data["dark"]);
    fill(palette.light, data["light"]);
    return data;
  }

  RenderResult TemplateEngine::render(std::string_view templateText) {
    return EngineImpl(m_themeData, m_options).render(templateText);
  }

  RenderFileResult
  TemplateEngine::renderFile(const std::filesystem::path& inputPath, const std::filesystem::path& outputPath) {
    return EngineImpl(m_themeData, m_options).renderFile(inputPath, outputPath);
  }

  namespace {

    material_color_utilities::DynamicScheme
    makeCustomColorScheme(std::string_view schemeType, material_color_utilities::Hct source) {
      if (schemeType == "tonal-spot")
        return material_color_utilities::SchemeTonalSpot(source, false);
      if (schemeType == "fruit-salad")
        return material_color_utilities::SchemeFruitSalad(source, false);
      if (schemeType == "rainbow")
        return material_color_utilities::SchemeRainbow(source, false);
      if (schemeType == "monochrome")
        return material_color_utilities::SchemeMonochrome(source, false);
      return material_color_utilities::SchemeContent(source, false);
    }

    std::string harmonizeHex(std::string_view srcHex, std::string_view targetHex) {
      const Color src = Color::fromHex(srcHex);
      const Color target = Color::fromHex(targetHex);
      material_color_utilities::Hct srcHct(src.toArgb());
      material_color_utilities::Hct targetHct(target.toArgb());
      double diff = targetHct.get_hue() - srcHct.get_hue();
      if (diff > 180.0)
        diff -= 360.0;
      else if (diff < -180.0)
        diff += 360.0;
      double rotation = std::min(std::fabs(diff) * 0.5, 15.0);
      if (diff < 0.0)
        rotation = -rotation;
      material_color_utilities::Hct result(
          std::fmod(srcHct.get_hue() + rotation + 360.0, 360.0), srcHct.get_chroma(), srcHct.get_tone()
      );
      return Color::fromArgb(result.ToInt()).toHex();
    }

    std::vector<CompareColorEntry> parseColorsToCompare(const toml::node* node) {
      std::vector<CompareColorEntry> out;
      const toml::array* arr = node == nullptr ? nullptr : node->as_array();
      if (arr == nullptr)
        return out;
      for (const auto& item : *arr) {
        const toml::table* tbl = item.as_table();
        if (tbl == nullptr)
          continue;
        auto name = tbl->get_as<std::string>("name");
        auto color = tbl->get_as<std::string>("color");
        if (name && color)
          out.push_back(CompareColorEntry{name->get(), color->get()});
      }
      return out;
    }

    std::optional<InputPathModes> parseInputPathModes(const toml::table& tpl) {
      const toml::table* tbl = tpl["input_path_modes"].as_table();
      if (tbl == nullptr)
        return std::nullopt;
      auto dark = tbl->get_as<std::string>("dark");
      auto light = tbl->get_as<std::string>("light");
      if (!dark || !light)
        return std::nullopt;
      return InputPathModes{.dark = dark->get(), .light = light->get()};
    }

    std::vector<std::string> parseOutputPaths(const toml::node* node) {
      std::vector<std::string> out;
      if (node == nullptr)
        return out;
      if (const toml::value<std::string>* str = node->as_string()) {
        out.push_back(str->get());
        return out;
      }
      const toml::array* arr = node->as_array();
      if (arr == nullptr)
        return out;
      out.reserve(arr->size());
      for (const auto& item : *arr) {
        if (const toml::value<std::string>* str = item.as_string())
          out.push_back(str->get());
      }
      return out;
    }

    // Expand a leading XDG base-directory token (e.g. "$XDG_CONFIG_HOME") to its
    // value per the XDG Base Directory spec: the env var if set, else the
    // spec-defined default under $HOME. Naming the spec variable directly keeps
    // output paths spec-correct instead of baking in a "~/.config" fallback that
    // ignores a relocated config/data/cache home. If the base can't be resolved
    // (no env var and no $HOME), the token is left intact so the bad path
    // surfaces rather than silently landing somewhere wrong.
    std::string expandXdgBaseDir(const std::string& path) {
      struct XdgBase {
        std::string_view token;
        std::string_view envVar;
        std::string_view homeDefault; // relative to $HOME
      };
      static constexpr std::array<XdgBase, 4> kBases = {{
          {"$XDG_CONFIG_HOME", "XDG_CONFIG_HOME", ".config"},
          {"$XDG_DATA_HOME", "XDG_DATA_HOME", ".local/share"},
          {"$XDG_STATE_HOME", "XDG_STATE_HOME", ".local/state"},
          {"$XDG_CACHE_HOME", "XDG_CACHE_HOME", ".cache"},
      }};
      for (const XdgBase& b : kBases) {
        if (!path.starts_with(b.token))
          continue;
        if (path.size() != b.token.size() && path[b.token.size()] != '/')
          continue;
        std::string base;
        if (const char* env = std::getenv(std::string(b.envVar).c_str()); env != nullptr && env[0] != '\0') {
          base = env;
        } else if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
          base = std::string(home) + "/" + std::string(b.homeDefault);
        } else {
          return path;
        }
        return base + path.substr(b.token.size());
      }
      return path;
    }

    std::filesystem::path resolveConfigPath(const std::filesystem::path& configPath, const std::string& path) {
      const std::filesystem::path expanded = FileUtils::expandUserPath(expandXdgBaseDir(path));
      if (expanded.is_absolute())
        return expanded;
      const std::filesystem::path base =
          configPath.has_parent_path() ? configPath.parent_path() : std::filesystem::path{};
      return base / expanded;
    }

    void appendPathsFromDynamicStdout(
        const std::filesystem::path& configPath, std::vector<std::string>& outputs, const std::string& stdoutText
    ) {
      std::string_view remaining(stdoutText);
      while (!remaining.empty()) {
        const std::size_t nl = remaining.find('\n');
        const std::string_view line = nl == std::string_view::npos ? remaining : remaining.substr(0, nl);
        remaining = nl == std::string_view::npos ? std::string_view{} : remaining.substr(nl + 1);
        std::string trimmed = StringUtils::trim(line);
        if (trimmed.empty() || trimmed.front() == '#') {
          continue;
        }
        outputs.push_back(resolveConfigPath(configPath, trimmed).string());
      }
    }

    std::optional<ParsedTemplateEntry> parseTemplateEntry(
        const std::filesystem::path& configPath, std::string_view name, const toml::table& tpl,
        std::string_view defaultMode
    ) {
      std::string inputPath;
      if (const auto modes = parseInputPathModes(tpl)) {
        inputPath = defaultMode == "light" ? modes->light : modes->dark;
      } else if (const auto input = tpl.get_as<std::string>("input_path")) {
        inputPath = input->get();
      }

      std::string inputPathDynamic;
      if (const auto ipd = tpl.get_as<std::string>("input_path_dynamic"))
        inputPathDynamic = ipd->get();

      if (inputPath.empty() && inputPathDynamic.empty())
        return std::nullopt;

      ParsedTemplateEntry entry;
      entry.name = std::string(name);

      if (!inputPath.empty())
        entry.inputPath = resolveConfigPath(configPath, inputPath).string();

      entry.inputPathDynamic = inputPathDynamic;
      entry.outputPaths = parseOutputPaths(tpl.get("output_path"));
      for (std::string& output : entry.outputPaths)
        output = resolveConfigPath(configPath, output).string();
      entry.colorsToCompare = parseColorsToCompare(tpl.get("colors_to_compare"));

      if (const auto compareTo = tpl.get_as<std::string>("compare_to"))
        entry.compareTo = compareTo->get();
      if (const auto preHook = tpl.get_as<std::string>("pre_hook"))
        entry.preHook = preHook->get();
      if (const auto postHook = tpl.get_as<std::string>("post_hook"))
        entry.postHook = postHook->get();
      if (const auto postAction = tpl.get_as<std::string>("post_action"))
        entry.postAction = postAction->get();
      if (const auto opd = tpl.get_as<std::string>("output_path_dynamic"))
        entry.outputPathDynamic = opd->get();
      if (const auto requiresPath = tpl.get_as<std::string>("requires_path"))
        entry.requiresPath = resolveConfigPath(configPath, requiresPath->get()).string();
      if (const auto index = tpl.get_as<int64_t>("index"))
        entry.index = static_cast<int>(index->get());
      return entry;
    }

    bool shouldSkipTemplateOutput(const ParsedTemplateEntry& entry, const std::string& outputPath) {
      if (!entry.requiresPath.empty()) {
        return !pathExists(entry.requiresPath);
      }
      if (!entry.gateOutputsByClientRoot) {
        return false;
      }
      const auto root = inferClientConfigRoot(std::filesystem::path(outputPath));
      if (!root) {
        return false;
      }
      return !pathExists(*root);
    }

  } // namespace

  bool TemplateEngine::processConfigFile(const std::filesystem::path& configPath) {
    auto cancelRequested = [this]() { return m_options.cancelRequested && m_options.cancelRequested(); };
    if (cancelRequested()) {
      return true;
    }

    toml::table root;
    try {
      root = toml::parse_file(configPath.string());
    } catch (const toml::parse_error&) {
      return false;
    }

    return processConfigTable(root, configPath);
  }

  bool TemplateEngine::processConfigTable(const toml::table& root, const std::filesystem::path& configPath) {
    auto cancelRequested = [this]() { return m_options.cancelRequested && m_options.cancelRequested(); };
    if (cancelRequested()) {
      return true;
    }

    if (const toml::table* config = root["config"].as_table()) {
      if (const toml::table* customColors = (*config)["custom_colors"].as_table()) {
        std::string sourceHex;
        if (auto modeIt = m_themeData.find(m_options.defaultMode); modeIt != m_themeData.end()) {
          if (auto it = modeIt->second.find("primary"); it != modeIt->second.end())
            sourceHex = it->second;
          else if (auto it2 = modeIt->second.find("source_color"); it2 != modeIt->second.end())
            sourceHex = it2->second;
        }

        for (const auto& [nameNode, valueNode] : *customColors) {
          const std::string name = std::string(nameNode.str());
          std::string darkHex;
          std::string lightHex;
          bool blend = true;

          if (const auto* str = valueNode.as_string()) {
            darkHex = str->get();
            lightHex = darkHex;
          } else if (const auto* tbl = valueNode.as_table()) {
            const auto* colorPtr = tbl->get_as<std::string>("color");
            const auto* colorDarkPtr = tbl->get_as<std::string>("color_dark");
            const auto* colorLightPtr = tbl->get_as<std::string>("color_light");
            const std::string base = colorPtr ? colorPtr->get() : std::string{};
            const std::string darkOverride = colorDarkPtr ? colorDarkPtr->get() : std::string{};
            const std::string lightOverride = colorLightPtr ? colorLightPtr->get() : std::string{};
            // Per mode: prefer its own override, then the shared color, then the other mode.
            auto pick = [](std::initializer_list<const std::string*> candidates) {
              for (const std::string* c : candidates)
                if (!c->empty())
                  return *c;
              return std::string{};
            };
            darkHex = pick({&darkOverride, &base, &lightOverride});
            lightHex = pick({&lightOverride, &base, &darkOverride});
            if (auto blendValue = tbl->get_as<bool>("blend"))
              blend = blendValue->get();
          } else {
            continue;
          }

          // pick() ties dark/light emptiness together, so this also guards lightHex.
          if (darkHex.empty())
            continue;

          for (std::string_view mode : kTemplateModes) {
            const std::string& colorHex = (mode == "dark") ? darkHex : lightHex;
            const std::string paletteHex = (blend && !sourceHex.empty()) ? harmonizeHex(colorHex, sourceHex) : colorHex;
            const auto scheme = makeCustomColorScheme(
                m_options.schemeType, material_color_utilities::Hct(Color::fromHex(paletteHex).toArgb())
            );
            const auto& palette = scheme.primary_palette;

            auto& modeData = m_themeData[std::string(mode)];
            modeData[name + "_source"] = colorHex;
            modeData[name + "_value"] = colorHex;
            if (mode == "dark") {
              modeData[name] = Color::fromArgb(palette.get(80.0)).toHex();
              modeData["on_" + name] = Color::fromArgb(palette.get(20.0)).toHex();
              modeData[name + "_container"] = Color::fromArgb(palette.get(30.0)).toHex();
              modeData["on_" + name + "_container"] = Color::fromArgb(palette.get(90.0)).toHex();
            } else {
              modeData[name] = Color::fromArgb(palette.get(40.0)).toHex();
              modeData["on_" + name] = Color::fromArgb(palette.get(100.0)).toHex();
              modeData[name + "_container"] = Color::fromArgb(palette.get(90.0)).toHex();
              modeData["on_" + name + "_container"] = Color::fromArgb(palette.get(10.0)).toHex();
            }
          }
        }
      }
    }

    const toml::table* templates = root["templates"].as_table();
    if (templates == nullptr)
      return true;

    std::vector<ParsedTemplateEntry> entries;
    entries.reserve(templates->size());
    for (const auto& [templateName, templateNode] : *templates) {
      if (!m_options.enabledTemplates.empty()
          && !m_options.enabledTemplates.contains(std::string(templateName.str()))) {
        continue;
      }
      const toml::table* tpl = templateNode.as_table();
      if (tpl == nullptr)
        continue;
      if (auto entry = parseTemplateEntry(configPath, templateName.str(), *tpl, m_options.defaultMode))
        entries.push_back(std::move(*entry));
    }
    std::ranges::stable_sort(entries, {}, &ParsedTemplateEntry::index);
    markMultiClientGatedEntries(entries);

    bool ok = true;
    for (const ParsedTemplateEntry& entry : entries) {
      if (cancelRequested()) {
        return ok;
      }

      std::string closestColor;
      if (!entry.compareTo.empty() && !entry.colorsToCompare.empty()) {
        Options compareOptions = m_options;
        compareOptions.closestColor.clear();
        compareOptions.configDir = configPath.has_parent_path() ? configPath.parent_path().string() : "";
        compareOptions.configFile = configPath.string();
        const auto compareRendered = EngineImpl(m_themeData, compareOptions).render(entry.compareTo);
        if (compareRendered.errorCount == 0)
          closestColor = findClosestColor(compareRendered.text, entry.colorsToCompare);
      }

      Options renderOptions = m_options;
      renderOptions.closestColor = closestColor;
      renderOptions.configDir = configPath.has_parent_path() ? configPath.parent_path().string() : "";
      renderOptions.configFile = configPath.string();

      std::string effectiveInput = entry.inputPath;
      if (!entry.inputPathDynamic.empty()) {
        const auto cmdRendered = EngineImpl(m_themeData, renderOptions).render(entry.inputPathDynamic);
        if (cmdRendered.errorCount == 0 && !cmdRendered.text.empty()) {
          const auto dynResult = process::runSync(cmdRendered.text);
          if (dynResult.exitCode == 0) {
            std::vector<std::string> dynamicInputs;
            appendPathsFromDynamicStdout(configPath, dynamicInputs, dynResult.out);

            if (!dynamicInputs.empty())
              effectiveInput = dynamicInputs.front();
          }
        }
      }

      std::vector<std::string> effectiveOutputs = entry.outputPaths;
      if (!entry.outputPathDynamic.empty()) {
        const auto cmdRendered = EngineImpl(m_themeData, renderOptions).render(entry.outputPathDynamic);
        if (cmdRendered.errorCount == 0 && !cmdRendered.text.empty()) {
          const auto dynResult = process::runSync(cmdRendered.text);
          if (dynResult.exitCode == 0) {
            appendPathsFromDynamicStdout(configPath, effectiveOutputs, dynResult.out);
          }
        }
      }

      auto runHook = [&](const std::string& hook) {
        if (!hook.empty() && !cancelRequested()) {
          const auto hookRendered = EngineImpl(m_themeData, renderOptions).render(hook);
          if (hookRendered.errorCount == 0 && !hookRendered.text.empty()) [[maybe_unused]]
            const bool hookOk = process::runSync(hookRendered.text);
        }
      };

      auto runPostAction = [&]() {
        if (entry.postAction.empty()) {
          return true;
        }
        if (entry.postAction != "kde-color-scheme" && entry.postAction != kFirefoxThemePostAction) {
          kLog.warn("unknown post action '{}' for template {}", entry.postAction, entry.name);
          return false;
        }
        if (effectiveOutputs.size() != 1) {
          kLog.warn(
              "post action '{}' for template {} requires exactly one output, got {}", entry.postAction, entry.name,
              effectiveOutputs.size()
          );
          return false;
        }

        if (entry.postAction == "kde-color-scheme") {
          const KdeColorSchemeApplyResult result = applyKdeColorScheme(effectiveOutputs.front());
          if (!result.success) {
            kLog.warn("post action '{}' for template {} failed: {}", entry.postAction, entry.name, result.error);
            return false;
          }
          if (!result.notificationError.empty()) {
            kLog.warn("applied KDE color scheme but failed to notify applications: {}", result.notificationError);
          }
          return true;
        }

        const FirefoxThemeApplyResult result = applyFirefoxTheme(effectiveOutputs.front(), m_options.defaultMode);
        if (!result.success) {
          kLog.warn("post action '{}' for template {} failed: {}", entry.postAction, entry.name, result.error);
          return false;
        }
        if (!result.warning.empty()) {
          kLog.warn("firefox-theme post action for template {}: {}", entry.name, result.warning);
        }
        return true;
      };

      const bool hasOutputs = !effectiveOutputs.empty();
      if (hasOutputs)
        runHook(entry.preHook);

      bool outputsOk = true;
      for (const std::string& outputPath : effectiveOutputs) {
        if (cancelRequested()) {
          return ok;
        }

        if (shouldSkipTemplateOutput(entry, outputPath)) {
          kLog.debug("skipping template {} -> {} (client not installed)", entry.name, outputPath);
          continue;
        }

        if (effectiveInput.empty()) {
          kLog.warn("failed to resolve input path for template {} -> {}; skipping", entry.name, outputPath);
          outputsOk = false;
          ok = false;
          continue;
        }

        const auto fileResult =
            EngineImpl(m_themeData, renderOptions).renderFile(effectiveInput, std::filesystem::path(outputPath));
        if (!fileResult.success) {
          outputsOk = false;
          ok = false;
          continue;
        }
      }

      if (cancelRequested()) {
        return ok;
      }

      if ((hasOutputs && outputsOk) || (!hasOutputs && (!entry.postHook.empty() || !entry.postAction.empty()))) {
        if (!runPostAction()) {
          ok = false;
        }
        runHook(entry.postHook);
      }
    }

    return ok;
  }

} // namespace noctalia::theme
