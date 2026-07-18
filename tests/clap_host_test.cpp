// CLAP hosting verification against a real dlopen'd .clap binary
// (tests/fixtures/test_clap_plugin.c, built as nt_test_clap.clap):
// library open + descriptor scan, instantiation, parameter
// enumeration, note events → audible sine at the right frequency,
// UI-thread param changes reaching the processor, and state
// round-trips through the CLAP state extension.
#include "ext/clap_host.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

namespace {

constexpr std::uint32_t kRate = 48000;

float goertzel(const std::vector<float>& mono, std::uint32_t rate, float freq) {
    const std::size_t n = mono.size();
    const double k = std::round(static_cast<double>(n) * freq / rate);
    const double w = 2.0 * 3.14159265358979 * k / static_cast<double>(n);
    const double c = 2.0 * std::cos(w);
    double s0 = 0.0;
    double s1 = 0.0;
    double s2 = 0.0;
    for (const float x : mono) {
        s0 = x + (c * s1) - s2;
        s2 = s1;
        s1 = s0;
    }
    return static_cast<float>(std::sqrt(std::max(0.0, (s1 * s1) + (s2 * s2) - (c * s1 * s2))) /
                              (static_cast<double>(n) / 2.0));
}

std::vector<float> render_mono(nt::ext::ClapPlugin& plugin, int blocks) {
    std::vector<float> mono;
    std::array<float, 256> out{};
    for (int b = 0; b < blocks; ++b) {
        plugin.process_block(nullptr, out.data(), 128);
        for (int i = 0; i < 128; ++i) {
            mono.push_back(out[static_cast<std::size_t>(i) * 2]);
        }
    }
    return mono;
}

} // namespace

TEST_CASE("clap library opens and lists the test instrument", "[clap]") {
    std::string error;
    auto library = nt::ext::ClapLibrary::open(NT_TEST_CLAP, error);
    INFO(error);
    REQUIRE(library != nullptr);
    REQUIRE(library->descriptors().size() == 1);
    CHECK(library->descriptors()[0].id == "nt.test.sine");
    CHECK(library->descriptors()[0].name == "NT TEST SINE");
    CHECK(library->descriptors()[0].is_instrument);
}

TEST_CASE("clap instance plays notes, takes params and round-trips state", "[clap]") {
    std::string error;
    auto library = nt::ext::ClapLibrary::open(NT_TEST_CLAP, error);
    REQUIRE(library != nullptr);
    auto plugin = nt::ext::ClapPlugin::create(*library, "nt.test.sine", kRate, error);
    INFO(error);
    REQUIRE(plugin != nullptr);
    CHECK(plugin->name() == "NT TEST SINE");
    CHECK_FALSE(plugin->has_audio_input());
    REQUIRE(plugin->params().size() == 1);
    CHECK(plugin->params()[0].name == "gain");
    CHECK(plugin->param_value(1) == 0.5);

    // Held A-4 → dominant 440 Hz line.
    plugin->plugin_note_on(69, 1.0F);
    const std::vector<float> tone = render_mono(*plugin, 20);
    const float at_440 = goertzel(tone, kRate, 440.0F);
    CHECK(at_440 > 0.2F);
    CHECK(at_440 > goertzel(tone, kRate, 620.0F) * 10.0F);

    // Param change through the UI ring: gain 0 silences the output.
    plugin->set_param(1, 0.0);
    (void)render_mono(*plugin, 1); // block that applies the event
    const std::vector<float> silent = render_mono(*plugin, 4);
    float peak = 0.0F;
    for (const float v : silent) {
        peak = std::max(peak, std::abs(v));
    }
    CHECK(peak < 1e-6F);

    // Note off ends the voice.
    plugin->set_param(1, 0.5);
    plugin->plugin_note_off(69);
    (void)render_mono(*plugin, 1);
    const std::vector<float> after_off = render_mono(*plugin, 4);
    float off_peak = 0.0F;
    for (const float v : after_off) {
        off_peak = std::max(off_peak, std::abs(v));
    }
    CHECK(off_peak < 1e-6F);

    // State: save at gain 0.5, change to 0.9, load → back to 0.5.
    const std::vector<std::uint8_t> saved = plugin->save_state();
    REQUIRE_FALSE(saved.empty());
    plugin->set_param(1, 0.9);
    (void)render_mono(*plugin, 1);
    REQUIRE(plugin->load_state(saved));
    CHECK(plugin->param_value(1) == 0.5);
}
