// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_controller_prompts.h"
#include "bbport_settings.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

int main(int argc, char** argv) {
    using namespace BbPrompts;
    // Caller supplies a fresh retained fixture path; the test never removes files.
    assert(argc == 2);
    const std::array<const char*, 4> nintendo{"B", "A", "Y", "X"};
    const std::array<const char*, 4> unknown{};
    assert(std::string(Label(Nintendo, 0, Identity, unknown)) == "B");
    assert(std::string(Label(Xbox, 0, Identity, unknown)) == "A");
    assert(std::string(Label(PlayStation, 0, Identity, unknown)) == "Cross");
    assert(std::string(Label(Auto, 0, Identity, nintendo)) == "B");
    assert(std::string(Label(Auto, 0, Identity, unknown)) == "Bottom");
    assert(std::string(Label(Nintendo, -1, Identity, unknown)) == "?");
    assert(ParseMap("1032") == (FaceMap{1, 0, 3, 2}));
    for (const char* bad : {"", "012", "01234", "0000", "4567", "-123", "abcd"}) {
        assert(ParseMap(bad) == Identity);
    }

    // Every permutation of Steam's four face outputs, including simultaneous
    // input/repeats, must resolve back to the physical label requested.
    FaceMap inputs = Identity;
    int permutations = 0;
    do {
        Calibration c;
        for (int physical = 0; physical < 4; ++physical) {
            assert(c.step == physical && !c.Complete());
            c.Press(inputs[physical]);
            c.Press((inputs[physical] + 1) % 4); // ignored until release
            assert(c.step == physical && !c.Complete());
            c.Release((inputs[physical] + 1) % 4); // wrong release ignored
            assert(c.step == physical);
            c.Release(inputs[physical]);
        }
        assert(c.Complete());
        for (int physical = 0; physical < 4; ++physical) {
            assert(c.map[inputs[physical]] == physical);
            assert(std::string(Label(Nintendo, inputs[physical], c.map, unknown)) == nintendo[physical]);
        }
        ++permutations;
    } while (std::next_permutation(inputs.begin(), inputs.end()));
    assert(permutations == 24);
    Calibration invalid;
    invalid.Press(8);
    assert(invalid.error && invalid.step == 0);
    invalid.Press(0);
    invalid.Release(0);
    invalid.Press(0);
    assert(invalid.error && invalid.step == 1 && !invalid.Complete());
    invalid = {}; // cancel/restart discards partial mapping
    assert(invalid.map == Identity && invalid.step == 0 && !invalid.error);

    setenv("BB_CONFIG", argv[1], 1);
    auto& settings = BbSettings::Get();
    settings.controller_layout = Nintendo;
    settings.controller_device = "fixture-controller";
    settings.controller_map = {1, 0, 3, 2};
    BbSettings::Save();
    settings.controller_layout = Auto;
    settings.controller_device.clear();
    settings.controller_map = Identity;
    BbSettings::Load();
    assert(settings.controller_layout == Nintendo);
    assert(settings.controller_device == "fixture-controller");
    assert(settings.controller_map == (FaceMap{1, 0, 3, 2}));
    std::printf("PASS: labels, fallback, all 24 face permutations, duplicate/release handling, config round-trip\n");
}
