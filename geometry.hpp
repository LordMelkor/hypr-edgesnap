#pragma once

// Pure snap geometry. Zone detection, zone rectangles and preview easing are
// adapted from omarchy-windows' contrib/aero-snap (MIT, see LICENSE).

#include <algorithm>
#include <optional>
#include <string_view>

namespace EdgeSnap {

    struct Point {
        double x = 0;
        double y = 0;
    };

    struct Rect {
        double x = 0;
        double y = 0;
        double w = 0;
        double h = 0;
    };

    enum class Zone {
        None,
        Left,
        LeftHalf,
        Center,
        Right,
        RightHalf,
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight,
        Maximize,
    };

    inline int columnsForMonitor(const Rect monitor, const std::string_view configuredColumns) {
        if (configuredColumns == "2")
            return 2;
        if (configuredColumns == "3")
            return 3;

        return monitor.w > 0 && monitor.h > 0 && monitor.w * 9.0 > monitor.h * 16.0 ? 3 : 2;
    }

    inline double outQuart(const double progress) {
        const double remaining = 1.0 - std::clamp(progress, 0.0, 1.0);
        return 1.0 - remaining * remaining * remaining * remaining;
    }

    inline double inCubic(const double progress) {
        const double clamped = std::clamp(progress, 0.0, 1.0);
        return clamped * clamped * clamped;
    }

    inline Rect interpolateRect(const Rect from, const Rect to, const double progress) {
        const double clamped = std::clamp(progress, 0.0, 1.0);
        return {
            from.x + (to.x - from.x) * clamped,
            from.y + (to.y - from.y) * clamped,
            from.w + (to.w - from.w) * clamped,
            from.h + (to.h - from.h) * clamped,
        };
    }

    inline Rect scaleRectFromCenter(const Rect rect, const double scale) {
        const double clamped = std::max(0.0, scale);
        const double width   = rect.w * clamped;
        const double height  = rect.h * clamped;
        return {
            rect.x + (rect.w - width) / 2.0,
            rect.y + (rect.h - height) / 2.0,
            width,
            height,
        };
    }

    inline Zone zoneAt(const Point cursor, const Rect monitor, const double edgeThreshold, const double cornerRatio, const int columns, const bool leftQuarters = true,
                       const bool rightQuarters = true) {
        if (monitor.w <= 0 || monitor.h <= 0 || edgeThreshold < 0)
            return Zone::None;

        const auto ratio          = std::clamp(cornerRatio, 0.0, 0.45);
        const bool nearLeft       = cursor.x <= monitor.x + edgeThreshold;
        const bool nearRight      = cursor.x >= monitor.x + monitor.w - edgeThreshold;
        const bool nearTop        = cursor.y <= monitor.y + edgeThreshold;
        const bool nearBottom     = cursor.y >= monitor.y + monitor.h - edgeThreshold;
        const bool inLeftRegion   = cursor.x <= monitor.x + monitor.w * ratio;
        const bool inRightRegion  = cursor.x >= monitor.x + monitor.w * (1.0 - ratio);
        const bool inTopRegion    = cursor.y <= monitor.y + monitor.h * ratio;
        const bool inBottomRegion = cursor.y >= monitor.y + monitor.h * (1.0 - ratio);

        if (!leftQuarters && (nearLeft || ((nearTop || nearBottom) && inLeftRegion)))
            return Zone::LeftHalf;
        if (!rightQuarters && (nearRight || ((nearTop || nearBottom) && inRightRegion)))
            return Zone::RightHalf;
        if (leftQuarters && ((nearTop && inLeftRegion) || (nearLeft && inTopRegion)))
            return Zone::TopLeft;
        if (rightQuarters && ((nearTop && inRightRegion) || (nearRight && inTopRegion)))
            return Zone::TopRight;
        if (nearBottom && columns == 3) {
            if (cursor.x < monitor.x + monitor.w / 3.0)
                return Zone::Left;
            if (cursor.x > monitor.x + monitor.w * 2.0 / 3.0)
                return Zone::Right;
            return Zone::Center;
        }
        if (leftQuarters && ((nearBottom && inLeftRegion) || (nearLeft && inBottomRegion)))
            return Zone::BottomLeft;
        if (rightQuarters && ((nearBottom && inRightRegion) || (nearRight && inBottomRegion)))
            return Zone::BottomRight;
        if (nearTop)
            return Zone::Maximize;
        if (nearLeft)
            return Zone::Left;
        if (nearRight)
            return Zone::Right;

        return Zone::None;
    }

    inline std::optional<Rect> rectForZone(const Rect workArea, const Zone zone, const double horizontalGap, const double verticalGap, const int columns) {
        if (workArea.w <= 0 || workArea.h <= 0 || horizontalGap < 0 || verticalGap < 0)
            return std::nullopt;

        if (zone == Zone::Maximize)
            return workArea;

        const int    columnCount = columns == 3 ? 3 : 2;
        const double columnWidth = (workArea.w - horizontalGap * (columnCount - 1)) / columnCount;
        const double halfWidth   = (workArea.w - horizontalGap) / 2.0;
        const double rowHeight   = (workArea.h - verticalGap) / 2.0;
        if (columnWidth <= 0 || halfWidth <= 0 || rowHeight <= 0)
            return std::nullopt;

        switch (zone) {
            case Zone::Left: return Rect{workArea.x, workArea.y, columnWidth, workArea.h};
            case Zone::LeftHalf: return Rect{workArea.x, workArea.y, halfWidth, workArea.h};
            case Zone::Center:
                if (columnCount != 3)
                    return std::nullopt;
                return Rect{workArea.x + columnWidth + horizontalGap, workArea.y, columnWidth, workArea.h};
            case Zone::Right: return Rect{workArea.x + (columnWidth + horizontalGap) * (columnCount - 1), workArea.y, columnWidth, workArea.h};
            case Zone::RightHalf: return Rect{workArea.x + halfWidth + horizontalGap, workArea.y, halfWidth, workArea.h};
            case Zone::TopLeft: return Rect{workArea.x, workArea.y, halfWidth, rowHeight};
            case Zone::TopRight: return Rect{workArea.x + halfWidth + horizontalGap, workArea.y, halfWidth, rowHeight};
            case Zone::BottomLeft: return Rect{workArea.x, workArea.y + rowHeight + verticalGap, halfWidth, rowHeight};
            case Zone::BottomRight: return Rect{workArea.x + halfWidth + horizontalGap, workArea.y + rowHeight + verticalGap, halfWidth, rowHeight};
            case Zone::None:
            case Zone::Maximize: return std::nullopt;
        }

        return std::nullopt;
    }

    struct Extents {
        double left = 0, top = 0, right = 0, bottom = 0;
    };

    // Moves a client box (inner geometry plus decoration extents) fully into
    // an area, shrinking it only when it cannot fit. Used for keyboard
    // restore, so a remembered box from a disconnected or rearranged monitor,
    // or one that would put the titlebar under the bar, stays reachable.
    inline Rect clampIntoArea(const Rect client, const Extents ext, const Rect area) {
        const double maxW = std::max(1.0, area.w - ext.left - ext.right);
        const double maxH = std::max(1.0, area.h - ext.top - ext.bottom);
        const double w    = std::min(client.w, maxW);
        const double h    = std::min(client.h, maxH);
        const double minX = area.x + ext.left, maxX = area.x + area.w - ext.right - w;
        const double minY = area.y + ext.top, maxY = area.y + area.h - ext.bottom - h;
        return {std::clamp(client.x, minX, std::max(minX, maxX)), std::clamp(client.y, minY, std::max(minY, maxY)), w, h};
    }

    // Where a snapped window goes when a drag pulls it out of its zone: the
    // remembered client size, placed so the cursor keeps the same relative
    // spot on the titlebar (horizontally proportional, kept away from the
    // ends; vertically the same offset, limited to the top quarter).
    inline Rect anchoredRestore(const Point cursor, const Rect currentOuter, const Extents ext, const double restoreW, const double restoreH) {
        const double anchorX = std::clamp((cursor.x - currentOuter.x) / std::max(1.0, currentOuter.w), 0.08, 0.92);
        const double anchorY = std::clamp(cursor.y - currentOuter.y, 0.0, std::max(0.0, currentOuter.h * 0.25));
        const double outerW  = restoreW + ext.left + ext.right;
        return {cursor.x - outerW * anchorX + ext.left, cursor.y - anchorY + ext.top, restoreW, restoreH};
    }

} // namespace EdgeSnap
