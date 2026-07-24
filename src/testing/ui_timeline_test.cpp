// Live ↔ resume parity: serializeTimeline / deserializeTimeline must
// round-trip kinds, titles, bodies, drill metadata.

#include <iostream>
#include <string>
#include <vector>

#include "src/ui/model/inkcell_app_model.hpp"

using namespace cortex::mk3::ui;

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { std::cerr << "FAIL " << m << "\n"; ++g_fail; } \
  else std::cout << "PASS " << m << "\n"; } while (0)

int main() {
    std::vector<TimelineRow> live;
    live.push_back({TimelineKind::User, "you", "what are the triggers?", true, "", "", "", false});
    live.push_back({TimelineKind::Thought, "thought", "scanning workflow runtime…", true, "", "", "", false});
    live.push_back({TimelineKind::Action, "agent:discovery #d1", "find triggers", true, "agent", "discovery", "d1", true});
    live.push_back({TimelineKind::Result, "#d1 discovery", "found 3 triggers", true, "agent", "discovery", "d1", true});
    live.push_back({TimelineKind::Response, "response", "we have on_start, on_step, on_done", true, "", "", "", false});
    // ephemeral — must be dropped
    live.push_back({TimelineKind::Stream, "stream", "12 bytes", true, "", "", "", false});
    live.push_back({TimelineKind::Status, "status", "agent running", true, "", "", "", false});

    std::string json = serializeTimeline(live);
    CHECK(!json.empty() && json != "[]", "serialize non-empty");
    // Writer may insert spaces around ':' — reject any stream/status kind entry.
    CHECK(json.find("\"stream\"") == std::string::npos, "stream rows not persisted");
    CHECK(json.find("\"status\"") == std::string::npos, "status rows not persisted");

    auto back = deserializeTimeline(json);
    CHECK(back.size() == 5, "5 persistable rows restored (got " + std::to_string(back.size()) + ")");
    CHECK(back[0].kind == TimelineKind::User && back[0].body.find("triggers") != std::string::npos,
          "user row body");
    CHECK(back[1].kind == TimelineKind::Thought, "thought kind");
    CHECK(back[2].kind == TimelineKind::Action && back[2].actionName == "discovery" && back[2].drillable,
          "action drill metadata");
    CHECK(back[3].kind == TimelineKind::Result && back[3].actionId == "d1", "result id");
    CHECK(back[4].kind == TimelineKind::Response && back[4].body.find("on_start") != std::string::npos,
          "response body");

    // Round-trip twice (idempotent).
    auto again = deserializeTimeline(serializeTimeline(back));
    CHECK(again.size() == back.size(), "second round-trip size stable");

    std::cout << (g_fail ? "FAIL\n" : "ok\n");
    return g_fail ? 1 : 0;
}
