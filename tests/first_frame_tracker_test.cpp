#include "core/first_frame_tracker.h"
#include "test_check.h"

#include <optional>

int main() {
  {
    FirstFrameTracker tracker;
    int reports = 0;
    tracker.setCallback([&reports]() { ++reports; });
    TEST_CHECK(tracker.done());
    TEST_CHECK(reports == 0);
  }
  {
    FirstFrameTracker tracker;
    int reports = 0;
    tracker.setCallback([&reports]() { ++reports; });
    FirstFrameTracker::Member a(tracker);
    FirstFrameTracker::Member b(tracker);
    a.shown();
    TEST_CHECK(!tracker.done());
    TEST_CHECK(reports == 0);
    b.shown();
    TEST_CHECK(tracker.done());
    TEST_CHECK(reports == 1);
    a.shown();
    FirstFrameTracker::Member late(tracker);
    late.shown();
    TEST_CHECK(reports == 1);
  }
  {
    FirstFrameTracker tracker;
    int reports = 0;
    tracker.setCallback([&reports]() { ++reports; });
    FirstFrameTracker::Member a(tracker);
    std::optional<FirstFrameTracker::Member> b(std::in_place, tracker);
    a.shown();
    b.reset();
    TEST_CHECK(tracker.done());
    TEST_CHECK(reports == 1);
  }
  {
    FirstFrameTracker tracker;
    int reports = 0;
    tracker.setCallback([&reports]() { ++reports; });
    std::optional<FirstFrameTracker::Member> a(std::in_place, tracker);
    a.reset();
    TEST_CHECK(tracker.done());
    TEST_CHECK(reports == 1);
  }
  return 0;
}
