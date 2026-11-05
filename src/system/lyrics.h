#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct LyricsWord {
  std::size_t startByte = 0;
  std::size_t endByte = 0;
  std::int64_t startMs = 0;
  std::int64_t endMs = 0;
};

struct LyricsProgress {
  std::size_t segment = 0;
  float fraction = 0.0F;
};

struct LyricsLine {
  std::int64_t timeMs = 0;
  std::string text;
  std::optional<std::int64_t> endMs;
  std::vector<LyricsWord> words;

  // For lines without word timing the sweep finishes `finishEarlyMs` before the line ends (at most half its length),
  // so the line is fully filled by the time it is replaced.
  LyricsProgress
  progress(std::int64_t positionMs, std::optional<std::int64_t> nextLineMs, std::int64_t finishEarlyMs = 0) const;
};

struct LyricsWindow {
  std::vector<std::string> lines;
  std::optional<std::size_t> activeRow;
};

struct LyricsDocument {
  std::vector<LyricsLine> lines;
  std::vector<std::string> plainLines;
  bool instrumental = false;

  std::optional<std::size_t> activeLine(std::int64_t positionMs) const;
  LyricsWindow window(std::int64_t positionMs, std::size_t lineCount) const;
  // The same window centred on a given line instead of a playback position.
  LyricsWindow windowAround(std::optional<std::size_t> active, std::size_t lineCount) const;
};

struct LyricsTrack {
  std::string title;
  std::string artist;
  std::string album;
  std::int64_t durationUs = 0;
};

enum class LyricsStatus : std::uint8_t { READY, NOT_FOUND, OFFLINE, ERROR };

struct LyricsResult {
  LyricsStatus status = LyricsStatus::NOT_FOUND;
  LyricsDocument document;
  // Length of the recording the timestamps were made for; 0 when the source does not say.
  std::int64_t recordingDurationMs = 0;
};

LyricsResult parseLrclibSearch(std::string_view body, const LyricsTrack& track);
LyricsResult parseLrcmuxResponse(std::string_view body, const LyricsTrack& track, std::string_view expectedIsrc);
// NetEase `song/lyric/v1` response: word-timed `yrc` when present, otherwise timed or plain `lrc`.
LyricsResult parseNeteaseLyric(std::string_view body);
// LyricsPlus `v2/lyrics/get` response, accepted only when its metadata names the same track.
LyricsResult parseLyricsPlusResponse(std::string_view body, const LyricsTrack& track);
// KuGou `download?fmt=lrc` response: base64 LRC with a "title - artist" row and credits before the lyrics.
LyricsResult parseKugouLyric(std::string_view body);
std::vector<LyricsLine> parseLrc(std::string_view text);
std::vector<std::string> splitLyricsLines(std::string_view text);