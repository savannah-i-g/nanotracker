// ntp-convert — web plugin manifest → NTP v1 converter.
//
//   ntp-convert <web-plugin.json> [-o <ntp-plugin.json>]
//
// Concepts map 1:1 where the web feature survived the port; everything
// that did not (worklet nodes, webview UIs, splitter/merger channel
// routing, capability-gated features) is reported explicitly on stderr
// so the author knows exactly what needs hand-porting. The web format's
// v1-v5 migration debt dies here: legacy oscillator/envelope/filter
// shorthand and v2 single-target mod routes normalise into the single
// NTP graph schema.
#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

using nlohmann::json;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) — CLI-lifetime report sink
std::vector<std::string> g_report;

void report(const std::string& message) {
    g_report.push_back(message);
}

json convert_mod_routes(const json& routes) {
    json out = json::array();
    for (const json& route : routes) {
        if (!route.is_object()) {
            continue;
        }
        json converted = json::object();
        converted["source"] = route.value("source", "");
        json targets = json::array();
        if (route.contains("targets") && route["targets"].is_array()) {
            for (const json& target : route["targets"]) {
                json t = json::object();
                t["target"] = target.value("target", "");
                t["depth"] = target.value("depth", 1.0);
                if (target.contains("offset")) {
                    t["offset"] = target["offset"];
                }
                if (target.contains("scale")) {
                    t["scale"] = target["scale"];
                }
                if (target.contains("slew")) {
                    t["slew"] = target["slew"];
                }
                if (target.contains("transform")) {
                    t["transform"] = target["transform"];
                }
                targets.push_back(std::move(t));
            }
        } else if (route.contains("target")) {
            // v2 single-target shorthand.
            targets.push_back(
                json{{"target", route.value("target", "")}, {"depth", route.value("depth", 1.0)}});
        }
        converted["targets"] = std::move(targets);
        out.push_back(std::move(converted));
    }
    return out;
}

// Carries a graph node, translating what changed and reporting what
// cannot exist in NTP v1. Returns a null json for dropped nodes.
json convert_node(const json& node, bool voice_default) {
    const std::string id = node.value("id", "?");
    const std::string type = node.value("type", "");
    if (type == "worklet") {
        report("node \"" + id +
               "\": worklet DSP has no NTP equivalent — rebuild from "
               "built-in nodes (granular/wavetable/sampler cover the shipped worklets)");
        return {};
    }
    if (type == "splitter" || type == "merger") {
        report("node \"" + id + "\": " + type +
               " channel routing is not in the NTP v1 node "
               "set — restructure with panner/mixer nodes");
        return {};
    }
    json out = node; // carry unknown-but-harmless fields as-is
    if (type == "analyser") {
        // UI-only pass-through on the web; meters read node peaks
        // natively, so a unity gain preserves the graph shape.
        out["type"] = "gain";
        out["gain"] = 1.0;
        report("node \"" + id +
               "\": analyser became a unity gain (meters bind to node "
               "outputs directly in NTP)");
    }
    if (!node.contains("scope")) {
        out["scope"] = voice_default ? "voice" : "shared";
    }
    return out;
}

// Legacy (pre-v3) instrument shorthand → graph nodes. The web scheduled
// these through BuiltinVoice; NTP only has the graph, so the classic
// osc → filter → amp(envelope) chain is synthesised.
json legacy_instrument_graph(const json& dsp) {
    json nodes = json::array();
    json connections = json::array();
    json mod_routes = json::array();

    int osc_index = 0;
    for (const json& osc : dsp.value("oscillators", json::array())) {
        const std::string id = "osc" + std::to_string(++osc_index);
        const std::string osc_type = osc.value("type", "sine");
        json node = json::object();
        node["id"] = id;
        node["type"] = osc_type == "noise" ? "noise" : "oscillator";
        node["scope"] = "voice";
        if (osc_type != "noise") {
            node["oscType"] = osc_type;
            node["frequency"] = 0.0;
            node["detune"] = osc.value("detune", 0.0);
            mod_routes.push_back(json{
                {"source", "pitch"},
                {"targets", json::array({json{{"target", id + ".frequency"}, {"depth", 1.0}}})}});
        }
        node["gain"] = osc.value("mix", 1.0);
        if (osc.contains("fmTarget")) {
            report("oscillator \"" + id +
                   "\": fmTarget shorthand — re-express as a "
                   "toParam connection onto the carrier's frequency");
        }
        nodes.push_back(std::move(node));
        connections.push_back(json{{"from", id}, {"to", "mix"}});
    }
    nodes.push_back(json{{"id", "mix"}, {"type", "mixer"}, {"scope", "voice"}, {"gain", 1.0}});

    std::string tail = "mix";
    if (dsp.contains("filter") && dsp["filter"].is_object()) {
        const json& filter = dsp["filter"];
        nodes.push_back(json{{"id", "filter"},
                             {"type", "biquad"},
                             {"scope", "voice"},
                             {"filterType", filter.value("type", "lowpass")},
                             {"frequency", filter.value("frequency", 1000.0)},
                             {"q", filter.value("Q", 0.707)}});
        connections.push_back(json{{"from", tail}, {"to", "filter"}});
        tail = "filter";
    }

    const json envelope = dsp.value("envelope", json::object());
    nodes.push_back(json{
        {"id", "env"},
        {"type", "envelope"},
        {"scope", "voice"},
        {"release", envelope.value("release", 0.25)},
        {"envStages", json::array({json{{"target", 1.0}, {"time", envelope.value("attack", 0.005)}},
                                   json{{"target", envelope.value("sustain", 0.7)},
                                        {"time", envelope.value("decay", 0.1)}}})}});
    nodes.push_back(json{{"id", "amp"}, {"type", "gain"}, {"scope", "voice"}, {"gain", 0.0}});
    connections.push_back(json{{"from", tail}, {"to", "amp"}});
    connections.push_back(json{{"from", "env"}, {"to", "amp"}, {"toParam", "gain"}});
    connections.push_back(json{{"from", "amp"}, {"to", "voiceOut"}});

    if (dsp.contains("samples") && !dsp["samples"].empty()) {
        json sampler = json::object();
        sampler["id"] = "sampler";
        sampler["type"] = "sampler";
        sampler["zones"] = dsp["samples"];
        nodes.push_back(std::move(sampler));
        connections.push_back(json{{"from", "sampler"}, {"to", "output"}});
    }
    for (const json& route : convert_mod_routes(dsp.value("modRoutes", json::array()))) {
        mod_routes.push_back(route);
    }
    for (const char* feature : {"unison", "portamento", "lfos", "envelopes", "filters"}) {
        if (dsp.contains(feature)) {
            report(std::string("legacy \"") + feature +
                   "\" shorthand needs hand-porting into graph nodes");
        }
    }
    return json{
        {"nodes", std::move(nodes)},
        {"connections", std::move(connections)},
        {"modRoutes", std::move(mod_routes)},
    };
}

// Control trees are recursive by design (the loader bounds depth to 4).
// NOLINTNEXTLINE(misc-no-recursion)
json convert_ui(const json& ui) {
    json controls = json::array();
    for (const json& control : ui.value("controls", json::array())) {
        const std::string type = control.value("type", "");
        if (type == "webview") {
            report("webview UI control dropped — the host shows the auto-param panel");
            continue;
        }
        if (type == "waveform_view") {
            report("waveform_view control dropped (webview-era chrome)");
            continue;
        }
        json converted = control;
        if (type == "group" && control.contains("children")) {
            converted = control;
            converted["children"] = convert_ui(json{{"controls", control["children"]}})[0];
        }
        controls.push_back(std::move(converted));
    }
    // Wrapped so group recursion above can reuse this function.
    return json::array({std::move(controls)});
}

int run(int argc, char** argv) {
    const char* input_path = nullptr;
    const char* output_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-o" && i + 1 < argc) {
            output_path = argv[++i];
        } else {
            input_path = argv[i];
        }
    }
    if (input_path == nullptr) {
        std::fprintf(stderr, "usage: ntp-convert <web-plugin.json> [-o <ntp-plugin.json>]\n");
        return 2;
    }
    std::ifstream input(input_path);
    if (!input) {
        std::fprintf(stderr, "cannot open %s\n", input_path);
        return 1;
    }
    const json web = json::parse(input, nullptr, false);
    if (web.is_discarded() || !web.is_object()) {
        std::fprintf(stderr, "%s is not valid JSON\n", input_path);
        return 1;
    }

    json out = json::object();
    out["ntp"] = 1;
    out["id"] = web.value("id", "");
    out["name"] = web.value("name", "");
    out["version"] = web.value("version", "1.0.0");
    if (web.contains("author")) {
        out["author"] = web["author"];
    }
    if (web.contains("description")) {
        out["description"] = web["description"];
    }
    std::string type = web.value("type", "fx");
    if (type == "control-source") {
        report("type control-source is not in NTP v1 — emitting as fx");
        type = "fx";
    }
    out["type"] = type;
    out["params"] = web.value("params", json::array());
    if (web.contains("presets")) {
        out["presets"] = web["presets"];
    }
    if (web.contains("requires") && !web["requires"].empty()) {
        out["requires"] = web["requires"];
        // "userSamples" (user-assignable slots + POVR) is native; only
        // capabilities beyond it will be refused at load.
        for (const json& capability : web["requires"]) {
            if (capability.is_string() && capability.get<std::string>() != "userSamples") {
                report("requires[] carries \"" + capability.get<std::string>() +
                       "\" — NTP v1 hosts refuse unknown capabilities, expect a load refusal "
                       "until the feature lands natively");
            }
        }
    }

    const json dsp = web.value("dsp", json::object());
    out["voices"] = dsp.value("voices", 8);
    out["voiceStealing"] = dsp.value("voiceStealing", "oldest");
    if (dsp.contains("releaseTail")) {
        out["releaseTail"] = dsp["releaseTail"];
    }

    const bool is_instrument = type == "instrument";
    if (dsp.contains("graph") && dsp["graph"].is_object()) {
        // v3 graph form: node-by-node translation.
        const json& graph = dsp["graph"];
        json nodes = json::array();
        for (const json& node : graph.value("nodes", json::array())) {
            json converted = convert_node(node, is_instrument);
            if (!converted.is_null()) {
                nodes.push_back(std::move(converted));
            }
        }
        json connections = json::array();
        for (const json& connection : graph.value("connections", json::array())) {
            if (connection.value("outputIndex", 0) != 0 || connection.value("inputIndex", 0) != 0) {
                report("connection with channel indices dropped (splitter/merger routing)");
                continue;
            }
            json converted = json::object();
            converted["from"] = connection.value("from", "");
            converted["to"] = connection.value("to", "");
            if (connection.contains("toParam")) {
                converted["toParam"] = connection["toParam"];
            }
            connections.push_back(std::move(converted));
        }
        out["graph"] =
            json{{"nodes", std::move(nodes)},
                 {"connections", std::move(connections)},
                 {"modRoutes", convert_mod_routes(graph.value("modRoutes", json::array()))}};
    } else if (is_instrument) {
        out["graph"] = legacy_instrument_graph(dsp);
        report("legacy oscillator/envelope/filter shorthand synthesised into a graph — "
               "review the generated chain");
    } else {
        // FX: the web's flat nodes/connections was already graph-shaped.
        json nodes = json::array();
        for (const json& node : dsp.value("nodes", json::array())) {
            json converted = convert_node(node, false);
            if (!converted.is_null()) {
                nodes.push_back(std::move(converted));
            }
        }
        out["graph"] =
            json{{"nodes", std::move(nodes)},
                 {"connections", dsp.value("connections", json::array())},
                 {"modRoutes", convert_mod_routes(dsp.value("modRoutes", json::array()))}};
    }

    if (web.contains("ports")) {
        out["ports"] = web["ports"];
    }
    if (web.contains("ui")) {
        out["ui"] = json{{"controls", convert_ui(web["ui"])[0]}};
    }

    const std::string serialised = out.dump(2);
    if (output_path != nullptr) {
        std::ofstream output(output_path);
        output << serialised << '\n';
        if (!output.good()) {
            std::fprintf(stderr, "write failed: %s\n", output_path);
            return 1;
        }
    } else {
        std::printf("%s\n", serialised.c_str());
    }
    for (const std::string& message : g_report) {
        std::fprintf(stderr, "note: %s\n", message.c_str());
    }
    std::fprintf(stderr, "%zu conversion note(s)\n", g_report.size());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // json access and serialisation can throw (e.g. invalid UTF-8 in
    // the source manifest); a converter should report, not abort.
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "conversion failed: %s\n", e.what());
        return 1;
    }
}
