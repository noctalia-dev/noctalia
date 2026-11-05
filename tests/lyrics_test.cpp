#include "dbus/mpris/mpris_service.h"
#include "system/lyrics.h"
#include "system/lyrics_service.h"
#include "test_check.h"

#include <cstdint>
#include <glib.h>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>

int main() {
  MprisPlayerInfo identity;
  identity.busName = "org.mpris.MediaPlayer2.test";
  identity.trackId = "/track/1";
  identity.title = "Song";
  identity.artists = {"Artist", "Guest"};
  identity.lengthUs = 180000000;
  const auto identityKey = LyricsService::trackKey(identity);
  auto updated = identity;
  updated.album = "Album";
  updated.lengthUs += 1000;
  TEST_CHECK(LyricsService::trackKey(updated) == identityKey);
  updated = identity;
  updated.busName = "org.mpris.MediaPlayer2.other";
  TEST_CHECK(LyricsService::trackKey(updated) != identityKey);
  updated = identity;
  updated.trackId = "/track/2";
  TEST_CHECK(LyricsService::trackKey(updated) != identityKey);
  updated = identity;
  updated.title = "Next song";
  TEST_CHECK(LyricsService::trackKey(updated) != identityKey);
  updated = identity;
  updated.artists = {"Artist, Guest"};
  TEST_CHECK(LyricsService::trackKey(updated) != identityKey);
  identity.trackId.clear();
  updated = identity;
  updated.title = "A different stream track";
  TEST_CHECK(LyricsService::trackKey(updated) != LyricsService::trackKey(identity));

  // Chromium swaps in a hover-preview video's title while the real video keeps playing with the same length.
  MprisPlayerInfo riot;
  riot.busName = "org.mpris.MediaPlayer2.chromium";
  riot.trackId = "/org/chromium/MediaPlayer2/TrackList/Track63DC";
  riot.title = "Tech N9ne - Riot Maker (Feat. Skatterman & Snug Brim) | OFFICIAL AUDIO";
  riot.lengthUs = 334821000;
  riot.playbackStatus = "Playing";
  auto preview = riot;
  preview.title = "Atzenkalle - Weekend Diss aus der VBT Splash 2012 Final";
  preview.positionUs = 36420415;
  TEST_CHECK(LyricsService::continuesTrack(preview, riot.trackId, riot.title, riot.lengthUs, 36000000));
  // A real change restarts playback, even when a player repeats the previous length.
  preview.positionUs = 500000;
  TEST_CHECK(!LyricsService::continuesTrack(preview, riot.trackId, riot.title, riot.lengthUs, 36000000));
  preview.positionUs = 36420415;
  TEST_CHECK(!LyricsService::continuesTrack(preview, riot.trackId, riot.title, riot.lengthUs));
  TEST_CHECK(!LyricsService::continuesTrack(preview, riot.trackId, riot.title, 200000000, 36000000));
  TEST_CHECK(!LyricsService::continuesTrack(preview, "/other", riot.title, riot.lengthUs, 36000000));
  preview.playbackStatus = "Paused";
  preview.positionUs = 90000000;
  TEST_CHECK(LyricsService::continuesTrack(preview, riot.trackId, riot.title, riot.lengthUs));

  const auto lines = parseLrc(
      "[ar:Artist]\r\n[00:12.345]Third\r\n[00:01.5][00:05.25]Repeated\r\n"
      "[00:07]\r\n[00:10.00]日本語\r\n"
      "[00:60.00]Invalid seconds\n[bad]Not a lyric\n[00:00.1234]Invalid fraction\n"
  );
  TEST_CHECK(lines.size() == 5);
  TEST_CHECK(lines[0].timeMs == 1500);
  TEST_CHECK(lines[0].text == "Repeated");
  TEST_CHECK(lines[1].timeMs == 5250);
  TEST_CHECK(lines[1].text == "Repeated");
  TEST_CHECK(lines[2].timeMs == 7000);
  TEST_CHECK(lines[2].text.empty());
  TEST_CHECK(lines[3].timeMs == 10000);
  TEST_CHECK(lines[3].text == "日本語");
  TEST_CHECK(lines[4].timeMs == 12345);
  TEST_CHECK(lines[4].text == "Third");

  LyricsDocument document{.lines = lines};
  TEST_CHECK(!document.activeLine(0));
  TEST_CHECK(!document.activeLine(1499));
  TEST_CHECK(document.activeLine(1500) == 0);
  TEST_CHECK(document.activeLine(5249) == 0);
  TEST_CHECK(document.activeLine(5250) == 1);
  TEST_CHECK(document.activeLine(7000) == 2);
  TEST_CHECK(document.activeLine(12000) == 3);
  TEST_CHECK(document.activeLine(12345) == 4);
  TEST_CHECK(document.activeLine(std::numeric_limits<std::int64_t>::max()) == 4);
  // Backwards seeks reselect the earlier lyric immediately.
  TEST_CHECK(document.activeLine(1600) == 0);

  const auto intro = document.window(0, 3);
  TEST_CHECK(intro.lines.size() == 3);
  TEST_CHECK(!intro.activeRow);
  TEST_CHECK(intro.lines[0].empty());
  TEST_CHECK(intro.lines[1] == "Repeated");
  TEST_CHECK(intro.lines[2] == "Repeated");
  const auto first = document.window(1500, 3);
  TEST_CHECK(first.activeRow == 1);
  TEST_CHECK(first.lines[0].empty());
  TEST_CHECK(first.lines[1] == "Repeated");
  const auto single = document.window(10000, 1);
  TEST_CHECK(single.lines.size() == 1);
  TEST_CHECK(single.activeRow == 0);
  TEST_CHECK(single.lines[0] == "日本語");
  TEST_CHECK(document.window(0, 1).lines[0] == "Repeated");
  TEST_CHECK(!document.window(0, 1).activeRow);
  const auto last = document.window(20000, 4);
  TEST_CHECK(last.lines.size() == 4);
  TEST_CHECK(last.activeRow == 1);
  TEST_CHECK(last.lines[0] == "日本語");
  TEST_CHECK(last.lines[1] == "Third");
  TEST_CHECK(last.lines[2].empty());
  TEST_CHECK(last.lines[3].empty());
  TEST_CHECK(document.window(1500, 0).lines.empty());

  const auto offset = parseLrc("[00:01.00]First\n[offset:+250]\n[00:02.00]Second");
  TEST_CHECK(offset.size() == 2);
  TEST_CHECK(offset[0].timeMs == 750);
  TEST_CHECK(offset[1].timeMs == 1750);
  const auto negativeOffset = parseLrc("[offset:-250]\n[00:00]Intro");
  TEST_CHECK(negativeOffset.size() == 1);
  TEST_CHECK(negativeOffset[0].timeMs == 250);
  const auto duplicate = parseLrc("[00:01]A\n[00:01]B\n[00:02]C");
  TEST_CHECK(duplicate.size() == 3);
  TEST_CHECK(duplicate[0].text == "A");
  TEST_CHECK(duplicate[1].text == "B");
  TEST_CHECK(LyricsDocument{.lines = duplicate}.activeLine(1000) == 1);
  TEST_CHECK(parseLrc("[999999999999999999999:00]Overflow\n[00:-1]Negative\n[00:00.]Empty").empty());
  TEST_CHECK(!LyricsDocument{}.activeLine(0));
  const auto bom = parseLrc("\xEF\xBB\xBF[00:01.00]First");
  TEST_CHECK(bom.size() == 1);
  TEST_CHECK(bom[0].text == "First");
  const auto plain = splitLyricsLines("First\r\n\r\nLast\n");
  TEST_CHECK(plain.size() == 3);
  TEST_CHECK(plain[0] == "First");
  TEST_CHECK(plain[1].empty());
  TEST_CHECK(plain[2] == "Last");
  const auto unsynced = LyricsDocument{.plainLines = plain}.window(10000, 2);
  TEST_CHECK(unsynced.lines.size() == 2);
  TEST_CHECK(!unsynced.activeRow);
  TEST_CHECK(unsynced.lines[0] == "First");
  TEST_CHECK(unsynced.lines[1].empty());

  const LyricsTrack video{.title = "Look the Other Way", .artist = "Beartooth", .durationUs = 187301000};
  const auto record = [](std::string_view title, std::string_view artist, double duration, std::string_view synced) {
    return nlohmann::json{
        {"trackName", title},    {"artistName", artist},          {"albumName", "Album"},   {"duration", duration},
        {"instrumental", false}, {"plainLyrics", "Plain lyrics"}, {"syncedLyrics", synced},
    };
  };
  const auto candidates = nlohmann::json::array({
      record("Look The Other Way", "Beartooth", 187.0, ""),
      record("Look The Other Way", "Beartooth", 230.0, "[00:01]Wrong duration"),
      record("Look The Other Way (live)", "Beartooth", 187.0, "[00:01]Wrong recording"),
      record("Look The Other Way", "Cover Artist", 187.0, "[00:01]Wrong performer"),
      record("Look The Other Way", "Beartooth", 185.0, "[00:01]Outside two seconds"),
      record("Look The Other Way", "Beartooth", 187.0, "[00:01]Matched"),
      record("Look The Other Way", "Beartooth", 186.0, "[00:01]Less precise"),
  });
  const auto matched = parseLrclibSearch(candidates.dump(), video);
  TEST_CHECK(matched.status == LyricsStatus::READY);
  TEST_CHECK(matched.document.lines.size() == 1);
  TEST_CHECK(matched.document.lines[0].text == "Matched");
  const auto wrongRecording = nlohmann::json::array({candidates[1], candidates[2], candidates[3]});
  TEST_CHECK(parseLrclibSearch(wrongRecording.dump(), video).status == LyricsStatus::NOT_FOUND);
  // A video cut a few seconds longer still matches when nothing closer exists.
  const auto looseTiming = nlohmann::json::array({record("Look The Other Way", "Beartooth", 200.0, "[00:01]Loose")});
  TEST_CHECK(parseLrclibSearch(looseTiming.dump(), video).status == LyricsStatus::READY);
  const auto remaster =
      nlohmann::json::array({record("Look The Other Way - Remastered 2011", "Beartooth", 187.0, "[00:01]Remaster")});
  TEST_CHECK(parseLrclibSearch(remaster.dump(), video).status == LyricsStatus::READY);
  TEST_CHECK(parseLrclibSearch("[]", video).status == LyricsStatus::NOT_FOUND);
  TEST_CHECK(parseLrclibSearch("not json", video).status == LyricsStatus::ERROR);
  TEST_CHECK(parseLrclibSearch("{}", video).status == LyricsStatus::ERROR);
  TEST_CHECK(parseLrclibSearch("[{}]", video).status == LyricsStatus::ERROR);
  const auto plainMatch = parseLrclibSearch(nlohmann::json::array({candidates[0]}).dump(), video);
  TEST_CHECK(plainMatch.status == LyricsStatus::READY);
  TEST_CHECK(plainMatch.document.lines.empty());
  TEST_CHECK(plainMatch.document.plainLines.size() == 1);
  const auto plainAlbum = nlohmann::json::array({candidates[0], candidates[5]});
  auto trackWithAlbum = video;
  trackWithAlbum.album = "Album";
  TEST_CHECK(parseLrclibSearch(plainAlbum.dump(), trackWithAlbum).document.lines.size() == 1);
  // A file whose timing is identical to another length's entry does not vouch for its own length.
  const std::string smackLrc =
      "[00:01.97]Shady!\n[00:03.53]Konvict!\n[00:05.46]Upfront!\n[00:07.55]Akon!\n[00:16.47]I feel you creepin'";
  const LyricsTrack smack{.title = "Smack That", .artist = "Akon", .durationUs = 251541000};
  const auto mislabelled = nlohmann::json::array({
      record("Smack That", "Akon", 252.0, smackLrc),
      record("Smack That", "Akon", 213.0, smackLrc),
  });
  const auto smackResult = parseLrclibSearch(mislabelled.dump(), smack);
  TEST_CHECK(smackResult.status == LyricsStatus::READY);
  TEST_CHECK(smackResult.recordingDurationMs == 0);
  TEST_CHECK(parseLrclibSearch(nlohmann::json::array({mislabelled[0]}).dump(), smack).recordingDurationMs == 252000);
  const auto cyrillic = record("ПЕСНЯ", "МУЗЫКАНТ", 180.0, "[00:01]Слова");
  const auto unicodeMatch = parseLrclibSearch(
      nlohmann::json::array({cyrillic}).dump(), {.title = "песня", .artist = "музыкант", .durationUs = 180000000}
  );
  TEST_CHECK(unicodeMatch.status == LyricsStatus::READY);
  const auto punctuation = record("Don’t Stop", "Björk", 180.0, "[00:01]Words");
  TEST_CHECK(
      parseLrclibSearch(
          nlohmann::json::array({punctuation}).dump(),
          {.title = "Don't Stop", .artist = "BJÖRK", .durationUs = 180000000}
      )
          .status
      == LyricsStatus::READY
  );

  const std::string wordsJson =
      R"({"track":{"title":"Different catalog spelling","artist":"Artist","duration":4,"isrc":"TEST00000001"},"meta":{"level":"word"},"lines":[{"text":"Hello 世界","start":1000,"end":4000,"words":[{"text":"Hello","start":1000,"end":2000},{"text":"世界","start":2500,"end":3500}]}]})";
  const auto karaoke = parseLrcmuxResponse(wordsJson, {}, "TEST00000001");
  TEST_CHECK(karaoke.status == LyricsStatus::READY);
  TEST_CHECK(karaoke.document.lines.size() == 1);
  const auto& sung = karaoke.document.lines[0];
  TEST_CHECK(sung.words.size() == 2);
  TEST_CHECK(sung.words[0].startByte == 0);
  TEST_CHECK(sung.words[0].endByte == 5);
  TEST_CHECK(sung.words[1].startByte == 6);
  TEST_CHECK(sung.words[1].endByte == 12);
  TEST_CHECK(sung.progress(500, {}).segment == 0);
  TEST_NEAR(sung.progress(500, {}).fraction, 0.0F, 0.0001F);
  TEST_NEAR(sung.progress(1500, {}).fraction, 0.5F, 0.0001F);
  TEST_CHECK(sung.progress(2200, {}).segment == 1);
  TEST_NEAR(sung.progress(2200, {}).fraction, 0.0F, 0.0001F);
  TEST_CHECK(sung.progress(3000, {}).segment == 1);
  TEST_NEAR(sung.progress(3000, {}).fraction, 0.5F, 0.0001F);
  TEST_CHECK(sung.progress(4000, {}).segment == 2);
  // Seeking backwards restores the first word's partial fill.
  TEST_CHECK(sung.progress(1500, {}).segment == 0);
  TEST_NEAR(sung.progress(1500, {}).fraction, 0.5F, 0.0001F);
  TEST_CHECK(parseLrcmuxResponse(wordsJson, {}, "OTHER0000001").status == LyricsStatus::NOT_FOUND);
  TEST_CHECK(parseLrcmuxResponse("{}", {}, "TEST00000001").status == LyricsStatus::ERROR);
  // NetEase word-timed lyrics; JSON credit rows are skipped and punctuation segments keep their own timing.
  const auto neteaseYrc = nlohmann::json{
      {"code", 200},
      {"lrc", {{"lyric", "[00:00.000] 作词 : Someone\n[00:14.89]Ugh, I don't"}}},
      {"yrc",
       {{"lyric",
         "{\"t\":0,\"c\":[{\"tx\":\"作词: \"}]}\n"
         "[14940,1560](14940,60,0)Ugh(15000,90,0), (15090,120,0)I (15210,210,0)don't (15420,180,0)(yeah)"}}},
  };
  const auto netease = parseNeteaseLyric(neteaseYrc.dump());
  TEST_CHECK(netease.status == LyricsStatus::READY);
  TEST_CHECK(netease.document.lines.size() == 1);
  const auto& neteaseLine = netease.document.lines[0];
  TEST_CHECK(neteaseLine.timeMs == 14940);
  TEST_CHECK(neteaseLine.endMs == 16500);
  TEST_CHECK(neteaseLine.text == "Ugh, I don't (yeah)");
  TEST_CHECK(neteaseLine.words.size() == 5);
  TEST_CHECK(neteaseLine.words[0].startMs == 14940);
  TEST_CHECK(neteaseLine.words[0].endMs == 15000);
  TEST_CHECK(
      neteaseLine.text.substr(
          neteaseLine.words[0].startByte, neteaseLine.words[0].endByte - neteaseLine.words[0].startByte
      )
      == "Ugh"
  );
  TEST_CHECK(
      neteaseLine.text.substr(
          neteaseLine.words[4].startByte, neteaseLine.words[4].endByte - neteaseLine.words[4].startByte
      )
      == "(yeah)"
  );
  // Without word timing it falls back to the timed lines, dropping credit rows.
  const auto neteaseLrc = parseNeteaseLyric(
      nlohmann::json{{"code", 200}, {"lrc", {{"lyric", "[00:00.000] 作词 : Someone\n[00:14.89]Ugh, I don't"}}}}.dump()
  );
  TEST_CHECK(neteaseLrc.status == LyricsStatus::READY);
  TEST_CHECK(neteaseLrc.document.lines.size() == 1);
  TEST_CHECK(neteaseLrc.document.lines[0].timeMs == 14890);
  TEST_CHECK(neteaseLrc.document.lines[0].words.empty());
  TEST_CHECK(parseNeteaseLyric(nlohmann::json{{"code", 200}, {"pureMusic", true}}.dump()).document.instrumental);
  TEST_CHECK(
      parseNeteaseLyric(nlohmann::json{{"code", 200}, {"lrc", {{"lyric", ""}}}}.dump()).status
      == LyricsStatus::NOT_FOUND
  );
  TEST_CHECK(parseNeteaseLyric(nlohmann::json{{"code", 404}}.dump()).status == LyricsStatus::ERROR);
  TEST_CHECK(parseNeteaseLyric("not json").status == LyricsStatus::ERROR);
  // LyricsPlus: word timing from syllables (background vocals skipped) and the recording length.
  const auto lyricsPlus = nlohmann::json{
      {"type", "Word"},
      {"metadata", {{"title", "Bohemian Rhapsody"}, {"artist", "Queen"}, {"totalDuration", "5:54.320"}}},
      {"lyrics",
       nlohmann::json::array({{
           {"time", 0},
           {"duration", 2906},
           {"text", "Is this the real life?"},
           {"syllabus",
            nlohmann::json::array({
                {{"time", 0}, {"duration", 458}, {"text", "Is "}},
                {{"time", 458}, {"duration", 348}, {"text", "this "}},
                {{"time", 900}, {"duration", 100}, {"text", "(ooh) "}, {"isBackground", true}},
                {{"time", 2057}, {"duration", 849}, {"text", "life?"}},
            })},
       }})},
  };
  const LyricsTrack queen{.title = "Bohemian Rhapsody", .artist = "Queen", .durationUs = 354000000};
  const auto plus = parseLyricsPlusResponse(lyricsPlus.dump(), queen);
  TEST_CHECK(plus.status == LyricsStatus::READY);
  TEST_CHECK(plus.recordingDurationMs == 354320);
  TEST_CHECK(plus.document.lines.size() == 1);
  TEST_CHECK(plus.document.lines[0].text == "Is this life?");
  TEST_CHECK(plus.document.lines[0].words.size() == 3);
  TEST_CHECK(plus.document.lines[0].words[2].startMs == 2057);
  TEST_CHECK(plus.document.lines[0].endMs == 2906);
  TEST_CHECK(
      parseLyricsPlusResponse(lyricsPlus.dump(), {.title = "Other Song", .artist = "Queen"}).status
      == LyricsStatus::NOT_FOUND
  );
  TEST_CHECK(parseLyricsPlusResponse(R"({"error":{"status":404}})", queen).status == LyricsStatus::NOT_FOUND);

  // KuGou: base64 LRC with a "title - artist" row and credits first, HTML-escaped text.
  const std::string kugouLrc = "[00:00.03]Starboy - The Weeknd\n[00:05.20]Lyrics by："
                               "Abel\n[00:16.47]I&apos;m tryna put you in the worst mood\n";
  std::unique_ptr<gchar, decltype(&g_free)> encoded(
      g_base64_encode(reinterpret_cast<const guchar*>(kugouLrc.data()), kugouLrc.size()), g_free
  );
  const auto kugou = parseKugouLyric(nlohmann::json{{"content", encoded.get()}}.dump());
  TEST_CHECK(kugou.status == LyricsStatus::READY);
  TEST_CHECK(kugou.document.lines.size() == 1);
  TEST_CHECK(kugou.document.lines[0].timeMs == 16470);
  TEST_CHECK(kugou.document.lines[0].text == "I'm tryna put you in the worst mood");
  TEST_CHECK(parseKugouLyric(R"({"content":""})").status == LyricsStatus::NOT_FOUND);

  const LyricsLine lineTiming{.timeMs = 1000, .text = "Line", .endMs = 3000};
  TEST_NEAR(lineTiming.progress(2000, {}).fraction, 0.5F, 0.0001F);
  TEST_NEAR(lineTiming.progress(4000, {}).fraction, 1.0F, 0.0001F);
  // A sweep without word timing finishes early (at most half the line) so the line is complete when replaced.
  TEST_NEAR(lineTiming.progress(2800, {}, 400).fraction, 1.0F, 0.0001F);
  TEST_NEAR(lineTiming.progress(2000, {}, 400).fraction, 1000.0F / 1600.0F, 0.0001F);
  TEST_NEAR(lineTiming.progress(2000, {}, 5000).fraction, 1.0F, 0.0001F);
  const LyricsLine noEnd{.timeMs = 1000, .text = "Line"};
  TEST_NEAR(noEnd.progress(2000, 3000).fraction, 0.5F, 0.0001F);
  TEST_CHECK(noEnd.progress(2000, {}).segment == 1);
  return 0;
}
