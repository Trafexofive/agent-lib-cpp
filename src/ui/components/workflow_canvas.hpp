#pragma once
// Infinite cell-space workflow canvas.
// World coords in cells. Camera pans freely — no clamp to content bounds.
// Nodes + orthogonal edges + flow pulse. Not n8n spaghetti: layered DAG.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "inkcell/surface.hpp"
#include "inkcell/text.hpp"
#include "src/ui/components/workflow_rail.hpp"
#include "src/ui/model/workflow_run_model.hpp"
#include "src/ui/theme/cortex_theme.hpp"
#include "src/workflows/workflow.hpp"

namespace cortex::mk3::ui::components {

constexpr float kCanvasMinZoom = 0.4f;
constexpr float kCanvasMaxZoom = 2.5f;
constexpr float kCanvasZoomStep = 1.25f;

struct CanvasCamera {
    float x = 0.f;  // world cell at viewport left
    float y = 0.f;  // world cell at viewport top
    float zoom = 1.f;  // world cell -> screen cells

    void pan(float dx, float dy) {
        x += dx;
        y += dy;
    }

    // World point at viewport center.
    void worldCenter(int viewW, int viewH, float& wx, float& wy) const {
        float z = zoom > 1e-4f ? zoom : 1.f;
        wx = x + static_cast<float>(viewW) / (2.f * z);
        wy = y + static_cast<float>(viewH) / (2.f * z);
    }

    void centerOn(float wx, float wy, int viewW, int viewH) {
        float z = zoom > 1e-4f ? zoom : 1.f;
        x = wx - static_cast<float>(viewW) / (2.f * z);
        y = wy - static_cast<float>(viewH) / (2.f * z);
    }
};

// Zoom anchored at the viewport center (the world point under center stays put).
inline void zoomAround(CanvasCamera& cam, int viewW, int viewH, float factor) {
    float wx, wy;
    cam.worldCenter(viewW, viewH, wx, wy);
    cam.zoom = std::max(kCanvasMinZoom, std::min(kCanvasMaxZoom, cam.zoom * factor));
    cam.centerOn(wx, wy, viewW, viewH);
}

struct CanvasNode {
    std::string id;
    std::string label;
    std::string type;
    std::string ref;
    int wx = 0;  // world top-left
    int wy = 0;
    int ww = 22;
    int wh = 5;
    model::StepStatus status = model::StepStatus::Pending;
    double ms = 0.0;
    bool human = false;
    bool checkpoint = false;
    int depth = 0;
};

struct CanvasEdge {
    std::string from;
    std::string to;
    enum class Kind { Seq, Branch, Body, Catch } kind = Kind::Seq;
};

struct CanvasGraph {
    std::vector<CanvasNode> nodes;
    std::vector<CanvasEdge> edges;
    int worldW = 0;
    int worldH = 0;

    int indexOf(const std::string& id) const {
        for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
            if (nodes[static_cast<size_t>(i)].id == id) return i;
        return -1;
    }

    const CanvasNode* find(const std::string& id) const {
        int i = indexOf(id);
        return i < 0 ? nullptr : &nodes[static_cast<size_t>(i)];
    }

    CanvasNode* findMut(const std::string& id) {
        int i = indexOf(id);
        return i < 0 ? nullptr : &nodes[static_cast<size_t>(i)];
    }
};

namespace canvas_detail {

inline constexpr int kNodeW = 24;
inline constexpr int kNodeH = 5;
inline constexpr int kGapX = 6;
inline constexpr int kGapY = 2;

inline std::string uniqueId(const workflows::WorkflowStep& s, const std::string& path) {
    if (!s.id.empty()) return path.empty() ? s.id : path + "/" + s.id;
    return path + "/" + s.type + "@" + std::to_string(reinterpret_cast<uintptr_t>(&s) & 0xffff);
}

inline void addEdge(CanvasGraph& g, const std::string& from, const std::string& to,
                    CanvasEdge::Kind k) {
    if (from.empty() || to.empty() || from == to) return;
    CanvasEdge e;
    e.from = from;
    e.to = to;
    e.kind = k;
    g.edges.push_back(std::move(e));
}

// Layout a step list starting at (x,y). Returns bottom y after last node.
// prevId: sequential edge from previous sibling / parent chain.
inline int layoutSteps(CanvasGraph& g, const std::vector<workflows::WorkflowStep>& steps, int x,
                       int y, int depth, const std::string& path, const std::string& prevId,
                       CanvasEdge::Kind edgeKind) {
    std::string last = prevId;
    CanvasEdge::Kind ek = edgeKind;
    for (size_t i = 0; i < steps.size(); ++i) {
        const auto& s = steps[i];
        std::string id = uniqueId(s, path);
        // Prefer bare id at top level for status matching
        if (path.empty() && !s.id.empty()) id = s.id;

        CanvasNode n;
        n.id = id;
        n.label = !s.name.empty() ? s.name : (!s.id.empty() ? s.id : s.type);
        n.type = s.type;
        n.ref = model::stepRefOf(s);
        n.wx = x;
        n.wy = y;
        n.ww = kNodeW;
        n.wh = kNodeH;
        n.human = (s.type == "human");
        n.checkpoint = (s.type == "checkpoint");
        n.depth = depth;
        g.nodes.push_back(n);

        if (!last.empty()) addEdge(g, last, id, ek);
        last = id;
        ek = CanvasEdge::Kind::Seq;

        int below = y + kNodeH;
        int branchX = x + kNodeW + kGapX;
        int branchY = y;

        auto nest = [&](const std::vector<workflows::WorkflowStep>& kids, CanvasEdge::Kind knd,
                        int& cursorY) {
            if (kids.empty()) return;
            cursorY = layoutSteps(g, kids, branchX, cursorY, depth + 1, id, id, knd);
            cursorY += kGapY;
        };

        if (s.type == "condition") {
            int cy = branchY;
            nest(s.thenSteps, CanvasEdge::Kind::Branch, cy);
            nest(s.elseSteps, CanvasEdge::Kind::Branch, cy);
            below = std::max(below, cy);
        } else if (s.type == "switch") {
            int cy = branchY;
            for (const auto& kv : s.switchCases) nest(kv.second, CanvasEdge::Kind::Branch, cy);
            nest(s.switchDefault, CanvasEdge::Kind::Branch, cy);
            below = std::max(below, cy);
        } else if (s.type == "try_catch") {
            int cy = branchY;
            nest(s.tryBody, CanvasEdge::Kind::Body, cy);
            nest(s.catchBody, CanvasEdge::Kind::Catch, cy);
            nest(s.finallyBody, CanvasEdge::Kind::Body, cy);
            below = std::max(below, cy);
        } else if (s.type == "loop" || s.type == "map" || s.type == "reduce") {
            int cy = branchY;
            if (!s.body.empty())
                nest(s.body, CanvasEdge::Kind::Body, cy);
            else if (!s.steps.empty())
                nest(s.steps, CanvasEdge::Kind::Body, cy);
            below = std::max(below, cy);
        } else if (s.type == "parallel" || s.type == "parallel_join" || s.type == "parallel_race" ||
                   s.type == "workflow") {
            int cy = branchY;
            nest(s.steps, CanvasEdge::Kind::Body, cy);
            below = std::max(below, cy);
        }

        y = below + kGapY;
    }
    return y;
}

inline void finalizeBounds(CanvasGraph& g) {
    int maxX = 0, maxY = 0;
    for (const auto& n : g.nodes) {
        maxX = std::max(maxX, n.wx + n.ww);
        maxY = std::max(maxY, n.wy + n.wh);
    }
    g.worldW = maxX + 8;
    g.worldH = maxY + 8;
}

}  // namespace canvas_detail

inline CanvasGraph buildCanvasGraph(const workflows::WorkflowManifest& wf) {
    CanvasGraph g;
    canvas_detail::layoutSteps(g, wf.steps, 2, 1, 0, "", "", CanvasEdge::Kind::Seq);
    canvas_detail::finalizeBounds(g);
    return g;
}

// Apply live/preview step statuses onto graph nodes (top-level id match + nested path).
inline void applyRunStatusToGraph(CanvasGraph& g, const model::WorkflowRunState& run) {
    std::unordered_map<std::string, const model::WorkflowStepView*> byId;
    for (const auto& s : run.steps) byId[s.id] = &s;
    for (auto& n : g.nodes) {
        auto it = byId.find(n.id);
        if (it == byId.end()) {
            // nested ids like parent/child — match suffix
            auto slash = n.id.rfind('/');
            if (slash != std::string::npos) {
                it = byId.find(n.id.substr(slash + 1));
            }
        }
        if (it == byId.end()) continue;
        n.status = it->second->status;
        n.ms = it->second->ms;
    }
}

// World → screen
inline void worldToScreen(const CanvasCamera& cam, int wx, int wy, int& sx, int& sy) {
    float z = cam.zoom > 1e-4f ? cam.zoom : 1.f;
    sx = static_cast<int>(std::lround((static_cast<float>(wx) - cam.x) * z));
    sy = static_cast<int>(std::lround((static_cast<float>(wy) - cam.y) * z));
}

inline void screenToWorld(const CanvasCamera& cam, int sx, int sy, int& wx, int& wy) {
    float z = cam.zoom > 1e-4f ? cam.zoom : 1.f;
    wx = static_cast<int>(std::lround(static_cast<float>(sx) / z + cam.x));
    wy = static_cast<int>(std::lround(static_cast<float>(sy) / z + cam.y));
}

inline bool nodeVisible(const CanvasCamera& cam, const CanvasNode& n, int vw, int vh) {
    int sx, sy;
    worldToScreen(cam, n.wx, n.wy, sx, sy);
    float sw = static_cast<float>(n.ww) * cam.zoom;
    float sh = static_cast<float>(n.wh) * cam.zoom;
    if (sx + sw < 0 || sy + sh < 0) return false;
    if (sx >= vw || sy >= vh) return false;
    return true;
}

inline void drawHLine(inkcell::Surface& s, int x0, int x1, int y, int vx, int vy, int vw, int vh,
                      inkcell::Style st, const char* ch = "─") {
    if (y < 0 || y >= vh) return;
    if (x0 > x1) std::swap(x0, x1);
    for (int x = x0; x <= x1; ++x) {
        if (x < 0 || x >= vw) continue;
        s.text({vx + x, vy + y}, ch, st);
    }
}

inline void drawVLine(inkcell::Surface& s, int x, int y0, int y1, int vx, int vy, int vw, int vh,
                      inkcell::Style st, const char* ch = "│") {
    if (x < 0 || x >= vw) return;
    if (y0 > y1) std::swap(y0, y1);
    for (int y = y0; y <= y1; ++y) {
        if (y < 0 || y >= vh) continue;
        s.text({vx + x, vy + y}, ch, st);
    }
}

inline inkcell::Style edgeStyle(CanvasEdge::Kind k, bool flow, bool pulseOn) {
    if (flow) {
        auto st = pulseOn ? theme::cyan() : theme::amber();
        st.bold = true;
        return st;
    }
    switch (k) {
        case CanvasEdge::Kind::Branch:
            return theme::violet_soft();
        case CanvasEdge::Kind::Catch:
            return theme::amber_soft();
        case CanvasEdge::Kind::Body:
            return theme::muted();
        case CanvasEdge::Kind::Seq:
        default:
            return theme::dim();
    }
}

// Orthogonal edge: from bottom-center → to top-center (or side for branches).
inline void drawEdge(inkcell::Surface& s, inkcell::Rect view, const CanvasCamera& cam,
                     const CanvasNode& a, const CanvasNode& b, CanvasEdge::Kind kind, bool flow,
                     float tSec) {
    const float phase = tSec > 0.f ? std::fmod(tSec * 1.4f, 1.f) : 0.f;
    const bool pulseOn = phase < 0.5f;
    auto st = edgeStyle(kind, flow, pulseOn);

    int ax, ay, bx, by;
    // exit bottom-center of a, enter top-center of b for seq; side for branch rightward
    int awx = a.wx + a.ww / 2;
    int awy = a.wy + a.wh;
    int bwx = b.wx + b.ww / 2;
    int bwy = b.wy;
    if (kind != CanvasEdge::Kind::Seq && b.wx >= a.wx + a.ww) {
        awx = a.wx + a.ww;
        awy = a.wy + a.wh / 2;
        bwx = b.wx;
        bwy = b.wy + b.wh / 2;
    }

    worldToScreen(cam, awx, awy, ax, ay);
    worldToScreen(cam, bwx, bwy, bx, by);

    const int vx = view.x, vy = view.y, vw = view.w, vh = view.h;

    if (kind != CanvasEdge::Kind::Seq && b.wx >= a.wx + a.ww) {
        // horizontal then vertical then horizontal
        int midX = (ax + bx) / 2;
        drawHLine(s, ax, midX, ay, vx, vy, vw, vh, st);
        drawVLine(s, midX, ay, by, vx, vy, vw, vh, st);
        drawHLine(s, midX, bx, by, vx, vy, vw, vh, st);
        if (flow) {
            // flowing dot along path length approx
            int path = std::abs(midX - ax) + std::abs(by - ay) + std::abs(bx - midX);
            if (path > 0) {
                int pos = static_cast<int>(phase * static_cast<float>(path)) % (path + 1);
                int px = ax, py = ay;
                int rem = pos;
                int d1 = std::abs(midX - ax);
                if (rem <= d1) {
                    px = ax + (midX >= ax ? rem : -rem);
                } else {
                    rem -= d1;
                    int d2 = std::abs(by - ay);
                    if (rem <= d2) {
                        px = midX;
                        py = ay + (by >= ay ? rem : -rem);
                    } else {
                        rem -= d2;
                        px = midX + (bx >= midX ? rem : -rem);
                        py = by;
                    }
                }
                if (px >= 0 && px < vw && py >= 0 && py < vh)
                    s.text({vx + px, vy + py}, "▶", theme::bright());
            }
        }
    } else {
        // vertical elbow
        int midY = (ay + by) / 2;
        drawVLine(s, ax, ay, midY, vx, vy, vw, vh, st);
        drawHLine(s, ax, bx, midY, vx, vy, vw, vh, st);
        drawVLine(s, bx, midY, by, vx, vy, vw, vh, st);
        if (flow) {
            int path = std::abs(midY - ay) + std::abs(bx - ax) + std::abs(by - midY);
            if (path > 0) {
                int pos = static_cast<int>(phase * static_cast<float>(path)) % (path + 1);
                int px = ax, py = ay;
                int rem = pos;
                int d1 = std::abs(midY - ay);
                if (rem <= d1) {
                    py = ay + (midY >= ay ? rem : -rem);
                } else {
                    rem -= d1;
                    int d2 = std::abs(bx - ax);
                    if (rem <= d2) {
                        py = midY;
                        px = ax + (bx >= ax ? rem : -rem);
                    } else {
                        rem -= d2;
                        px = bx;
                        py = midY + (by >= midY ? rem : -rem);
                    }
                }
                if (px >= 0 && px < vw && py >= 0 && py < vh)
                    s.text({vx + px, vy + py}, "●", theme::cyan());
            }
        }
    }
}

inline void drawNode(inkcell::Surface& s, inkcell::Rect view, const CanvasCamera& cam,
                     const CanvasNode& n, bool selected, bool current, float tSec) {
    int sx, sy;
    worldToScreen(cam, n.wx, n.wy, sx, sy);
    const int vx = view.x, vy = view.y, vw = view.w, vh = view.h;

    // Scaled box size (zoom-aware), clamped to at least 2x2.
    const int sw = std::max(2, (int)std::lround((float)n.ww * cam.zoom));
    const int sh = std::max(2, (int)std::lround((float)n.wh * cam.zoom));

    // Clip reject
    if (sx + sw < 0 || sy + sh < 0 || sx >= vw || sy >= vh) return;

    const float phase = tSec > 0.f ? std::fmod(tSec * 0.625f, 1.f) : 0.f;
    const bool pulseOn = phase < 0.55f;

    auto border = theme::dim();
    if (selected) {
        border = theme::cyan();
        border.bold = true;
    } else if (current || n.status == model::StepStatus::Running) {
        border = pulseOn ? theme::cyan() : theme::amber();
        border.bold = true;
    } else if (n.status == model::StepStatus::Ok) {
        border = theme::green_soft();
    } else if (n.status == model::StepStatus::Fail) {
        border = theme::red();
    } else if (n.status == model::StepStatus::Skip) {
        border = theme::italic_dim();
    }

    auto bg = selected ? theme::panel_3() : theme::panel_2();

    auto put = [&](int lx, int ly, const std::string& ch, inkcell::Style st) {
        int x = sx + lx;
        int y = sy + ly;
        if (x < 0 || y < 0 || x >= vw || y >= vh) return;
        s.text({vx + x, vy + y}, ch, st.with_bg(bg.bg));
    };

    // Box — draws at scaled size; label LOD drops when the box is too small.
    for (int row = 0; row < sh; ++row) {
        for (int col = 0; col < sw; ++col) {
            const bool edge = row == 0 || row == sh - 1 || col == 0 || col == sw - 1;
            if (!edge) {
                put(col, row, " ", bg);
                continue;
            }
            const char* ch = " ";
            if (row == 0 && col == 0)
                ch = "╭";
            else if (row == 0 && col == sw - 1)
                ch = "╮";
            else if (row == sh - 1 && col == 0)
                ch = "╰";
            else if (row == sh - 1 && col == sw - 1)
                ch = "╯";
            else if (row == 0 || row == sh - 1)
                ch = "─";
            else
                ch = "│";
            put(col, row, ch, border);
        }
    }

    // Status glyph + label (LOD — drop text as the node shrinks under zoom).
    const bool wide = sw >= 12;
    const bool tall = sh >= 4;
    std::string glyph = model::stepStatusGlyph(n.status);
    auto gst = stepStatusStyle(n.status, selected, pulseOn);
    if (sw >= 3 && (sw >= 6 || !wide)) put(2, std::min(1, sh - 1), glyph, gst);

    if (wide) {
        std::string title = n.label;
        put(4, 1, inkcell::text::truncate(title, std::max(4, sw - 6)),
            (selected ? theme::bright() : theme::text()));
    }
    if (wide && tall) {
        std::string meta = n.type;
        if (!n.ref.empty() && n.ref != n.type) meta += " · " + n.ref;
        put(2, 2, inkcell::text::truncate(meta, std::max(4, sw - 4)), theme::italic_dim());

        std::string foot;
        if (n.ms > 0.0) foot = formatStepMs(n.ms);
        if (n.human) foot += foot.empty() ? "HITL" : " · HITL";
        if (n.checkpoint) foot += foot.empty() ? "ckpt" : " · ckpt";
        if (foot.empty()) foot = model::stepStatusLabel(n.status);
        put(2, 3, inkcell::text::truncate(foot, std::max(4, sw - 4)), gst);
    }
}

struct CanvasDrawOpts {
    int selected = 0;
    std::string currentId;
    float tSec = 0.f;
    bool showChrome = true;
    std::string title;
    std::string statusLine;
};

// Draw infinite canvas into view. Field/theme already behind — we only paint graph.
inline void drawWorkflowCanvas(inkcell::Surface& s, inkcell::Rect view, const CanvasGraph& graph,
                               const CanvasCamera& cam, const CanvasDrawOpts& opt) {
    if (view.w < 8 || view.h < 4) return;

    // Soft vignette corners only — leave field visible in empty space
    // (no full fill: infinite void = theme/field breathing)

    // Edges under nodes
    std::unordered_map<std::string, model::StepStatus> status;
    for (const auto& n : graph.nodes) status[n.id] = n.status;

    for (const auto& e : graph.edges) {
        const CanvasNode* a = graph.find(e.from);
        const CanvasNode* b = graph.find(e.to);
        if (!a || !b) continue;
        bool flow = false;
        auto sa = status[e.from];
        auto sb = status[e.to];
        if (sa == model::StepStatus::Running || sb == model::StepStatus::Running)
            flow = true;
        if (sa == model::StepStatus::Ok && sb == model::StepStatus::Running) flow = true;
        if (e.from == opt.currentId || e.to == opt.currentId) flow = true;
        drawEdge(s, view, cam, *a, *b, e.kind, flow, opt.tSec);
    }

    // Nodes
    for (int i = 0; i < static_cast<int>(graph.nodes.size()); ++i) {
        const auto& n = graph.nodes[static_cast<size_t>(i)];
        bool sel = (i == opt.selected);
        bool cur = (!opt.currentId.empty() && n.id == opt.currentId);
        drawNode(s, view, cam, n, sel, cur, opt.tSec);
    }

    if (!opt.showChrome) return;

    // HUD chip — top-left of viewport (camera + counts)
    {
        char camBuf[64];
        std::snprintf(camBuf, sizeof(camBuf), "cam %+.0f,%+.0f", cam.x, cam.y);
        std::string hud = camBuf;
        hud += "  ·  ";
        hud += std::to_string(graph.nodes.size()) + " nodes";
        hud += "  ·  ";
        hud += std::to_string(graph.edges.size()) + " edges";
        auto st = theme::italic_dim();
        // translucent-ish: just text, no bar fill — void stays
        s.text({view.x + 1, view.y}, inkcell::text::truncate(hud, view.w - 2), st);
    }
    if (!opt.title.empty()) {
        s.text({view.x + 1, view.y + 1},
               inkcell::text::truncate(opt.title, view.w - 2), theme::bright());
    }
    if (!opt.statusLine.empty() && view.h > 2) {
        s.text({view.x + 1, view.bottom() - 1},
               inkcell::text::truncate(opt.statusLine, view.w - 2), theme::italic_accent());
    }
}

// Fit the WHOLE graph into the viewport: adjust zoom so every node fits,
// then point the camera at the graph centre. Because it dollies (changes
// zoom), the nodes you were looking at stay in view — nothing disappears
// during the animation. Cap at 1.0 so we never zoom waaay out of a tiny graph.
// Fit the WHOLE graph into the viewport: adjust zoom so every node fits, then
// point the camera at the graph centre (= overlay of the workflow). Because it
// dollies (changes zoom + pans), the nodes you were looking at slide toward
// the centre and stay in view while zooming out — no blank frames mid-animation
// when starting on real content. '.' maps here.
inline void cameraFitGraph(CanvasCamera& cam, const CanvasGraph& g, int viewW, int viewH) {
    if (g.nodes.empty()) {
        cam.x = 0;
        cam.y = 0;
        return;
    }
    int minX = g.nodes[0].wx, minY = g.nodes[0].wy;
    int maxX = minX + g.nodes[0].ww, maxY = minY + g.nodes[0].wh;
    for (const auto& n : g.nodes) {
        minX = std::min(minX, n.wx);
        minY = std::min(minY, n.wy);
        maxX = std::max(maxX, n.wx + n.ww);
        maxY = std::max(maxY, n.wy + n.wh);
    }
    const int gw = std::max(1, maxX - minX), gh = std::max(1, maxY - minY);
    const float pad = 1.6f;
    float fitZoom = std::min((float)((viewW - 6) / pad) / (float)gw,
                             (float)((viewH - 6) / pad) / (float)gh);
    fitZoom = std::max(kCanvasMinZoom, std::min(1.f, fitZoom));
    cam.zoom = fitZoom;
    float cx = static_cast<float>(minX + maxX) * 0.5f;
    float cy = static_cast<float>(minY + maxY) * 0.5f;
    cam.centerOn(cx, cy, viewW, viewH);
}

// Fit camera so graph content is framed with padding (zoom unchanged).
inline void cameraFrameGraph(CanvasCamera& cam, const CanvasGraph& g, int viewW, int viewH) {
    if (g.nodes.empty()) {
        cam.x = 0;
        cam.y = 0;
        return;
    }
    int minX = g.nodes[0].wx, minY = g.nodes[0].wy;
    int maxX = minX + g.nodes[0].ww, maxY = minY + g.nodes[0].wh;
    for (const auto& n : g.nodes) {
        minX = std::min(minX, n.wx);
        minY = std::min(minY, n.wy);
        maxX = std::max(maxX, n.wx + n.ww);
        maxY = std::max(maxY, n.wy + n.wh);
    }
    float cx = static_cast<float>(minX + maxX) * 0.5f;
    float cy = static_cast<float>(minY + maxY) * 0.5f;
    cam.centerOn(cx, cy, viewW, viewH);
}

inline void cameraCenterNode(CanvasCamera& cam, const CanvasNode& n, int viewW, int viewH) {
    float cx = static_cast<float>(n.wx) + static_cast<float>(n.ww) * 0.5f;
    float cy = static_cast<float>(n.wy) + static_cast<float>(n.wh) * 0.5f;
    cam.centerOn(cx, cy, viewW, viewH);
}

// Hit-test screen cell → node index or -1.
inline int hitTestNode(const CanvasGraph& g, const CanvasCamera& cam, int screenX, int screenY) {
    int wx, wy;
    screenToWorld(cam, screenX, screenY, wx, wy);
    // reverse order — topmost last drawn
    for (int i = static_cast<int>(g.nodes.size()) - 1; i >= 0; --i) {
        const auto& n = g.nodes[static_cast<size_t>(i)];
        if (wx >= n.wx && wx < n.wx + n.ww && wy >= n.wy && wy < n.wy + n.wh) return i;
    }
    return -1;
}

}  // namespace cortex::mk3::ui::components
