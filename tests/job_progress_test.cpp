#include "notification/job_progress.h"

#include <cassert>
#include <map>
#include <string>

int main() {
  JobProgressManager manager;
  const auto first = manager.add(":1.20", "org.example.Copy", 3);
  const auto second = manager.add(":1.20", "org.example.Copy", 1);
  assert(first != second);
  assert(manager.jobs().size() == 2);

  const std::map<std::string, sdbus::Variant> update{
      {"title", sdbus::Variant(std::string{"Copying files"})},
      {"percent", sdbus::Variant(std::uint32_t{37})},
      {"processedBytes", sdbus::Variant(std::uint64_t{370})},
      {"totalBytes", sdbus::Variant(std::uint64_t{1000})},
      {"speed", sdbus::Variant(std::uint64_t{64})},
      {"elapsedTime", sdbus::Variant(std::int64_t{12})},
      {"suspended", sdbus::Variant(true)},
  };
  assert(manager.update(first, update));
  const auto* job = manager.find(first);
  assert(job != nullptr && job->title == "Copying files");
  assert(job->percent == 37 && job->processedBytes == 370 && job->totalBytes == 1000 && job->suspended);
  assert(job->elapsedTime == 12);
  assert(job->speedHistory == std::vector<std::uint64_t>{64});
  assert(manager.find(second)->title.empty());

  // Separate byte-derived display/ETA rates must never enter the raw graph or average statistic.
  const auto rawHistory = manager.find(first)->speedHistory;
  assert(manager.update(first, {{"displaySpeed", sdbus::Variant(std::uint64_t{100})},
      {"etaSpeed", sdbus::Variant(std::uint64_t{50})}}));
  assert(manager.find(first)->currentSpeed() == 100);
  assert(!manager.find(first)->remainingSeconds()); // Paused.
  assert(manager.find(first)->speedHistory == rawHistory && manager.find(first)->speed == 64);
  assert(manager.find(second)->currentSpeed() == std::nullopt);
  assert(manager.update(second, {{"speed", sdbus::Variant(std::uint64_t{32})}}));
  assert(manager.find(second)->currentSpeed() == 32); // Unchanged for other clients without byte windows.
  assert(manager.update(first, {{"suspended", sdbus::Variant(false)}}));
  assert(manager.find(first)->remainingSeconds() == 12);
  assert(manager.update(first, {{"etaSpeed", sdbus::Variant(std::uint64_t{0})}}));
  assert(!manager.find(first)->remainingSeconds());
  assert(manager.update(first, {{"etaSpeed", sdbus::Variant(std::uint64_t{50})},
      {"processedBytes", sdbus::Variant(std::uint64_t{1000})}}));
  assert(!manager.find(first)->remainingSeconds()); // Completion.
  assert(manager.update(first, {{"processedBytes", sdbus::Variant(std::uint64_t{370})},
      {"suspended", sdbus::Variant(true)}}));

  std::pair<std::uint32_t, JobProgressControl> requested{};
  manager.setControlCallback([](void* context, std::uint32_t id, JobProgressControl control) {
    auto* result = static_cast<std::pair<std::uint32_t, JobProgressControl>*>(context);
    result->first = id;
    result->second = control;
  }, &requested);
  manager.request(first, JobProgressControl::Resume);
  assert(requested.first == first && requested.second == JobProgressControl::Resume);
  assert(manager.find(first)->suspensionPending == false);
  assert(manager.update(first, {{"suspended", sdbus::Variant(false)}}));
  manager.request(first, JobProgressControl::Suspend);
  assert(manager.find(first)->suspensionPending == true);
  manager.request(first, JobProgressControl::Resume); // Pending request suppresses an opposite repeat.
  assert(requested.second == JobProgressControl::Suspend);
  assert(manager.update(first, {{"suspended", sdbus::Variant(false)}}));
  assert(!manager.find(first)->suspensionPending && !manager.find(first)->suspended);

  const std::map<std::string, sdbus::Variant> invalid{{"percent", sdbus::Variant(std::uint32_t{101})}};
  assert(!manager.update(first, invalid));
  assert(manager.find(first)->percent == 37);
  assert(!manager.finish(first, 0, std::string(4097, 'x')));
  assert(manager.finish(first, 1, "cancelled"));
  assert(manager.find(first)->terminated && manager.find(first)->errorCode == 1);
  manager.removeOwner(":1.20");
  assert(manager.jobs().empty());
  return 0;
}
