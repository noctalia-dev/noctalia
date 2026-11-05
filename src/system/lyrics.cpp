#include "system/lyrics.h"

#include "system/lyrics_catalog.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <glib.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <system_error>
#include <tuple>
#include <utility>

namespace {
  std::string matchKey(std::string_view text) {
    const std::string input(text);
    if (!g_utf8_validate(input.c_str(), static_cast<gssize>(input.size()), nullptr)) {
      return {};
    }
    std::unique_ptr<char, decltype(&g_free)> normalized(
        g_utf8_normalize(input.c_str(), static_cast<gssize>(input.size()), G_NORMALIZE_ALL_COMPOSE), g_free
    );
    std::unique_ptr<char, decltype(&g_free)> folded(g_utf8_casefold(normalized.get(), -1), g_free);
    std::string result;
    for (const char* cursor = folded.get(); *cursor != '\0'; cursor = g_utf8_next_char(cursor)) {
      const auto codepoint = g_utf8_get_char(cursor);
      const auto type = g_unichar_type(codepoint);
      const bool mark =
          type == G_UNICODE_NON_SPACING_MARK || type == G_UNICODE_SPACING_MARK || type == G_UNICODE_ENCLOSING_MARK;
      if (g_unichar_isalnum(codepoint) || mark) {
        result.append(cursor, static_cast<std::size_t>(g_utf8_next_char(cursor) - cursor));
      }
    }
    return result;
  }

  std::optional<LyricsTrackScore> scoreRecord(
      const LyricsTrack& wanted, const nlohmann::json& record, std::string_view titleKey, std::string_view artistKey
  ) {
    const double duration = record.at("duration").get<double>();
    if (!std::isfinite(duration) || duration < 0.0 || duration > 86400.0) {
      return std::nullopt;
    }
    return scoreLyricsTrack(
        wanted,
        {
            .title = record.at(titleKey).get<std::string>(),
            .artist = record.at(artistKey).get<std::string>(),
            .durationUs = static_cast<std::int64_t>(std::llround(duration * 1000000.0)),
        }
    );
  }

  std::optional<std::int64_t> integer(std::string_view text);

  constexpr std::string_view kKugouInstrumental = "纯音乐";

  // Lengths further apart than this belong to different recordings.
  constexpr std::int64_t kSameTimingLengthGapMs = 2000;

  // Whether two lyric files share their opening lines at the same times.
  bool sameTiming(const std::vector<LyricsLine>& a, const std::vector<LyricsLine>& b) {
    constexpr std::size_t kCompared = 8;
    constexpr std::int64_t kToleranceMs = 50;
    const auto count = std::min({a.size(), b.size(), kCompared});
    if (count < 4) {
      return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
      if (std::llabs(a[i].timeMs - b[i].timeMs) > kToleranceMs || matchKey(a[i].text) != matchKey(b[i].text)) {
        return false;
      }
    }
    return true;
  }

  // KuGou escapes its LRC bodies as HTML, e.g. "I&apos;m".
  std::string htmlUnescape(std::string_view text) {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 6> entities = {{
        {"&apos;", "'"},
        {"&#39;", "'"},
        {"&quot;", "\""},
        {"&lt;", "<"},
        {"&gt;", ">"},
        {"&amp;", "&"},
    }};
    std::string result;
    result.reserve(text.size());
    while (!text.empty()) {
      const auto* const entity =
          std::ranges::find_if(entities, [text](const auto& pair) { return text.starts_with(pair.first); });
      if (entity != entities.end()) {
        result += entity->second;
        text.remove_prefix(entity->first.size());
      } else {
        result.push_back(text.front());
        text.remove_prefix(1);
      }
    }
    return result;
  }

  // The "title - artist" row and "role：name" credits KuGou puts before the lyrics.
  bool kugouHeaderLine(std::string_view text, bool first) {
    return text.contains(':') || text.contains("：") || (first && text.contains(" - "));
  }

  // Credit rows such as "作词 : Name" that NetEase stores as ordinary lyric lines.
  bool creditLine(std::string_view text) {
    constexpr std::array<std::string_view, 17> labels = {
        "作词",      "作曲",        "编曲",        "制作人",     "制作",     "混音",     "母带",     "监制",     "录音",
        "Lyrics by", "Composed by", "Produced by", "Written by", "Composer", "Producer", "Arranger", "Lyricist",
    };
    return std::ranges::any_of(labels, [text](std::string_view label) {
      if (!text.starts_with(label)) {
        return false;
      }
      auto rest = text.substr(label.size());
      while (rest.starts_with(' ')) {
        rest.remove_prefix(1);
      }
      return rest.starts_with(':') || rest.starts_with("：");
    });
  }

  struct YrcTag {
    std::int64_t start = 0;
    std::int64_t duration = 0;
    std::size_t length = 0;
  };

  // `(start,duration,flag)` at the beginning of `text`.
  std::optional<YrcTag> yrcTag(std::string_view text) {
    if (!text.starts_with('(')) {
      return std::nullopt;
    }
    const auto close = text.find(')');
    if (close == std::string_view::npos) {
      return std::nullopt;
    }
    const auto inner = text.substr(1, close - 1);
    const auto first = inner.find(',');
    const auto second = first == std::string_view::npos ? first : inner.find(',', first + 1);
    if (second == std::string_view::npos) {
      return std::nullopt;
    }
    const auto start = integer(inner.substr(0, first));
    const auto duration = integer(inner.substr(first + 1, second - first - 1));
    if (!start || !duration || !integer(inner.substr(second + 1))) {
      return std::nullopt;
    }
    return YrcTag{.start = *start, .duration = *duration, .length = close + 1};
  }

  // `[start,duration](start,duration,0)word(start,duration,0)word`
  std::optional<LyricsLine> parseYrcLine(std::string_view raw) {
    if (!raw.starts_with('[')) {
      return std::nullopt;
    }
    const auto close = raw.find(']');
    const auto comma = raw.find(',');
    if (close == std::string_view::npos || comma == std::string_view::npos || comma > close) {
      return std::nullopt;
    }
    const auto start = integer(raw.substr(1, comma - 1));
    const auto duration = integer(raw.substr(comma + 1, close - comma - 1));
    if (!start || !duration || *start < 0 || *duration < 0) {
      return std::nullopt;
    }
    LyricsLine line{.timeMs = *start};
    if (*duration > 0) {
      line.endMs = *start + *duration;
    }
    auto rest = raw.substr(close + 1);
    while (!rest.empty()) {
      const auto tag = yrcTag(rest);
      if (!tag || tag->start < 0 || tag->duration < 0) {
        return std::nullopt;
      }
      rest.remove_prefix(tag->length);
      std::size_t end = 0;
      while (end < rest.size() && (rest[end] != '(' || !yrcTag(rest.substr(end)))) {
        ++end;
      }
      const auto segment = rest.substr(0, end);
      rest.remove_prefix(end);
      const auto begin = line.text.size();
      line.text += segment;
      const auto first = segment.find_first_not_of(" \t");
      const auto last = segment.find_last_not_of(" \t");
      if (first == std::string_view::npos) {
        continue;
      }
      if (!line.words.empty() && tag->start < line.words.back().startMs) {
        return std::nullopt;
      }
      line.words.push_back({
          .startByte = begin + first,
          .endByte = begin + last + 1,
          .startMs = tag->start,
          .endMs = tag->start + tag->duration,
      });
    }
    if (line.words.empty()) {
      return std::nullopt;
    }
    return line;
  }

  std::optional<std::int64_t> integer(std::string_view text) {
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
      return std::nullopt;
    }
    return value;
  }

  std::optional<std::int64_t> timestamp(std::string_view text) {
    if (!std::ranges::all_of(text, [](char c) { return (c >= '0' && c <= '9') || c == ':' || c == '.'; })) {
      return std::nullopt;
    }
    const auto colon = text.find(':');
    if (colon == std::string_view::npos) {
      return std::nullopt;
    }
    const auto minutes = integer(text.substr(0, colon));
    text.remove_prefix(colon + 1);
    const auto dot = text.find('.');
    const auto seconds = integer(text.substr(0, dot));
    if (!minutes || !seconds || *minutes < 0 || *minutes > 1440 || *seconds < 0 || *seconds >= 60) {
      return std::nullopt;
    }
    std::int64_t fraction = 0;
    if (dot != std::string_view::npos) {
      const auto digits = text.substr(dot + 1);
      if (digits.empty() || digits.size() > 3) {
        return std::nullopt;
      }
      const auto value = integer(digits);
      if (!value || *value < 0) {
        return std::nullopt;
      }
      fraction = *value * (digits.size() == 1 ? 100 : digits.size() == 2 ? 10 : 1);
    }
    return *minutes * 60000 + *seconds * 1000 + fraction;
  }
} // namespace

LyricsProgress LyricsLine::progress(
    std::int64_t positionMs, std::optional<std::int64_t> nextLineMs, std::int64_t finishEarlyMs
) const {
  if (!words.empty()) {
    for (std::size_t i = 0; i < words.size(); ++i) {
      const auto& word = words[i];
      if (positionMs < word.startMs) {
        return {.segment = i};
      }
      if (positionMs < word.endMs) {
        return {
            .segment = i,
            .fraction = static_cast<float>(positionMs - word.startMs) / static_cast<float>(word.endMs - word.startMs)
        };
      }
    }
    return {.segment = words.size()};
  }
  const auto end = endMs ? endMs : nextLineMs;
  if (!end || *end <= timeMs) {
    return {.segment = 1};
  }
  const auto sweepEnd = *end - std::clamp<std::int64_t>(finishEarlyMs, 0, (*end - timeMs) / 2);
  return {
      .fraction =
          std::clamp(static_cast<float>(positionMs - timeMs) / static_cast<float>(sweepEnd - timeMs), 0.0F, 1.0F)
  };
}

LyricsResult parseLrcmuxResponse(std::string_view body, const LyricsTrack& track, std::string_view expectedIsrc) {
  LyricsResult result;
  try {
    const auto json = nlohmann::json::parse(body);
    const auto& recording = json.at("track");
    const bool sameRecording = expectedIsrc.empty() ? scoreRecord(track, recording, "title", "artist").has_value()
                                                    : recording.at("isrc").get<std::string>() == expectedIsrc;
    if (!sameRecording) {
      return result;
    }
    if (const auto duration = recording.find("duration"); duration != recording.end() && duration->is_number()) {
      result.recordingDurationMs = std::llround(std::clamp(duration->get<double>(), 0.0, 86400.0) * 1000.0);
    }
    const auto level = json.at("meta").at("level").get<std::string>();
    if (level != "none" && level != "line" && level != "word") {
      result.status = LyricsStatus::ERROR;
      return result;
    }
    for (const auto& item : json.at("lines")) {
      auto text = item.at("text").get<std::string>();
      result.document.plainLines.push_back(text);
      if (level == "none") {
        continue;
      }
      LyricsLine line{.timeMs = item.value("start", std::int64_t{0}), .text = std::move(text)};
      if (line.timeMs < 0) {
        result.status = LyricsStatus::ERROR;
        return result;
      }
      if (const auto end = item.find("end");
          end != item.end() && end->is_number_integer() && end->get<std::int64_t>() > line.timeMs) {
        line.endMs = end->get<std::int64_t>();
      }
      if (const auto words = item.find("words"); words != item.end() && words->is_array()) {
        std::size_t offset = 0;
        for (const auto& word : *words) {
          const auto token = word.at("text").get<std::string>();
          const auto start = line.text.find(token, offset);
          const auto startMs = word.at("start").get<std::int64_t>();
          const auto endMs = word.at("end").get<std::int64_t>();
          if (token.empty()
              || start == std::string::npos
              || startMs < line.timeMs
              || endMs < startMs
              || (!line.words.empty() && startMs < line.words.back().startMs)) {
            result.status = LyricsStatus::ERROR;
            return result;
          }
          offset = start + token.size();
          line.words.push_back({.startByte = start, .endByte = offset, .startMs = startMs, .endMs = endMs});
        }
      }
      result.document.lines.push_back(std::move(line));
    }
    std::ranges::stable_sort(result.document.lines, {}, &LyricsLine::timeMs);
    if (!result.document.lines.empty() || !result.document.plainLines.empty()) {
      result.status = LyricsStatus::READY;
    }
  } catch (const nlohmann::json::exception&) {
    result.status = LyricsStatus::ERROR;
  }
  return result;
}

LyricsResult parseNeteaseLyric(std::string_view body) {
  LyricsResult result;
  try {
    const auto json = nlohmann::json::parse(body);
    if (json.at("code").get<int>() != 200) {
      result.status = LyricsStatus::ERROR;
      return result;
    }
    if (json.value("pureMusic", false)) {
      result.document.instrumental = true;
      result.status = LyricsStatus::READY;
      return result;
    }
    const auto field = [&json](const char* group) -> std::string {
      const auto it = json.find(group);
      if (it == json.end() || !it->is_object()) {
        return {};
      }
      const auto text = it->find("lyric");
      return text != it->end() && text->is_string() ? text->get<std::string>() : std::string{};
    };
    std::vector<LyricsLine> lines;
    bool wordTimed = true;
    for (const auto& raw : splitLyricsLines(field("yrc"))) {
      if (raw.empty() || raw.starts_with('{')) {
        continue;
      }
      auto line = parseYrcLine(raw);
      if (!line) {
        wordTimed = false;
        break;
      }
      if (!creditLine(line->text)) {
        lines.push_back(std::move(*line));
      }
    }
    const auto plain = field("lrc");
    if (!wordTimed || lines.empty()) {
      lines = parseLrc(plain);
      std::erase_if(lines, [](const LyricsLine& line) { return creditLine(line.text); });
    }
    std::ranges::stable_sort(lines, {}, &LyricsLine::timeMs);
    for (const auto& line : lines) {
      result.document.plainLines.push_back(line.text);
    }
    if (lines.empty()) {
      for (auto& text : splitLyricsLines(plain)) {
        if (!text.empty() && !creditLine(text)) {
          result.document.plainLines.push_back(std::move(text));
        }
      }
    }
    result.document.lines = std::move(lines);
    if (!result.document.lines.empty() || !result.document.plainLines.empty()) {
      result.status = LyricsStatus::READY;
    }
  } catch (const nlohmann::json::exception&) {
    result.status = LyricsStatus::ERROR;
  }
  return result;
}

LyricsResult parseLyricsPlusResponse(std::string_view body, const LyricsTrack& track) {
  LyricsResult result;
  try {
    const auto json = nlohmann::json::parse(body);
    const auto items = json.find("lyrics");
    if (items == json.end() || !items->is_array() || items->empty()) {
      return result;
    }
    const auto& metadata = json.at("metadata");
    if (const auto total = metadata.find("totalDuration"); total != metadata.end() && total->is_string()) {
      if (const auto ms = timestamp(total->get_ref<const std::string&>())) {
        result.recordingDurationMs = *ms;
      }
    }
    const LyricsTrack found{
        .title = metadata.value("title", std::string{}),
        .artist = metadata.value("artist", std::string{}),
        .durationUs = result.recordingDurationMs * 1000,
    };
    if (!scoreLyricsTrack(track, found)) {
      return result;
    }
    const auto type = json.value("type", std::string{});
    const bool timed = type == "Word" || type == "Line";
    for (const auto& item : *items) {
      LyricsLine line;
      const auto syllables = item.find("syllabus");
      if (type == "Word" && syllables != item.end() && syllables->is_array() && !syllables->empty()) {
        for (const auto& syllable : *syllables) {
          if (syllable.value("isBackground", false) || !syllable.at("time").is_number_integer()) {
            continue;
          }
          const auto text = syllable.at("text").get<std::string>();
          const auto start = syllable.at("time").get<std::int64_t>();
          const auto end = start + std::max<std::int64_t>(0, syllable.value("duration", std::int64_t{0}));
          const auto begin = line.text.size();
          line.text += text;
          const auto first = text.find_first_not_of(" \t");
          const auto last = text.find_last_not_of(" \t");
          if (first == std::string::npos || start < 0 || (!line.words.empty() && start < line.words.back().startMs)) {
            continue;
          }
          line.words.push_back(
              {.startByte = begin + first, .endByte = begin + last + 1, .startMs = start, .endMs = end}
          );
        }
        while (line.text.ends_with(' ')) {
          line.text.pop_back();
        }
      } else {
        line.text = item.value("text", std::string{});
      }
      result.document.plainLines.push_back(line.text);
      const auto time = item.find("time");
      if (!timed || time == item.end() || !time->is_number_integer() || time->get<std::int64_t>() < 0) {
        continue;
      }
      line.timeMs = time->get<std::int64_t>();
      if (const auto duration = item.value("duration", std::int64_t{0}); duration > 0) {
        line.endMs = line.timeMs + duration;
      }
      if (!line.words.empty() && line.words.front().startMs < line.timeMs) {
        line.timeMs = line.words.front().startMs;
      }
      result.document.lines.push_back(std::move(line));
    }
    std::ranges::stable_sort(result.document.lines, {}, &LyricsLine::timeMs);
    if (!result.document.lines.empty() || !result.document.plainLines.empty()) {
      result.status = LyricsStatus::READY;
    }
  } catch (const nlohmann::json::exception&) {
    result.status = LyricsStatus::ERROR;
  }
  return result;
}

LyricsResult parseKugouLyric(std::string_view body) {
  LyricsResult result;
  try {
    const auto json = nlohmann::json::parse(body);
    const auto content = json.find("content");
    if (content == json.end() || !content->is_string() || content->get_ref<const std::string&>().empty()) {
      return result;
    }
    gsize size = 0;
    std::unique_ptr<guchar, decltype(&g_free)> decoded(
        g_base64_decode(content->get_ref<const std::string&>().c_str(), &size), g_free
    );
    const std::string text = htmlUnescape({reinterpret_cast<const char*>(decoded.get()), size});
    auto lines = parseLrc(text);
    if (lines.size() == 1 && lines.front().text.contains(kKugouInstrumental)) {
      result.document.instrumental = true;
      result.status = LyricsStatus::READY;
      return result;
    }
    std::size_t header = 0;
    while (header < lines.size() && kugouHeaderLine(lines[header].text, header == 0)) {
      ++header;
    }
    lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(header));
    for (const auto& line : lines) {
      result.document.plainLines.push_back(line.text);
    }
    result.document.lines = std::move(lines);
    if (!result.document.lines.empty()) {
      result.status = LyricsStatus::READY;
    }
  } catch (const nlohmann::json::exception&) {
    result.status = LyricsStatus::ERROR;
  }
  return result;
}

LyricsResult parseLrclibSearch(std::string_view body, const LyricsTrack& track) {
  LyricsResult result;
  try {
    const auto candidates = nlohmann::json::parse(body);
    if (!candidates.is_array()) {
      result.status = LyricsStatus::ERROR;
      return result;
    }
    const auto album = matchKey(track.album);
    std::optional<std::tuple<int, int, int, bool, double>> bestScore;
    for (const auto& candidate : candidates) {
      const auto match = scoreRecord(track, candidate, "trackName", "artistName");
      if (!match) {
        continue;
      }
      LyricsDocument document;
      document.instrumental = candidate.at("instrumental").get<bool>();
      if (const auto it = candidate.find("syncedLyrics"); it != candidate.end() && it->is_string()) {
        document.lines = parseLrc(it->get_ref<const std::string&>());
      }
      if (const auto it = candidate.find("plainLyrics"); it != candidate.end() && it->is_string()) {
        document.plainLines = splitLyricsLines(it->get_ref<const std::string&>());
      }
      const int quality = !document.lines.empty() ? 2 : !document.plainLines.empty() ? 1 : 0;
      if (quality == 0 && !document.instrumental) {
        continue;
      }
      const bool sameAlbum = !album.empty() && matchKey(candidate.at("albumName").get<std::string>()) == album;
      const auto score = std::tuple{
          match->durationConfidence, quality, match->titleConfidence, sameAlbum, -match->durationDifferenceMs
      };
      if (!bestScore || score > *bestScore) {
        bestScore = score;
        result.status = LyricsStatus::READY;
        result.document = std::move(document);
        result.recordingDurationMs = std::llround(candidate.at("duration").get<double>() * 1000.0);
      }
    }
    // Identical timing cannot fit two recordings of different lengths, so such a length claim is not trusted.
    if (!result.document.lines.empty()) {
      for (const auto& candidate : candidates) {
        const auto synced = candidate.find("syncedLyrics");
        const auto duration = candidate.find("duration");
        if (synced == candidate.end()
            || !synced->is_string()
            || duration == candidate.end()
            || !duration->is_number()) {
          continue;
        }
        const auto otherMs = std::llround(duration->get<double>() * 1000.0);
        if (otherMs > 0
            && std::llabs(otherMs - result.recordingDurationMs) > kSameTimingLengthGapMs
            && sameTiming(result.document.lines, parseLrc(synced->get_ref<const std::string&>()))) {
          result.recordingDurationMs = 0;
          break;
        }
      }
    }
  } catch (const nlohmann::json::exception&) {
    result.status = LyricsStatus::ERROR;
  }
  return result;
}

std::vector<std::string> splitLyricsLines(std::string_view text) {
  std::vector<std::string> lines;
  if (text.starts_with("\xEF\xBB\xBF")) {
    text.remove_prefix(3);
  }
  while (!text.empty()) {
    const auto end = text.find('\n');
    auto line = text.substr(0, end);
    if (line.ends_with('\r')) {
      line.remove_suffix(1);
    }
    lines.emplace_back(line);
    if (end == std::string_view::npos) {
      break;
    }
    text.remove_prefix(end + 1);
  }
  return lines;
}

std::vector<LyricsLine> parseLrc(std::string_view text) {
  std::vector<LyricsLine> lines;
  std::int64_t offsetMs = 0;
  for (const auto& raw : splitLyricsLines(text)) {
    std::string_view line = raw;
    if (line.starts_with("[offset:") && line.ends_with(']')) {
      auto value = line.substr(8, line.size() - 9);
      if (value.starts_with('+')) {
        value.remove_prefix(1);
      }
      if (const auto offset = integer(value); offset && *offset >= -86400000 && *offset <= 86400000) {
        offsetMs = *offset;
      }
      continue;
    }
    std::vector<std::int64_t> times;
    while (line.starts_with('[')) {
      const auto end = line.find(']');
      if (end == std::string_view::npos) {
        break;
      }
      const auto time = timestamp(line.substr(1, end - 1));
      if (!time) {
        break;
      }
      times.push_back(*time);
      line.remove_prefix(end + 1);
    }
    while (line.starts_with(' ') || line.starts_with('\t')) {
      line.remove_prefix(1);
    }
    for (const auto time : times) {
      lines.push_back({.timeMs = time, .text = std::string(line)});
    }
  }
  // Positive LRC offsets advance the lyrics relative to the audio.
  for (auto& line : lines) {
    line.timeMs -= offsetMs;
  }
  std::ranges::stable_sort(lines, [](const LyricsLine& a, const LyricsLine& b) { return a.timeMs < b.timeMs; });
  return lines;
}

std::optional<std::size_t> LyricsDocument::activeLine(std::int64_t positionMs) const {
  const auto next = std::ranges::upper_bound(lines, positionMs, {}, &LyricsLine::timeMs);
  if (next == lines.begin()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(std::distance(lines.begin(), next) - 1);
}

LyricsWindow LyricsDocument::window(std::int64_t positionMs, std::size_t lineCount) const {
  return windowAround(activeLine(positionMs), lineCount);
}

LyricsWindow LyricsDocument::windowAround(std::optional<std::size_t> active, std::size_t lineCount) const {
  LyricsWindow result{.lines = std::vector<std::string>(lineCount)};
  if (lineCount == 0) {
    return result;
  }
  if (lines.empty()) {
    std::copy_n(plainLines.begin(), std::min(lineCount, plainLines.size()), result.lines.begin());
    return result;
  }
  const auto middle = (lineCount - 1) / 2;
  // Before the first line the opening lyric waits in the middle row, unhighlighted.
  const auto current = active ? static_cast<std::ptrdiff_t>(*active) : 0;
  for (std::size_t i = 0; i < lineCount; ++i) {
    const auto index = current + static_cast<std::ptrdiff_t>(i) - static_cast<std::ptrdiff_t>(middle);
    if (index >= 0 && static_cast<std::size_t>(index) < lines.size()) {
      result.lines[i] = lines[static_cast<std::size_t>(index)].text;
    }
  }
  if (active) {
    result.activeRow = middle;
  }
  return result;
}
