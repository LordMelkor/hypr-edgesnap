#pragma once
// Hyprland API that differs between 0.56 and 0.57, which split CWindow into
// backend/metadata/presentation parts, made workspace spaces private and passes
// the render context with render-stage events. main.cpp uses only these
// helpers, so it reads the same on both. Include after the Hyprland headers.

#ifdef EDGESNAP_HYPRLAND_057
#include <hyprland/src/desktop/view/window/WindowGroupMembership.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>

namespace EdgesnapCompat {
    inline bool grouped(const PHLWINDOW& w) {
        return !!w->grouping().group();
    }
    // A regular numbered workspace: not special, not named (negative IDs).
    inline bool regularWorkspace(const PHLWINDOW& w) {
        if (!w->m_workspace)
            return false;
        const auto id = w->m_workspace->id();
        const auto* numbered = std::get_if<Workspace::SWorkspaceNumberedID>(&id);
        return numbered && numbered->value > 0;
    }
    inline SP<Layout::CSpace> space(const PHLWORKSPACE& ws) {
        return ws ? ws->space() : nullptr;
    }
    inline bool isX11(const PHLWINDOW& w) {
        return w->backend().isX11();
    }

    using RenderStageEvent = Event::SRenderStageEvent;
    inline eRenderStage stage(const RenderStageEvent& e) {
        return e.stage;
    }
    inline PHLMONITOR renderMonitor(const RenderStageEvent& e) {
        return e.monitor;
    }
    inline void addPassElement(const RenderStageEvent& e, UP<IPassElement>&& element) {
        if (e.context)
            Render::IHyprRenderer::addPassElement(e.context->get(), std::move(element));
    }
}
#else
namespace EdgesnapCompat {
    inline bool grouped(const PHLWINDOW& w) {
        return !!w->m_group;
    }
    // A regular numbered workspace: not special, not named (negative IDs).
    inline bool regularWorkspace(const PHLWINDOW& w) {
        return w->m_workspace && w->workspaceID() > 0;
    }
    inline SP<Layout::CSpace> space(const PHLWORKSPACE& ws) {
        return ws ? ws->m_space : nullptr;
    }
    inline bool isX11(const PHLWINDOW& w) {
        return w->m_isX11;
    }

    using RenderStageEvent = eRenderStage;
    inline eRenderStage stage(const RenderStageEvent& e) {
        return e;
    }
    inline PHLMONITOR renderMonitor(const RenderStageEvent&) {
        return g_pHyprRenderer->renderData().pMonitor.lock();
    }
    inline void addPassElement(const RenderStageEvent&, UP<IPassElement>&& element) {
        g_pHyprRenderer->m_renderPass.add(std::move(element));
    }
}
#endif
