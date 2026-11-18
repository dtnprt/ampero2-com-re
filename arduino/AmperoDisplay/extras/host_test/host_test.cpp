// Host test for AmperoClient: replays the device frames of the Wireshark capture and compares
// the frames the sketch sends with the ones the Hotone editor sent.
//   build (MSVC):  cl /EHsc /std:c++17 /I. host_test.cpp ..\..\AmperoClient.cpp
//   run:           host_test.exe ..\..\..\..\csharp\tests\Ampero.Protocol.Tests\TestData\capture_frames.txt
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../AmperoClient.h"

uint32_t g_millis = 0;
SerialShim Serial0;

using Bytes = std::vector<uint8_t>;
static std::vector<Bytes> g_sent;
static int g_fail = 0;

static bool capture(const uint8_t* f, size_t n) {
  g_sent.emplace_back(f, f + n);
  return true;
}

static Bytes hex(const std::string& s) {
  Bytes b;
  for (size_t i = 0; i + 1 < s.size(); i += 2) b.push_back((uint8_t)std::stoi(s.substr(i, 2), nullptr, 16));
  return b;
}

#define CHECK(c)                                              \
  do {                                                        \
    if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } \
    else std::printf("ok   %s\n", #c);                        \
  } while (0)

int main(int argc, char** argv) {
  std::ifstream in(argc > 1 ? argv[1] : "capture_frames.txt");
  std::vector<std::pair<std::string, Bytes>> frames;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ss(line);
    std::string dir, h;
    ss >> dir >> h;
    frames.push_back({dir, hex(h)});
  }
  CHECK(frames.size() > 100);
  auto hostHas = [&](const Bytes& f) {
    for (auto& p : frames) if (p.first == "OUT" && p.second == f) return true;
    return false;
  };

  AmperoClient c;
  c.begin(capture);
  g_millis = 1000;
  c.onConnected();
  CHECK(g_sent.size() == 1 && g_sent[0] == frames[0].second);   // Connect, identical to editor

  g_millis += 300;
  c.loop();                                                      // -> request current preset
  CHECK(g_sent.size() == 2 && hostHas(g_sent[1]));

  // replay every device frame in capture order; drive loop() in between
  int scenesSeen = 0;
  std::vector<std::string> names;
  uint32_t rev = c.revision();
  for (auto& p : frames) {
    if (p.first != "IN") continue;
    c.onSysEx(p.second.data(), p.second.size());
    if (c.revision() != rev) {
      rev = c.revision();
      if (c.hasPreset() && (names.empty() || names.back() != c.presetName())) names.push_back(c.presetName());
      scenesSeen++;
    }
    g_millis += 20;
    c.loop();
  }
  for (auto& n : names) std::printf("     preset seen: %s\n", n.c_str());
  CHECK(names.size() == 4);
  CHECK(names.size() == 4 && names[0] == "Le Chug" && names[3] == "Juicy Fusion");
  CHECK(c.presetIndex() == 6);
  CHECK(std::string(c.sceneName(0)) == "Scene 1");
  CHECK(c.activeScene() == 1);   // last device notification in the capture: scene 1 (later scene changes came from the host)

  // every request the client sent while replaying must be a frame the editor also sent
  int unknown = 0;
  for (size_t i = 1; i < g_sent.size(); i++) if (!hostHas(g_sent[i])) unknown++;
  std::printf("     frames sent: %zu, not in capture: %d\n", g_sent.size(), unknown);
  CHECK(unknown == 0);

  g_sent.clear();
  c.selectScene(2);
  CHECK(g_sent.size() == 1 && hostHas(g_sent[0]) && c.activeScene() == 2);

  std::printf(g_fail ? "\n%d FEHLER\n" : "\nalle Tests ok\n", g_fail);
  return g_fail ? 1 : 0;
}
