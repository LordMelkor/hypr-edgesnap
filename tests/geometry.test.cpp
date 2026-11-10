#include "geometry.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

    using EdgeSnap::Point;
    using EdgeSnap::Rect;
    using EdgeSnap::Zone;

    void expect(const bool condition, const std::string_view message) {
        if (condition)
            return;

        std::cerr << "geometry test failed: " << message << '\n';
        std::exit(1);
    }

    bool closeTo(const double actual, const double expected) {
        return std::abs(actual - expected) < 0.001;
    }

    void expectRect(const Rect actual, const Rect expected, const std::string_view message) {
        expect(closeTo(actual.x, expected.x) && closeTo(actual.y, expected.y) && closeTo(actual.w, expected.w) && closeTo(actual.h, expected.h), message);
    }

} // namespace

int main() {
    const Rect ultrawide{0, 0, 4096, 1152};
    const Rect widescreen{0, 0, 1920, 1080};


    // 2560x1080 (21:9) is the monitor this was first deployed on.
    expect(EdgeSnap::columnsForMonitor(Rect{0, 0, 2560, 1080}, "auto") == 3, "auto selects thirds on a 21:9 monitor");
    expect(EdgeSnap::columnsForMonitor(ultrawide, "auto") == 3, "auto selects thirds above 16:9");
    expect(EdgeSnap::columnsForMonitor(widescreen, "auto") == 2, "auto keeps exact 16:9 in halves");
    expect(EdgeSnap::columnsForMonitor(ultrawide, "2") == 2, "two columns can be forced on an ultrawide monitor");
    expect(EdgeSnap::columnsForMonitor(widescreen, "3") == 3, "three columns can be forced on a 16:9 monitor");

    expect(closeTo(EdgeSnap::outQuart(0.5), 0.9375), "OutQuart reaches the target without overshoot");
    expect(closeTo(EdgeSnap::inCubic(0.5), 0.125), "InCubic holds most motion until the end");
    expectRect(EdgeSnap::interpolateRect(Rect{0, 10, 100, 80}, Rect{200, 30, 300, 120}, 0.5), Rect{100, 20, 200, 100}, "preview boxes interpolate together");
    expectRect(EdgeSnap::scaleRectFromCenter(Rect{10, 20, 200, 100}, 0.96), Rect{14, 22, 192, 96}, "preview entrance scales around its center");

    expect(EdgeSnap::zoneAt(Point{0, 576}, ultrawide, 12, 0.25, 3) == Zone::Left, "left edge selects the outer column");
    expect(EdgeSnap::zoneAt(Point{4095, 100}, ultrawide, 12, 0.25, 3) == Zone::TopRight, "side-edge corner bands are generous");
    expect(EdgeSnap::zoneAt(Point{2048, 0}, ultrawide, 12, 0.25, 3) == Zone::Maximize, "top center maximizes");
    expect(EdgeSnap::zoneAt(Point{2048, 0}, ultrawide, 12, 0.5, 3) == Zone::Maximize, "the maximum corner ratio preserves a maximize band");
    expect(EdgeSnap::zoneAt(Point{100, 1151}, ultrawide, 12, 0.25, 3) == Zone::Left, "bottom-left third selects the left column");
    expect(EdgeSnap::zoneAt(Point{2048, 1151}, ultrawide, 12, 0.25, 3) == Zone::Center, "bottom center selects the middle column");
    expect(EdgeSnap::zoneAt(Point{3996, 1151}, ultrawide, 12, 0.25, 3) == Zone::Right, "bottom-right third selects the right column");
    expect(EdgeSnap::zoneAt(Point{0, 1000}, ultrawide, 12, 0.25, 3) == Zone::BottomLeft, "lower left side keeps the bottom-left quarter");
    expect(EdgeSnap::zoneAt(Point{4095, 1000}, ultrawide, 12, 0.25, 3) == Zone::BottomRight, "lower right side keeps the bottom-right quarter");
    expect(EdgeSnap::zoneAt(Point{100, 1151}, ultrawide, 12, 0.25, 2) == Zone::BottomLeft, "two-column mode keeps the bottom-left corner");
    expect(EdgeSnap::zoneAt(Point{2048, 1151}, ultrawide, 12, 0.25, 2) == Zone::None, "two-column mode leaves bottom center untouched");
    expect(EdgeSnap::zoneAt(Point{500, 500}, ultrawide, 12, 0.25, 3) == Zone::None, "interior movement is untouched");
    expect(EdgeSnap::zoneAt(Point{0, 100}, ultrawide, 12, 0.25, 3, false, true) == Zone::LeftHalf, "left half mode replaces the upper-left quarter");
    expect(EdgeSnap::zoneAt(Point{0, 1000}, ultrawide, 12, 0.25, 3, false, true) == Zone::LeftHalf, "left half mode replaces the lower-left quarter");
    expect(EdgeSnap::zoneAt(Point{4095, 100}, ultrawide, 12, 0.25, 3, true, false) == Zone::RightHalf, "right half mode replaces the upper-right quarter");
    expect(EdgeSnap::zoneAt(Point{4095, 1000}, ultrawide, 12, 0.25, 3, true, false) == Zone::RightHalf, "right half mode replaces the lower-right quarter");
    expect(EdgeSnap::zoneAt(Point{0, 100}, ultrawide, 12, 0.25, 3, true, false) == Zone::TopLeft, "left and right modes remain independent");

    const Rect workArea{10, 36, 4076, 1106};
    expectRect(*EdgeSnap::rectForZone(workArea, Zone::Left, 10, 10, 3), Rect{10, 36, 1352, 1106}, "left third preserves the horizontal gaps");
    expectRect(*EdgeSnap::rectForZone(workArea, Zone::Center, 10, 10, 3), Rect{1372, 36, 1352, 1106}, "middle third preserves the horizontal gaps");
    expectRect(*EdgeSnap::rectForZone(workArea, Zone::Right, 10, 10, 3), Rect{2734, 36, 1352, 1106}, "right third reaches the work area edge");
    expectRect(*EdgeSnap::rectForZone(workArea, Zone::Left, 10, 10, 2), Rect{10, 36, 2033, 1106}, "forced halves preserve the horizontal gap");
    expectRect(*EdgeSnap::rectForZone(workArea, Zone::LeftHalf, 10, 10, 3), Rect{10, 36, 2033, 1106}, "left half ignores the three-column layout");
    expectRect(*EdgeSnap::rectForZone(workArea, Zone::RightHalf, 10, 10, 3), Rect{2053, 36, 2033, 1106}, "right half ignores the three-column layout");
    expectRect(*EdgeSnap::rectForZone(workArea, Zone::BottomRight, 10, 10, 3), Rect{2053, 594, 2033, 548}, "quarters remain halves of the work area");
    expectRect(*EdgeSnap::rectForZone(workArea, Zone::Maximize, 10, 10, 3), workArea, "maximize uses the complete work area");
    expectRect(*EdgeSnap::rectForZone(workArea, Zone::Left, 0, 0, 3), Rect{10, 36, 4076.0 / 3.0, 1106}, "gapless columns consume the complete width");
    expectRect(*EdgeSnap::rectForZone(workArea, Zone::RightHalf, 0, 0, 3), Rect{2048, 36, 2038, 1106}, "gapless halves meet without spacing");
    expectRect(*EdgeSnap::rectForZone(workArea, Zone::BottomRight, 0, 0, 3), Rect{2048, 589, 2038, 553}, "gapless quarters meet without spacing");
    expect(!EdgeSnap::rectForZone(workArea, Zone::Center, 10, 10, 2), "center is unavailable in two-column mode");
    expect(!EdgeSnap::rectForZone(Rect{0, 0, 8, 8}, Zone::Left, 10, 10, 3), "impossible geometry is rejected");

    // Keyboard restore keeps a remembered box reachable. Area: 2560x1080 with
    // a 26px bar; extents: 2px border plus a 30px titlebar on top.
    const Rect                belowBar{0, 26, 2560, 1054};
    const EdgeSnap::Extents titled{2, 32, 2, 2};
    expectRect(EdgeSnap::clampIntoArea(Rect{300, 200, 800, 600}, titled, belowBar), Rect{300, 200, 800, 600}, "a box that fits is restored unchanged");
    expectRect(EdgeSnap::clampIntoArea(Rect{300, 30, 800, 600}, titled, belowBar), Rect{300, 58, 800, 600}, "the titlebar is kept below the bar");
    expectRect(EdgeSnap::clampIntoArea(Rect{3000, 200, 800, 600}, titled, belowBar), Rect{1758, 200, 800, 600}, "a box from a missing monitor area is brought back");
    expectRect(EdgeSnap::clampIntoArea(Rect{0, 0, 3000, 2000}, titled, belowBar), Rect{2, 58, 2556, 1020}, "an oversized box shrinks to the usable area");

    // Dragging a snapped window away restores its size under the cursor.
    const Rect leftHalfOuter{10, 36, 1260, 1034};
    expectRect(EdgeSnap::anchoredRestore(Point{640, 40}, leftHalfOuter, titled, 800, 600), Rect{240, 68, 800, 600},
               "the cursor keeps its relative spot on the titlebar");
    expect(closeTo(EdgeSnap::anchoredRestore(Point{12, 40}, leftHalfOuter, titled, 800, 600).x, 12 - 804 * 0.08 + 2),
           "a grab at the titlebar end keeps some of the window under the cursor");
    expect(closeTo(EdgeSnap::anchoredRestore(Point{640, 900}, leftHalfOuter, titled, 800, 600).y, 900 - 1034 * 0.25 + 32),
           "a grab low on the window keeps the titlebar near the cursor");

    return 0;
}
