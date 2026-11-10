// hypr-edgesnap: drag-to-edge snap zones, keyboard halves and size restore on
// drag-away for floating windows.
//
// Adapted from omarchy-windows' contrib/aero-snap (MIT, see LICENSE), reduced
// to snapping only: no border resizing, workspace management, cascading,
// focus-border handling or marker files. See STATE.md for the state machines.
//
// Ownership: Hyprland owns the interactive drag, fullscreen/maximize and its
// restore geometry. This plugin owns snap destinations, the preview and one
// record per snapped window (its pre-snap geometry).

#define WLR_USE_UNSTABLE

#include "geometry.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/supplementary/DragController.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <lua.hpp>

#include <linux/input-event-codes.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

    using EdgeSnap::Zone;
    using PreviewClock = std::chrono::steady_clock;

    // Cursor travel before a drag may detach a snapped window. Hyprland's
    // drag_threshold is 0 on Omarchy, so a click with a pixel of jitter on a
    // snapped titlebar must not restore its size.
    constexpr double DETACH_DISTANCE = 6.0;

    // X11 apps that move themselves through hypr-xcsd get the drag's release
    // forwarded over Wayland, while our geometry change reaches the X server
    // over the window manager's X11 connection, which is flushed first. Moving
    // the window before Xwayland has delivered that release sends it to
    // whatever is under the pointer then, and the app keeps thinking the button
    // is held. Give the release this long to land first.
    constexpr auto X11_APPLY_DELAY = std::chrono::milliseconds{30};

    HANDLE pluginHandle = nullptr;

    struct SConfigValues {
        SP<Config::Values::CBoolValue>   enabled;
        SP<Config::Values::CStringValue> columns;
        SP<Config::Values::CBoolValue>   quarters;
        SP<Config::Values::CBoolValue>   gaps;
        SP<Config::Values::CIntValue>    edgeThreshold;
        SP<Config::Values::CFloatValue>  cornerRatio;
        SP<Config::Values::CColorValue>  previewColor;
        SP<Config::Values::CColorValue>  previewBorderColor;
        SP<Config::Values::CIntValue>    previewBorderSize;
        SP<Config::Values::CIntValue>    previewRounding;
        SP<Config::Values::CBoolValue>   previewBlur;
        SP<Config::Values::CIntValue>    previewAnimationDuration;
    };

    struct SPlacement {
        SP<Layout::ITarget> target;
        Zone                zone = Zone::None;
        CBox                previewBox;
        CBox                clientBox;
    };

    struct SPreview {
        PHLMONITORREF             monitor;
        CBox                      fromBox;
        CBox                      targetBox;
        PreviewClock::time_point  startedAt;
        std::chrono::milliseconds duration{0};
        float                     fromOpacity   = 0.F;
        float                     targetOpacity = 1.F;
    };

    struct SPreviewFrame {
        CBox  box;
        float opacity = 0.F;
        bool  settled = true;
    };

    // One per snapped window. restoreBox is the geometry from before the
    // first snap and is never overwritten by later snaps. A Maximize record
    // is only meaningful while the window is still maximized.
    struct SSnapRecord {
        PHLWINDOWREF window;
        CBox         restoreBox;
        Vector2D     monitorOrigin; // origin of the monitor restoreBox was on
        CBox         snappedBox;
        Zone         zone = Zone::None;
    };

    struct SDragState {
        SP<Layout::ITarget>       target;
        CBox                      startBox;
        Vector2D                  startCursor;
        bool                      detachDecided = false;
        std::optional<SPlacement> candidate;
    };

    struct SPluginState {
        SConfigValues             config;
        std::optional<SPreview>   preview;
        std::optional<SDragState> drag;
        std::vector<SSnapRecord>  records;
        UP<SEventLoopDoLaterLock> pendingApply;
        SP<CEventLoopTimer>       x11Timer; // one timer for the plugin's lifetime
        std::function<void()>     x11Pending;
        CHyprSignalListener       mouseMoveListener;
        CHyprSignalListener       mouseButtonListener;
        CHyprSignalListener       renderListener;
        CHyprSignalListener       windowDestroyListener;
    };

    UP<SPluginState> state;

    EdgeSnap::Rect toRect(const CBox& box) {
        return {box.x, box.y, box.w, box.h};
    }

    CBox toBox(const EdgeSnap::Rect rect) {
        CBox box{rect.x, rect.y, rect.w, rect.h};
        box.round();
        return box;
    }

    bool sameBox(const CBox& lhs, const CBox& rhs) {
        return lhs.x == rhs.x && lhs.y == rhs.y && lhs.w == rhs.w && lhs.h == rhs.h;
    }

    EdgeSnap::Extents extentsOf(const PHLWINDOW& window) {
        const auto e = window->getWindowExtentsUnified(Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS);
        return {e.topLeft.x, e.topLeft.y, e.bottomRight.x, e.bottomRight.y};
    }

    bool isMaximized(const PHLWINDOW& window) {
        return Fullscreen::controller()->isFullscreen(window, Fullscreen::FSMODE_MAXIMIZED);
    }

    bool isTrueFullscreen(const PHLWINDOW& window) {
        return Fullscreen::controller()->isFullscreen(window, Fullscreen::FSMODE_FULLSCREEN);
    }

    // Floating, mapped, on a regular workspace and not in a group: the same
    // set of windows the keyboard helper this replaces acted on.
    bool eligible(const PHLWINDOW& window) {
        return Desktop::View::validMapped(window) && window->m_target && window->m_target->floating() && !window->m_group && window->m_workspace &&
            window->workspaceID() > 0;
    }

    PHLWORKSPACE activeWorkspace(const PHLMONITOR& monitor) {
        return monitor->m_activeSpecialWorkspace ? monitor->m_activeSpecialWorkspace : monitor->m_activeWorkspace;
    }

    // ---- preview -----------------------------------------------------------

    void damagePreviewBox(const CBox& box) {
        constexpr double padding = 12;
        g_pHyprRenderer->damageBox({box.x - padding, box.y - padding, box.w + padding * 2, box.h + padding * 2});
    }

    int configuredDuration() {
        return std::max(0, static_cast<int>(state->config.previewAnimationDuration->value()));
    }

    SPreviewFrame previewFrame(const SPreview& preview, const PreviewClock::time_point now) {
        const double elapsed  = std::chrono::duration<double, std::milli>(now - preview.startedAt).count();
        const double duration = static_cast<double>(preview.duration.count());
        const double progress = duration <= 0.0 ? 1.0 : std::clamp(elapsed / duration, 0.0, 1.0);
        const double eased    = preview.targetOpacity < preview.fromOpacity ? EdgeSnap::inCubic(progress) : EdgeSnap::outQuart(progress);

        return {
            toBox(EdgeSnap::interpolateRect(toRect(preview.fromBox), toRect(preview.targetBox), eased)),
            static_cast<float>(preview.fromOpacity + (preview.targetOpacity - preview.fromOpacity) * eased),
            progress >= 1.0,
        };
    }

    std::chrono::milliseconds previewDuration(const CBox& fromBox, const CBox& targetBox, const PHLMONITOR& monitor, const double opacityDistance) {
        const int configured = configuredDuration();
        if (configured == 0 || !monitor)
            return std::chrono::milliseconds{0};

        const double reference   = std::max(1.0, monitor->logicalBox().w / 3.0);
        const double boxDistance = std::max({
            std::abs(targetBox.x - fromBox.x),
            std::abs(targetBox.y - fromBox.y),
            std::abs(targetBox.w - fromBox.w),
            std::abs(targetBox.h - fromBox.h),
        });
        const double distance = std::clamp(std::max(boxDistance / reference, opacityDistance), 0.1, 1.0);
        return std::chrono::milliseconds{std::max(1, static_cast<int>(std::round(configured * distance)))};
    }

    void clearPreview() {
        if (!state || !state->preview || state->preview->targetOpacity <= 0.F)
            return;

        const auto now        = PreviewClock::now();
        const auto current    = previewFrame(*state->preview, now);
        const CBox target     = toBox(EdgeSnap::scaleRectFromCenter(toRect(current.box), 0.985));
        const int  configured = configuredDuration();
        const auto duration =
            configured == 0 ? std::chrono::milliseconds{0} : std::chrono::milliseconds{std::max(1, static_cast<int>(std::round(configured * 0.625 * current.opacity)))};

        damagePreviewBox(current.box);
        damagePreviewBox(target);
        state->preview = SPreview{state->preview->monitor, current.box, target, now, duration, current.opacity, 0.F};
    }

    void setPreview(const PHLMONITOR& monitor, const CBox& box) {
        if (state->preview && state->preview->monitor.lock() == monitor && sameBox(state->preview->targetBox, box) && state->preview->targetOpacity >= 1.F)
            return;

        const auto now = PreviewClock::now();
        if (state->preview && state->preview->monitor.lock() == monitor) {
            const auto current  = previewFrame(*state->preview, now);
            const auto duration = previewDuration(current.box, box, monitor, 1.0 - current.opacity);
            damagePreviewBox(current.box);
            damagePreviewBox(box);
            state->preview = SPreview{monitor, current.box, box, now, duration, current.opacity, 1.F};
            return;
        }

        if (state->preview) {
            const auto current = previewFrame(*state->preview, now);
            damagePreviewBox(current.box);
            damagePreviewBox(state->preview->targetBox);
        }

        const CBox from = toBox(EdgeSnap::scaleRectFromCenter(toRect(box), 0.96));
        state->preview  = SPreview{monitor, from, box, now, std::chrono::milliseconds{configuredDuration()}, 0.F, 1.F};
        damagePreviewBox(from);
        damagePreviewBox(box);
    }

    // ---- snap records ------------------------------------------------------

    void forgetRecord(const PHLWINDOW& window) {
        std::erase_if(state->records, [&](const auto& record) { return record.window.expired() || record.window.lock() == window; });
    }

    SSnapRecord* recordFor(const PHLWINDOW& window) {
        std::erase_if(state->records, [](const auto& record) { return record.window.expired(); });
        const auto found = std::ranges::find_if(state->records, [&](const auto& record) { return record.window.lock() == window; });
        return found == state->records.end() ? nullptr : &*found;
    }

    // The record that still describes this window. maximizedBefore is whether
    // the window was maximized before the caller changed anything: a Maximize
    // record whose maximization has since ended (titlebar button, native drag
    // restore) is stale and is dropped.
    SSnapRecord* liveRecordFor(const PHLWINDOW& window, const bool maximizedBefore) {
        const auto record = recordFor(window);
        if (record && record->zone == Zone::Maximize && !maximizedBefore) {
            forgetRecord(window);
            return nullptr;
        }
        return record;
    }

    Vector2D monitorOriginFor(const PHLWINDOW& window, const CBox& box) {
        for (const auto& monitor : State::monitorState()->monitors()) {
            if (monitor && monitor->logicalBox().containsPoint(box.middle()))
                return monitor->m_position;
        }
        const auto monitor = window->m_monitor.lock();
        return monitor ? monitor->m_position : Vector2D{};
    }

    void rememberSnap(const PHLWINDOW& window, const CBox& restoreBox, const CBox& snappedBox, const Zone zone) {
        if (const auto record = recordFor(window)) {
            record->snappedBox = snappedBox;
            record->zone       = zone;
            return;
        }
        state->records.push_back({window, restoreBox, monitorOriginFor(window, restoreBox), snappedBox, zone});
    }

    // The remembered box, moved with its monitor if that monitor moved and
    // onto the window's current monitor if it changed, then kept inside that
    // monitor's usable area (below the bar, titlebar included).
    CBox restoredBox(const PHLWINDOW& window, const SSnapRecord& record) {
        CBox       box     = record.restoreBox;
        const auto monitor = window->m_monitor.lock();
        if (!monitor || !window->m_workspace || !window->m_workspace->m_space)
            return box;

        box.translate(monitor->m_position - record.monitorOrigin);
        const CBox area = window->m_workspace->m_space->workArea(true);
        return toBox(EdgeSnap::clampIntoArea(toRect(box), extentsOf(window), toRect(area)));
    }

    void setGeometry(const SP<Layout::ITarget>& target, const CBox& box) {
        g_layoutManager->setTargetGeom(box, target);
        target->warpPositionSize();
        target->damageEntire();
    }

    // ---- placement ---------------------------------------------------------

    std::optional<SPlacement> placementFor(const SP<Layout::ITarget>& target, const PHLWORKSPACE& workspace, const PHLMONITOR& monitor, const Zone zone) {
        if (!target || !monitor || !workspace || !workspace->m_space || zone == Zone::None || !eligible(target->window()))
            return std::nullopt;

        const bool gapsEnabled = state->config.gaps->value();
        // The floating work area keeps reserved regions (the bar) but not
        // gaps_out; the tiled one includes gaps_out.
        const CBox workArea = workspace->m_space->workArea(zone == Zone::Maximize || !gapsEnabled);
        const int  columns  = EdgeSnap::columnsForMonitor(toRect(monitor->logicalBox()), state->config.columns->value());

        static auto  gapsInValue   = CConfigValue<Config::IComplexConfigValue>("general:gaps_in");
        const auto*  gapsIn        = sc<Config::CCssGapData*>(gapsInValue.ptr());
        const double horizontalGap = gapsEnabled ? gapsIn->m_left + gapsIn->m_right : 0.0;
        const double verticalGap   = gapsEnabled ? gapsIn->m_top + gapsIn->m_bottom : 0.0;
        const auto   zoneRect      = EdgeSnap::rectForZone(toRect(workArea), zone, horizontalGap, verticalGap, columns);
        if (!zoneRect)
            return std::nullopt;

        const CBox requestedOuter = toBox(*zoneRect);
        if (zone == Zone::Maximize)
            return SPlacement{target, zone, requestedOuter, {}};

        const auto     ext = extentsOf(target->window());
        const Vector2D availableSize{requestedOuter.w - ext.left - ext.right, requestedOuter.h - ext.top - ext.bottom};
        const Vector2D minSize         = target->minSize().value_or(Vector2D{1, 1});
        const Vector2D reportedMaxSize = target->maxSize().value_or(Math::VECTOR2D_MAX);
        const Vector2D maxSize{std::max(minSize.x, reportedMaxSize.x), std::max(minSize.y, reportedMaxSize.y)};

        if (availableSize.x < minSize.x || availableSize.y < minSize.y)
            return std::nullopt;

        const Vector2D clientSize{std::clamp(availableSize.x, minSize.x, maxSize.x), std::clamp(availableSize.y, minSize.y, maxSize.y)};
        const Vector2D outerSize{clientSize.x + ext.left + ext.right, clientSize.y + ext.top + ext.bottom};
        CBox           outer{
            requestedOuter.x + (requestedOuter.w - outerSize.x) / 2.0,
            requestedOuter.y + (requestedOuter.h - outerSize.y) / 2.0,
            outerSize.x,
            outerSize.y,
        };
        CBox client{outer.x + ext.left, outer.y + ext.top, clientSize.x, clientSize.y};
        outer.round();
        client.round();
        return SPlacement{target, zone, outer, client};
    }

    void applyPlacement(const SPlacement& placement, const CBox& restoreBox) {
        const auto target = placement.target;
        if (!target || !eligible(target->window()))
            return;

        const auto window = target->window();
        if (placement.zone == Zone::Maximize) {
            Fullscreen::controller()->setFullscreenMode(window, Fullscreen::FSMODE_MAXIMIZED);
            rememberSnap(window, restoreBox, placement.previewBox, placement.zone);
            return;
        }

        if (Fullscreen::controller()->isFullscreen(window))
            Fullscreen::controller()->setFullscreenMode(window, Fullscreen::FSMODE_NONE);
        setGeometry(target, placement.clientBox);
        rememberSnap(window, restoreBox, placement.clientBox, placement.zone);
    }

    // ---- keyboard ----------------------------------------------------------

    PHLWINDOW focusedWindow() {
        const auto focus = Desktop::focusState();
        return focus ? focus->window() : nullptr;
    }

    // Win+Up. Native maximize, exactly like the titlebar button: Hyprland
    // keeps the restore geometry and handles drag-away. Creates no record and
    // keeps any half-snap record for a later Win+Down.
    bool keyboardMaximize() {
        const auto window = focusedWindow();
        if (!eligible(window) || isTrueFullscreen(window))
            return false;
        if (!isMaximized(window))
            Fullscreen::controller()->setFullscreenMode(window, Fullscreen::FSMODE_MAXIMIZED);
        return true;
    }

    // Win+Down. Leaves maximize/fullscreen natively, then returns a snapped
    // window to its pre-snap geometry and forgets the record.
    bool keyboardRestore() {
        const auto window = focusedWindow();
        if (!eligible(window))
            return false;

        const bool wasMaximized = isMaximized(window);
        if (Fullscreen::controller()->isFullscreen(window))
            Fullscreen::controller()->setFullscreenMode(window, Fullscreen::FSMODE_NONE);

        const auto record = liveRecordFor(window, wasMaximized);
        if (!record)
            return true;

        const CBox box = restoredBox(window, *record);
        forgetRecord(window);
        setGeometry(window->m_target, box);
        return true;
    }

    // Win+Left/Right: halves regardless of the mouse column setting.
    bool keyboardSnap(const Zone zone) {
        const auto window = focusedWindow();
        if (!eligible(window) || isTrueFullscreen(window))
            return false;

        const auto target       = window->m_target;
        const bool wasMaximized = isMaximized(window);
        // Leave maximize first: the restored decorations decide the client
        // size, and Hyprland's own restore geometry is the pre-snap box.
        if (wasMaximized)
            Fullscreen::controller()->setFullscreenMode(window, Fullscreen::FSMODE_NONE);

        const auto placement = placementFor(target, window->m_workspace, window->m_monitor.lock(), zone);
        if (!placement) {
            if (wasMaximized) // leave the window as it was
                Fullscreen::controller()->setFullscreenMode(window, Fullscreen::FSMODE_MAXIMIZED);
            return false;
        }

        const auto record     = liveRecordFor(window, wasMaximized);
        const CBox restoreBox = record ? record->restoreBox : target->position();
        applyPlacement(*placement, restoreBox);
        return true;
    }

    int luaResult(lua_State* L, const bool handled) {
        lua_pushboolean(L, handled);
        return 1;
    }

    int luaLeft(lua_State* L) {
        return luaResult(L, state && state->config.enabled->value() && keyboardSnap(Zone::LeftHalf));
    }

    int luaRight(lua_State* L) {
        return luaResult(L, state && state->config.enabled->value() && keyboardSnap(Zone::RightHalf));
    }

    int luaMaximize(lua_State* L) {
        return luaResult(L, state && state->config.enabled->value() && keyboardMaximize());
    }

    int luaRestore(lua_State* L) {
        return luaResult(L, state && state->config.enabled->value() && keyboardRestore());
    }

    // ---- mouse -------------------------------------------------------------

    void resetDrag() {
        clearPreview();
        state->drag.reset();
    }

    // A snapped window that is dragged out of its zone takes back its
    // pre-snap size under the cursor. The native drag is ended and restarted
    // on the same window (not an exclusive grab), so whoever started it -
    // Super+drag, the Omarchy titlebar or hypr-xcsd - still ends it on release.
    bool detachSnapped(const SP<Layout::ITarget>& target, const Vector2D cursor, Event::SCallbackInfo& info) {
        const auto window = target->window();
        const auto record = recordFor(window);
        // Only while it still has the snapped size: a window resized by hand
        // since its snap keeps that size, and its record stays for Win+Down.
        if (!record || record->zone == Zone::Maximize || Fullscreen::controller()->isFullscreen(window) || state->drag->startBox.w != record->snappedBox.w ||
            state->drag->startBox.h != record->snappedBox.h)
            return false;

        const auto ext     = extentsOf(window);
        const CBox current = target->position();
        const CBox currentOuter{current.x - ext.left, current.y - ext.top, current.w + ext.left + ext.right, current.h + ext.top + ext.bottom};
        const CBox restore =
            toBox(EdgeSnap::anchoredRestore({cursor.x, cursor.y}, toRect(currentOuter), ext, record->restoreBox.w, record->restoreBox.h));

        forgetRecord(window);
        clearPreview();
        g_layoutManager->endDragTarget();
        g_layoutManager->setTargetGeom(restore, target);
        target->warpPositionSize();
        g_layoutManager->beginDragTarget(target, MBIND_MOVE);
        state->drag = SDragState{target, restore, cursor, true, std::nullopt};
        info.cancelled = true; // this motion must not also move the new drag
        return true;
    }

    void onMouseMove(const Vector2D cursor, Event::SCallbackInfo& info) {
        if (!state)
            return;

        const auto& controller = g_layoutManager->dragController();
        const auto  target     = controller->target();
        if (!state->config.enabled->value() || !target || controller->mode() != MBIND_MOVE || controller->draggingTiled() || !eligible(target->window())) {
            resetDrag();
            return;
        }

        const auto window = target->window();
        if (!state->drag || state->drag->target != target) {
            state->drag = SDragState{target, target->position(), cursor, false, std::nullopt};
            liveRecordFor(window, isMaximized(window)); // drop a stale Maximize record
        }

        if (!controller->dragThresholdReached() || (!state->drag->detachDecided && cursor.distance(state->drag->startCursor) < DETACH_DISTANCE)) {
            clearPreview();
            state->drag->candidate.reset();
            return;
        }

        if (!state->drag->detachDecided) {
            state->drag->detachDecided = true;
            if (detachSnapped(target, cursor, info))
                return;
        }

        const auto monitor = State::monitorState()->query().vec(cursor).run();
        if (!monitor) {
            clearPreview();
            state->drag->candidate.reset();
            return;
        }

        const auto monitorBox = monitor->logicalBox();
        const int  columns    = EdgeSnap::columnsForMonitor(toRect(monitorBox), state->config.columns->value());
        const bool quarters   = state->config.quarters->value();
        const auto zone = EdgeSnap::zoneAt({cursor.x, cursor.y}, toRect(monitorBox), state->config.edgeThreshold->value(), state->config.cornerRatio->value(), columns,
                                           quarters, quarters);
        const auto placement = placementFor(target, activeWorkspace(monitor), monitor, zone);
        if (!placement) {
            clearPreview();
            state->drag->candidate.reset();
            return;
        }

        state->drag->candidate = placement;
        setPreview(monitor, placement->previewBox);
    }

    // Runs fn after the release that ended a drag has been handled: on the
    // next loop turn, or for X11 windows after X11_APPLY_DELAY. A newer
    // release replaces anything still pending.
    void afterRelease(const PHLWINDOW& window, std::function<void()> fn) {
        state->pendingApply.reset();
        state->x11Pending = nullptr;
        state->x11Timer->updateTimeout(std::nullopt);
        if (!window || !window->m_isX11) {
            state->pendingApply = g_pEventLoopManager->doLaterLock(std::move(fn));
            return;
        }
        state->x11Pending = std::move(fn);
        state->x11Timer->updateTimeout(X11_APPLY_DELAY);
    }

    void onMouseButton(const IPointer::SButtonEvent event, Event::SCallbackInfo&) {
        if (!state || event.button != BTN_LEFT || event.state != WL_POINTER_BUTTON_STATE_RELEASED || !state->drag)
            return;

        const auto drag = *state->drag;
        resetDrag();

        // Released before the detach distance: a click on a snapped titlebar
        // with a little jitter. Hyprland moved it by those pixels (no drag
        // threshold); put it back so it stays exactly snapped.
        const auto window = drag.target ? drag.target->window() : nullptr;
        if (!drag.detachDecided) {
            afterRelease(window, [target = drag.target] {
                if (!state || !target || !eligible(target->window()) || Fullscreen::controller()->isFullscreen(target->window()))
                    return;
                const auto record = recordFor(target->window());
                const CBox current = target->position();
                if (record && record->zone != Zone::Maximize && current.w == record->snappedBox.w && current.h == record->snappedBox.h &&
                    !sameBox(current, record->snappedBox))
                    setGeometry(target, record->snappedBox);
            });
            return;
        }
        if (!drag.candidate)
            return;

        // Whoever owns the drag (Hyprland, the titlebar, hypr-xcsd) ends it on
        // this same release; apply after that, outside the input handler.
        afterRelease(window, [placement = *drag.candidate, restoreBox = drag.startBox] {
            if (!state || !state->config.enabled->value() || !placement.target || !eligible(placement.target->window()))
                return;
            if (g_layoutManager->dragController()->target() == placement.target)
                g_layoutManager->endDragTarget();
            applyPlacement(placement, restoreBox);
        });
    }

    void onRenderStage(const eRenderStage stage) {
        if (!state || stage != RENDER_POST_WINDOWS || !state->preview)
            return;

        const auto monitor = g_pHyprRenderer->renderData().pMonitor.lock();
        if (!monitor || state->preview->monitor.lock() != monitor)
            return;

        const auto frame = previewFrame(*state->preview, PreviewClock::now());
        if (frame.settled && state->preview->targetOpacity <= 0.F) {
            damagePreviewBox(frame.box);
            state->preview.reset();
            return;
        }

        if (!frame.settled) {
            damagePreviewBox(frame.box);
            damagePreviewBox(state->preview->targetBox);
        }

        CBox outer = frame.box;
        outer.translate(-monitor->m_position).scale(monitor->m_scale).round();

        const float opacity  = std::clamp(frame.opacity, 0.F, 1.F);
        const int   border   = std::max(0, static_cast<int>(std::round(state->config.previewBorderSize->value() * monitor->m_scale)));
        const int   rounding = std::max(0, static_cast<int>(std::round(state->config.previewRounding->value() * monitor->m_scale)));
        if (border > 0) {
            const CHyprColor            borderColor{static_cast<uint64_t>(state->config.previewBorderColor->value())};
            CRectPassElement::SRectData borderData;
            borderData.box   = outer;
            borderData.color = borderColor.modifyA(static_cast<float>(borderColor.a) * opacity);
            borderData.round = rounding;
            g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(borderData));
        }

        CBox inner{outer.x + border, outer.y + border, outer.w - border * 2, outer.h - border * 2};
        if (inner.w <= 0 || inner.h <= 0)
            return;

        const CHyprColor            fillColor{static_cast<uint64_t>(state->config.previewColor->value())};
        CRectPassElement::SRectData fillData;
        fillData.box   = inner;
        fillData.color = fillColor.modifyA(static_cast<float>(fillColor.a) * opacity);
        fillData.round = std::max(0, rounding - border);
        fillData.blur  = state->config.previewBlur->value();
        fillData.blurA = 0.48F * opacity;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(fillData));
    }

    template <typename T, typename... Args>
    SP<T> configValue(Args&&... args) {
        auto value = makeShared<T>(std::forward<Args>(args)...);
        HyprlandAPI::addConfigValueV2(pluginHandle, value);
        return value;
    }

    void registerConfig() {
        using namespace Config::Values;
        auto& c   = state->config;
        c.enabled = configValue<CBoolValue>("plugin:edgesnap:enabled", "Snap floating windows to screen edges and restore them when dragged away", true);
        c.columns = configValue<CStringValue>("plugin:edgesnap:columns", "Full-height drag columns: 2, 3, or auto (3 above 16:9)", "2",
                                              SStringValueOptions{.validator = [](const Config::STRING& value) -> std::expected<void, std::string> {
                                                  if (value == "auto" || value == "2" || value == "3")
                                                      return {};
                                                  return std::unexpected("expected auto, 2, or 3");
                                              }});
        c.quarters      = configValue<CBoolValue>("plugin:edgesnap:quarters", "Screen corners snap to quarters (otherwise to halves)", true);
        c.gaps          = configValue<CBoolValue>("plugin:edgesnap:gaps", "Keep gaps_out and gaps_in around snapped windows", true);
        c.edgeThreshold = configValue<CIntValue>("plugin:edgesnap:edge_threshold", "Logical pixels from a monitor edge that activate a zone", 12,
                                                 SIntValueOptions{.min = 1, .max = 128});
        c.cornerRatio   = configValue<CFloatValue>("plugin:edgesnap:corner_ratio", "Fraction of each monitor edge that belongs to a corner zone", 0.25F,
                                                   SFloatValueOptions{.min = 0.05F, .max = 0.45F});
        c.previewColor  = configValue<CColorValue>("plugin:edgesnap:preview_color", "Snap preview fill color", 0x331e3a8a);
        c.previewBorderColor = configValue<CColorValue>("plugin:edgesnap:preview_border_color", "Snap preview border color", 0xbd1d4ed8);
        c.previewBorderSize  = configValue<CIntValue>("plugin:edgesnap:preview_border_size", "Snap preview border size", 2, SIntValueOptions{.min = 0, .max = 16});
        c.previewRounding    = configValue<CIntValue>("plugin:edgesnap:preview_rounding", "Snap preview corner radius", 8, SIntValueOptions{.min = 0, .max = 64});
        c.previewBlur        = configValue<CBoolValue>("plugin:edgesnap:preview_blur", "Blur behind the snap preview", true);
        c.previewAnimationDuration =
            configValue<CIntValue>("plugin:edgesnap:preview_animation_duration", "Snap preview animation in milliseconds", 200, SIntValueOptions{.min = 0, .max = 1000});
    }

} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    pluginHandle = handle;

    if (std::string(__hyprland_api_get_hash()) != __hyprland_api_get_client_hash())
        throw std::runtime_error("hypr-edgesnap: built for a different Hyprland");

    state = makeUnique<SPluginState>();
    try {
        registerConfig();
        for (const auto& [name, fn] : {std::pair{"left", luaLeft}, {"right", luaRight}, {"maximize", luaMaximize}, {"restore", luaRestore}}) {
            if (!HyprlandAPI::addLuaFunction(handle, "edgesnap", name, fn))
                throw std::runtime_error(std::string{"hypr-edgesnap: cannot register hl.plugin.edgesnap."} + name);
        }
    } catch (...) {
        state.reset(); // Hyprland drops this plugin's config values and Lua functions
        throw;
    }

    state->x11Timer = makeShared<CEventLoopTimer>(
        std::nullopt,
        [](SP<CEventLoopTimer>, void*) {
            if (!state || !state->x11Pending)
                return;
            const auto fn = std::exchange(state->x11Pending, nullptr);
            fn();
        },
        nullptr);
    g_pEventLoopManager->addTimer(state->x11Timer);

    state->mouseMoveListener   = Event::bus()->m_events.input.mouse.move.listen([](Vector2D cursor, Event::SCallbackInfo& info) { onMouseMove(cursor, info); });
    state->mouseButtonListener = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent e, Event::SCallbackInfo& info) { onMouseButton(e, info); });
    state->renderListener      = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) { onRenderStage(stage); });
    state->windowDestroyListener = Event::bus()->m_events.window.destroy.listen([](PHLWINDOWREF window) {
        if (!state)
            return;
        std::erase_if(state->records, [&](const auto& record) { return record.window == window || record.window.expired(); });
        if (state->drag && state->drag->target && state->drag->target->window() == window.lock())
            resetDrag();
    });

    // Apply plugin:edgesnap:* values from the config, which was parsed before
    // these options existed.
    HyprlandAPI::reloadConfig();
    return {"hypr-edgesnap", "Edge snap zones and drag-away restore for floating windows", "LordMelkor", "0.1"};
}

// Unloading only stops snapping: windows keep their current geometry and the
// records are dropped (Win+Down then has nothing to restore).
APICALL EXPORT void PLUGIN_EXIT() {
    if (!state)
        return;
    state->pendingApply.reset();
    g_pEventLoopManager->removeTimer(state->x11Timer); // never fire into unloaded code
    state->x11Pending = nullptr;
    state->mouseMoveListener.reset();
    state->mouseButtonListener.reset();
    state->renderListener.reset();
    state->windowDestroyListener.reset();
    if (state->preview)
        damagePreviewBox(state->preview->targetBox);
    state.reset();
}
