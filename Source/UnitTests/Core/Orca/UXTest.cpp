// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <fmt/format.h>
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Image.h"
#include "Common/IOFile.h"
#include "Common/Logging/LogManager.h"
#include "Common/ScopeGuard.h"
#include <gtest/gtest.h>
#include <imgui.h>

#include "Core/Orca/Session/Events.h"
#include "Core/Core.h"
#include "Core/Orca/Bridge.h"
#include "Core/Orca/Session/Replay.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/Chat.h"
#include "Core/Orca/UX/ControllerSource.h"
#include "Core/Orca/UX/Controllers.h"
#include "Core/Orca/Games/Brawl/FreeSpace.h"
#include "Core/Orca/Games/Brawl/MatchBlock.h"
#include "Core/Orca/UX/GamePatches.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Games/Brawl/NameTags.h"
#include "Core/Orca/UX/OrbCombo.h"
#include "Core/Orca/UX/Overlay.h"
#include "Core/Orca/UX/OverlayKey.h"
#include "Core/Orca/UX/AppOrbDraw.h"
#include "Core/Rollback/Rollback.h"
#include "InputCommon/GCPadStatus.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/FrameDumper.h"

using namespace Orca::UX;

namespace
{
GCPadStatus ReferenceDecode(const std::array<u8, 9>& c)
{
  GCPadStatus pad = {};
  constexpr u16 b1_bits[8] = {PAD_BUTTON_A,    PAD_BUTTON_B,     PAD_BUTTON_X,    PAD_BUTTON_Y,
                              PAD_BUTTON_LEFT, PAD_BUTTON_RIGHT, PAD_BUTTON_DOWN, PAD_BUTTON_UP};
  constexpr u16 b2_bits[4] = {PAD_BUTTON_START, PAD_TRIGGER_Z, PAD_TRIGGER_R, PAD_TRIGGER_L};
  for (int i = 0; i < 8; ++i)
    if (c[1] & (1 << i))
      pad.button |= b1_bits[i];
  for (int i = 0; i < 4; ++i)
    if (c[2] & (1 << i))
      pad.button |= b2_bits[i];
  pad.stickX = c[3];
  pad.stickY = c[4];
  pad.substickX = c[5];
  pad.substickY = c[6];
  pad.triggerLeft = c[7];
  pad.triggerRight = c[8];
  return pad;
}

void ExpectSame(const GCPadStatus& a, const GCPadStatus& b)
{
  EXPECT_EQ(a.button, b.button);
  EXPECT_EQ(a.stickX, b.stickX);
  EXPECT_EQ(a.stickY, b.stickY);
  EXPECT_EQ(a.substickX, b.substickX);
  EXPECT_EQ(a.substickY, b.substickY);
  EXPECT_EQ(a.triggerLeft, b.triggerLeft);
  EXPECT_EQ(a.triggerRight, b.triggerRight);
  EXPECT_EQ(a.analogA, b.analogA);
  EXPECT_EQ(a.analogB, b.analogB);
  EXPECT_EQ(a.isConnected, b.isConnected);
}

std::string PortJson(int port, int seat, const char* type, int buttons,
                     std::array<int, 4> axes = {128, 128, 128, 128},
                     std::array<int, 2> triggers = {0, 0},
                     std::array<int, 6> origin = {128, 128, 128, 128, 0, 0})
{
  const bool connected = std::string(type) != "null";
  return fmt::format(
      R"({{"adapterId":"s-gc","port":{},"seat":{},"connected":{},"type":{},"buttons":{},)"
      R"("axes":[{},{},{},{}],"triggers":[{},{}],"origin":[{},{},{},{},{},{}],"calibrationRevision":0}})",
      port, seat, connected ? "true" : "false", connected ? fmt::format("\"{}\"", type) : "null",
      buttons, axes[0], axes[1], axes[2], axes[3], triggers[0], triggers[1], origin[0], origin[1],
      origin[2], origin[3], origin[4], origin[5]);
}

std::string PadJson(int index, const char* mapping, const std::string& axes,
                    const std::string& buttons)
{
  return fmt::format(
      R"({{"index":{},"id":"Xbox Wireless Controller 045e:0b13","connected":true,"mapping":"{}","axes":{},"buttons":{},"timestamp":1759370000123.25}})",
      index, mapping, axes, buttons);
}

std::string Buttons17(int pressed_index = -1, double value = 1)
{
  std::string s = "[";
  for (int i = 0; i < 17; ++i)
  {
    if (i)
      s += ',';
    s += i == pressed_index ? fmt::format(R"({{"value":{},"pressed":true}})", value) :
                              R"({"value":0,"pressed":false})";
  }
  return s + "]";
}

std::string Event(bool owned, const std::string& ports, const std::string& pads,
                  double sent = 1759370000500.125, double received = 1759370000499.5,
                  bool suspended = false, int schema = 1)
{
  return fmt::format(
      R"({{"type":"controllers","snapshot":{{"schema":{},"session":"0b6f","sequence":7,"sentAt":{:.3f},)"
      R"("receivedAt":{:.3f},"source":"native","state":"connected","message":"","owned":{},)"
      R"("suspended":{},"invalidReports":0,"ports":[{}],"pads":[{}]}}}})",
      schema, sent, received, owned ? "true" : "false", suspended ? "true" : "false", ports, pads);
}

std::string FourPorts(const std::string& p0 = PortJson(0, 0, "null", 0),
                      const std::string& p1 = PortJson(1, 1, "null", 0),
                      const std::string& p2 = PortJson(2, 2, "null", 0),
                      const std::string& p3 = PortJson(3, 3, "null", 0))
{
  return p0 + "," + p1 + "," + p2 + "," + p3;
}
}

TEST(OrcaUXControllers, AdapterBytesMatchDolphinsAdapterForEveryButtonMask)
{
  for (const bool wireless : {false, true})
  {
    for (int buttons = 0; buttons < 4096; ++buttons)
    {
      AdapterPort port;
      port.connected = true;
      port.wireless = wireless;
      port.buttons = static_cast<u16>(buttons);
      port.axes = {static_cast<u8>(buttons & 0xFF), static_cast<u8>(255 - (buttons & 0xFF)),
                   static_cast<u8>((buttons * 7) & 0xFF), static_cast<u8>((buttons * 13) & 0xFF)};
      port.triggers = {static_cast<u8>((buttons * 3) & 0xFF), static_cast<u8>((buttons >> 4) & 0xFF)};
      const std::array<u8, 9> wire{wireless ? u8(0x20) : u8(0x10),
                                   static_cast<u8>(buttons & 0xFF),
                                   static_cast<u8>(buttons >> 8),
                                   port.axes[0],
                                   port.axes[1],
                                   port.axes[2],
                                   port.axes[3],
                                   port.triggers[0],
                                   port.triggers[1]};
      ExpectSame(MapAdapterPort(port), ReferenceDecode(wire));
      if (::testing::Test::HasFailure())
        return;
    }
  }
}

TEST(OrcaUXControllers, EveryAxisByteIsPassedThroughUncalibrated)
{
  for (int v = 0; v < 256; ++v)
  {
    AdapterPort port;
    port.connected = true;
    port.axes = {u8(v), u8(v), u8(v), u8(v)};
    port.triggers = {u8(v), u8(v)};
    const GCPadStatus pad = MapAdapterPort(port);
    EXPECT_EQ(pad.stickX, v);
    EXPECT_EQ(pad.stickY, v);
    EXPECT_EQ(pad.substickX, v);
    EXPECT_EQ(pad.substickY, v);
    EXPECT_EQ(pad.triggerLeft, v);
    EXPECT_EQ(pad.triggerRight, v);
    EXPECT_EQ(pad.button & (PAD_GET_ORIGIN | PAD_USE_ORIGIN), 0);
  }
}

TEST(OrcaUXControllers, CalibrationOriginIsAnOffsetAndClamps)
{
  AdapterPort port;
  port.connected = true;
  port.origin = {131, 124, 126, 130, 30, 20};
  port.axes = {131, 124, 126, 130};
  port.triggers = {30, 20};
  GCPadStatus pad = MapAdapterPort(port);
  EXPECT_EQ(pad.stickX, 128);
  EXPECT_EQ(pad.stickY, 128);
  EXPECT_EQ(pad.substickX, 128);
  EXPECT_EQ(pad.substickY, 128);
  EXPECT_EQ(pad.triggerLeft, 0);
  EXPECT_EQ(pad.triggerRight, 0);

  port.axes = {255, 0, 200, 60};
  port.triggers = {10, 255};
  pad = MapAdapterPort(port);
  EXPECT_EQ(pad.stickX, 252);
  EXPECT_EQ(pad.stickY, 4);
  EXPECT_EQ(pad.substickX, 202);
  EXPECT_EQ(pad.substickY, 58);
  EXPECT_EQ(pad.triggerLeft, 0);
  EXPECT_EQ(pad.triggerRight, 235);

  port.origin = {100, 160, 128, 128, 0, 0};
  port.axes = {250, 10, 128, 128};
  pad = MapAdapterPort(port);
  EXPECT_EQ(pad.stickX, 255);
  EXPECT_EQ(pad.stickY, 0);
}

TEST(OrcaUXControllers, StandardPadOnAGameCubeLayout)
{
  StandardPad pad;
  EXPECT_EQ(MapStandardPad(pad).button, 0);
  EXPECT_EQ(MapStandardPad(pad).stickX, 128);
  EXPECT_EQ(MapStandardPad(pad).stickY, 128);

  const auto only = [](int index) {
    StandardPad p;
    p.buttons[index] = 1;
    p.pressed[index] = true;
    return MapStandardPad(p).button;
  };
  EXPECT_EQ(only(0), PAD_BUTTON_A);
  EXPECT_EQ(only(2), PAD_BUTTON_B);
  EXPECT_EQ(only(1), PAD_BUTTON_X);
  EXPECT_EQ(only(3), PAD_BUTTON_Y);
  EXPECT_EQ(only(4), PAD_TRIGGER_Z);
  EXPECT_EQ(only(5), PAD_TRIGGER_Z);
  EXPECT_EQ(only(9), PAD_BUTTON_START);
  EXPECT_EQ(only(12), PAD_BUTTON_UP);
  EXPECT_EQ(only(13), PAD_BUTTON_DOWN);
  EXPECT_EQ(only(14), PAD_BUTTON_LEFT);
  EXPECT_EQ(only(15), PAD_BUTTON_RIGHT);
  EXPECT_EQ(only(8), 0);
  EXPECT_EQ(only(16), 0);

  StandardPad p;
  p.buttons[6] = 0.45f;
  p.buttons[7] = 0.95f;
  p.pressed[7] = true;
  GCPadStatus s = MapStandardPad(p);
  EXPECT_EQ(s.triggerLeft, 115);
  EXPECT_EQ(s.triggerRight, 242);
  EXPECT_EQ(s.button, PAD_TRIGGER_R);
  p.buttons[6] = 0.6f;
  s = MapStandardPad(p);
  EXPECT_EQ(s.triggerLeft, 153);
  EXPECT_EQ(s.button, PAD_TRIGGER_L | PAD_TRIGGER_R);
  p = {};
  p.buttons[4] = 1;
  p.pressed[4] = true;
  EXPECT_EQ(MapStandardPad(p).button, PAD_TRIGGER_Z);
  EXPECT_EQ(MapStandardPad(p).triggerLeft, 0);

  p = {};
  p.axes = {1, 1, -1, -1};
  s = MapStandardPad(p);
  EXPECT_EQ(s.stickX, 255);
  EXPECT_EQ(s.stickY, 1);
  EXPECT_EQ(s.substickX, 1);
  EXPECT_EQ(s.substickY, 255);
  p.axes = {0.5f, -0.25f, 0, 0};
  s = MapStandardPad(p);
  EXPECT_EQ(s.stickX, 192);
  EXPECT_EQ(s.stickY, 160);
}

TEST(OrcaUXControllers, StandardPadCStickRestingOffCentreReadsAsNeutral)
{
  StandardPad p;
  p.axes = {0, 0, -0.06f, 0.06f};
  GCPadStatus s = MapStandardPad(p);
  EXPECT_EQ(s.substickX, 128);
  EXPECT_EQ(s.substickY, 128);
  p.axes = {0, 0, 0.14f, -0.14f};
  s = MapStandardPad(p);
  EXPECT_EQ(s.substickX, 128);
  EXPECT_EQ(s.substickY, 128);
  p.axes = {0, 0, NAN, 0};
  EXPECT_EQ(MapStandardPad(p).substickX, 128);
  p.axes = {0, 0, 1, 0};
  EXPECT_EQ(MapStandardPad(p).substickX, 255);
  p.axes = {0, 0, 0, -1};
  EXPECT_EQ(MapStandardPad(p).substickY, 255);
  p.axes = {0, 0, -0.25f, 0};
  s = MapStandardPad(p);
  EXPECT_EQ(s.substickX, 96);
  EXPECT_EQ(s.substickY, 128);
  p.axes = {0, 0, 0.15f, 0.15f};
  s = MapStandardPad(p);
  EXPECT_EQ(s.substickX, 147);
  EXPECT_EQ(s.substickY, 109);
  p.axes = {0.06f, -0.06f, 0, 0};
  s = MapStandardPad(p);
  EXPECT_EQ(s.stickX, 136);
  EXPECT_EQ(s.stickY, 136);
}

TEST(OrcaUXControllers, ParsesTheHelpersSnapshot)
{
  const std::string json =
      Event(true,
            FourPorts(PortJson(0, 0, "wired", 0x101, {140, 120, 128, 128}, {33, 0}),
                      PortJson(1, 1, "wireless", 0x800), PortJson(2, 2, "null", 0),
                      PortJson(3, 3, "null", 0)),
            PadJson(3, "standard", "[0.00003051850947599719,-1,0.5,-0.000030518509475997192]",
                    Buttons17(0)) +
                "," + PadJson(1, "", "[0,0,0,0,0,0]", "[]") + "," +
                PadJson(2, "standard", "[0,0,0,0]", Buttons17()));
  const auto s = ParseSnapshotEvent(json);
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ(s->sequence, 7u);
  EXPECT_DOUBLE_EQ(s->sent_at, 1759370000500.125);
  EXPECT_TRUE(s->owned);
  ASSERT_EQ(s->ports.size(), 4u);
  EXPECT_TRUE(s->ports[0].connected);
  EXPECT_FALSE(s->ports[0].wireless);
  EXPECT_EQ(s->ports[0].buttons, 0x101);
  EXPECT_EQ(s->ports[0].axes[0], 140);
  EXPECT_EQ(s->ports[0].triggers[0], 33);
  EXPECT_TRUE(s->ports[1].wireless);
  EXPECT_FALSE(s->ports[2].connected);
  ASSERT_EQ(s->pads.size(), 2u);
  EXPECT_EQ(s->pads[0].index, 2);
  EXPECT_EQ(s->pads[1].index, 3);
  EXPECT_FLOAT_EQ(s->pads[1].axes[1], -1);
  EXPECT_TRUE(s->pads[1].pressed[0]);

  const auto none = ParseSnapshotEvent(Event(false, "", ""));
  ASSERT_TRUE(none.has_value());
  EXPECT_TRUE(none->ports.empty());
  EXPECT_TRUE(none->pads.empty());
}

TEST(OrcaUXControllers, RefusesMalformedSnapshots)
{
  const std::string good = FourPorts();
  EXPECT_TRUE(ParseSnapshotEvent(Event(true, good, "")).has_value());
  EXPECT_FALSE(ParseSnapshotEvent(Event(true, good, "", 1, 1, false, 2)).has_value());
  EXPECT_FALSE(ParseSnapshotEvent(Event(false, good, "")).has_value());
  EXPECT_FALSE(ParseSnapshotEvent(Event(true, "", "")).has_value());
  EXPECT_FALSE(ParseSnapshotEvent(
                   Event(true, FourPorts(PortJson(1, 0, "wired", 0)), ""))
                   .has_value());
  EXPECT_FALSE(
      ParseSnapshotEvent(Event(true, FourPorts(PortJson(0, 0, "wired", 4096)), "")).has_value());
  EXPECT_FALSE(
      ParseSnapshotEvent(Event(true, FourPorts(PortJson(0, 0, "usb", 0)), "")).has_value());
  EXPECT_FALSE(ParseSnapshotEvent(Event(true, FourPorts(PortJson(0, 0, "wired", 0, {256, 0, 0, 0})),
                                        ""))
                   .has_value());
  EXPECT_FALSE(ParseSnapshotEvent(
                   Event(true, FourPorts(PortJson(0, 0, "wired", 0, {1, 2, 3, 4}, {0, 0},
                                                  {128, 128, 128, 128, 0, -1})),
                         ""))
                   .has_value());
  EXPECT_FALSE(ParseSnapshotEvent(Event(false, "", PadJson(0, "standard", "[0,0,0]", Buttons17())))
                   .has_value());
  EXPECT_FALSE(
      ParseSnapshotEvent(Event(false, "", PadJson(0, "standard", "[2,0,0,0]", Buttons17())))
          .has_value());
  EXPECT_FALSE(ParseSnapshotEvent(std::string_view{R"({"type":"other","snapshot":{}})"}).has_value());
  EXPECT_FALSE(ParseSnapshotEvent(std::string_view{"not json"}).has_value());
  EXPECT_FALSE(ParseSnapshotEvent(std::string(100, '[') + std::string(100, ']')).has_value());
  EXPECT_FALSE(ParseSnapshotEvent(std::string_view{R"({"type":"controllers","n":1e999})"}).has_value());
  EXPECT_FALSE(
      ParseSnapshotEvent(R"({"type":"controllers","n":)" + std::string(400, '9') + "}").has_value());
}

TEST(OrcaUXControllers, ChoosesTheClaimedAdapterPortThenTheFirstGamepad)
{
  const std::string pad = PadJson(0, "standard", "[0,0,0,0]", Buttons17(0));
  auto s = ParseSnapshotEvent(Event(true,
                                    FourPorts(PortJson(0, 2, "wired", 0x02), PortJson(1, 1, "null", 0),
                                              PortJson(2, 0, "wired", 0x01), PortJson(3, 3, "null", 0)),
                                    pad));
  ASSERT_TRUE(s);
  auto c = ChooseLocalPad(*s, 0);
  ASSERT_TRUE(c);
  EXPECT_EQ(c->source, PadSource::Adapter);
  EXPECT_EQ(c->index, 2);
  EXPECT_EQ(c->pad.button, PAD_BUTTON_A);
  EXPECT_FALSE(c->neutral);

  s = ParseSnapshotEvent(Event(true,
                               FourPorts(PortJson(0, 0, "null", 0), PortJson(1, 3, "wired", 0x04),
                                         PortJson(2, 1, "wired", 0x08), PortJson(3, 2, "null", 0)),
                               pad));
  c = ChooseLocalPad(*s, 0);
  ASSERT_TRUE(c);
  EXPECT_EQ(c->index, 2);
  EXPECT_EQ(c->pad.button, PAD_BUTTON_Y);

  s = ParseSnapshotEvent(Event(true, FourPorts(), pad));
  c = ChooseLocalPad(*s, 0);
  ASSERT_TRUE(c);
  EXPECT_EQ(c->source, PadSource::Gamepad);
  EXPECT_EQ(c->pad.button, PAD_BUTTON_A);

  s = ParseSnapshotEvent(Event(false, "", ""));
  EXPECT_FALSE(ChooseLocalPad(*s, 0).has_value());
}

TEST(OrcaUXControllers, StaleOrSuspendedInputIsNeutral)
{
  const std::string pad = PadJson(0, "standard", "[1,0,0,0]", Buttons17(0));
  const std::string ports = FourPorts(PortJson(0, 0, "wired", 0x01, {200, 128, 128, 128}));
  auto s = ParseSnapshotEvent(Event(true, ports, pad));
  EXPECT_FALSE(ChooseLocalPad(*s, 100)->neutral);
  EXPECT_EQ(ChooseLocalPad(*s, 100)->pad.stickX, 200);
  auto c = ChooseLocalPad(*s, 251);
  EXPECT_TRUE(c->neutral);
  EXPECT_EQ(c->source, PadSource::Adapter);
  EXPECT_EQ(c->pad.button, 0);
  EXPECT_EQ(c->pad.stickX, 128);
  EXPECT_TRUE(c->pad.isConnected);
  s = ParseSnapshotEvent(Event(true, ports, pad, 1759370000500.0, 1759370000300.0));
  EXPECT_FALSE(ChooseLocalPad(*s, 40)->neutral);
  EXPECT_TRUE(ChooseLocalPad(*s, 60)->neutral);
  s = ParseSnapshotEvent(Event(true, ports, pad, 1759370000500.0, 0));
  EXPECT_TRUE(ChooseLocalPad(*s, 0)->neutral);
  s = ParseSnapshotEvent(Event(true, ports, pad, 1759370000500.0, 1759370000500.0, true));
  EXPECT_TRUE(ChooseLocalPad(*s, 0)->neutral);
  s = ParseSnapshotEvent(Event(true, FourPorts(), "", 1759370000500.0, 1759370000400.0, true));
  ASSERT_TRUE(s);
  EXPECT_FALSE(ChooseLocalPad(*s, 0).has_value());
  s = ParseSnapshotEvent(Event(false, "", pad));
  EXPECT_FALSE(ChooseLocalPad(*s, 250)->neutral);
  EXPECT_TRUE(ChooseLocalPad(*s, 250.5)->neutral);
  EXPECT_EQ(ChooseLocalPad(*s, 250.5)->pad.stickX, 128);
}

namespace
{
float HelperAxis(int s, bool negated_by_app = false)
{
  const std::string printed = fmt::format("{:.8g}", s / (s < 0 ? 32768.0 : 32767.0));
  const double v = std::stod(printed);
  return static_cast<float>(negated_by_app ? -v : v);
}
int Hid8(int byte)
{
  return 257 * byte - 32768;
}
u8 LegacyAxisByte(float v, bool invert)
{
  const float s = std::clamp(invert ? -v : v, -1.0f, 1.0f);
  return static_cast<u8>(std::clamp(128 + static_cast<int>(std::lround(s * 127)), 0, 255));
}
}

TEST(OrcaUXControllers, BoxControllerBytesReachTheGameExactlyInBothDirections)
{
  for (int g = 1; g <= 255; ++g)
  {
    SCOPED_TRACE(g);
    StandardPad p;
    p.axes = {HelperAxis(Hid8(g)), HelperAxis(Hid8(255 - g)), HelperAxis(Hid8(g)),
              HelperAxis(Hid8(255 - g))};
    GCPadStatus s = MapStandardPad(p);
    EXPECT_EQ(s.stickX, g);
    EXPECT_EQ(s.stickY, g);
    if (std::abs(g - 128) > 26)
    {
      EXPECT_EQ(s.substickX, g);
      EXPECT_EQ(s.substickY, g);
    }
    p.axes = {HelperAxis(Hid8(g)), HelperAxis(Hid8(g), true), 0, 0};
    s = MapStandardPad(p);
    EXPECT_EQ(s.stickX, g);
    EXPECT_EQ(s.stickY, g);
    const int xi = (g - 128) * 65535 / 255 + 128;
    p.axes = {HelperAxis(xi), HelperAxis(-std::max(-32767, xi)), 0, 0};
    s = MapStandardPad(p);
    EXPECT_EQ(s.stickX, g);
    EXPECT_EQ(s.stickY, g);
  }
  StandardPad p;
  p.axes = {HelperAxis(Hid8(0)), HelperAxis(Hid8(255)), 0, 0};
  EXPECT_EQ(MapStandardPad(p).stickX, 1);
  EXPECT_EQ(MapStandardPad(p).stickY, 1);
  p.axes = {HelperAxis(Hid8(128)), HelperAxis(Hid8(127)), 0, 0};
  EXPECT_EQ(MapStandardPad(p).stickX, 128);
  EXPECT_EQ(MapStandardPad(p).stickY, 128);
}

TEST(OrcaUXControllers, AnalogSticksKeepTheirMapOffTheEightBitLattice)
{
  int changed = 0;
  for (int raw = -32768; raw <= 32767; ++raw)
  {
    for (const bool negated : {false, true})
    {
      const float v = HelperAxis(raw, negated);
      for (const bool invert : {false, true})
      {
        StandardPad p;
        p.axes = {invert ? 0.0f : v, invert ? v : 0.0f, 0, 0};
        const GCPadStatus s = MapStandardPad(p);
        const int now = invert ? s.stickY : s.stickX;
        const int before = LegacyAxisByte(v, invert);
        if (now == before)
          continue;
        ++changed;
        const int s16 = negated ? -raw : raw;
        const int m = ((s16 % 257) + 257) % 257;
        ASSERT_TRUE(m == 128 || m == 129) << raw << " " << negated << " " << invert;
        ASSERT_EQ(std::abs(now - before), 1) << raw << " " << negated << " " << invert;
        ASSERT_EQ(now - before, (v < 0) != invert ? -1 : 1) << raw << " " << negated << " " << invert;
      }
    }
  }
  EXPECT_GT(changed, 0);
  StandardPad p;
  for (const int raw : {0, 1, -1, 100, -100, 1000, -1000, 32767, -32768})
  {
    p.axes = {HelperAxis(raw), HelperAxis(raw), 0, 0};
    EXPECT_EQ(MapStandardPad(p).stickX, LegacyAxisByte(HelperAxis(raw), false)) << raw;
    EXPECT_EQ(MapStandardPad(p).stickY, LegacyAxisByte(HelperAxis(raw), true)) << raw;
  }
}

namespace
{
std::vector<Orca::Events::PortInfo> Ports(std::initializer_list<Orca::Events::PortInfo> list)
{
  return list;
}
}

TEST(OrcaUXOverlay, ToastsFollowThePortListOnceEach)
{
  OverlayModel m;
  m.OnPorts(0, Ports({{0, "cy", false}}), 0);
  EXPECT_TRUE(m.View(10, Stats::Off).toasts.empty());
  m.OnPorts(300, Ports({{0, "cy", false}, {1, "ada", true}}), 1000);
  m.OnPorts(300, Ports({{0, "cy", false}, {1, "ada", true}}), 1010);
  auto v = m.View(1100, Stats::Off);
  ASSERT_EQ(v.toasts.size(), 1u);
  EXPECT_EQ(v.toasts[0].text, "ada joined on port 2");
  EXPECT_FLOAT_EQ(v.toasts[0].alpha, 1);
  EXPECT_NEAR(m.View(3750, Stats::Off).toasts[0].alpha, 0.5f, 1e-4);
  EXPECT_TRUE(m.View(4001, Stats::Off).toasts.empty());
  m.OnPorts(900, Ports({{0, "cy", false}}), 5000);
  v = m.View(5001, Stats::Off);
  ASSERT_EQ(v.toasts.size(), 1u);
  EXPECT_EQ(v.toasts[0].text, "ada left");
  m.OnPorts(120, Ports({{0, "bo", true}, {1, "cy", false}}), 5100);
  const auto joined = m.View(5101, Stats::Off).toasts;
  ASSERT_EQ(joined.size(), 2u);
  EXPECT_EQ(joined[1].text, "bo joined on port 1");

  OverlayModel joiner;
  joiner.OnPorts(0, Ports({{0, "cy", true}, {1, "ada", false}}), 0);
  ASSERT_EQ(joiner.View(1, Stats::Off).toasts.size(), 1u);
  EXPECT_EQ(joiner.View(1, Stats::Off).toasts[0].text, "cy is on port 1");
}

TEST(OrcaUXOverlay, NothingInAPictureTooThinForText)
{
  for (const float h : {0.0f, 1.0f, 2.0f, 16.0f, 17.0f, 99.0f, -5.0f, std::nanf("")})
    EXPECT_EQ(OverlayUnit(h), 0.0f) << h;
  EXPECT_FLOAT_EQ(OverlayUnit(OVERLAY_MIN_HEIGHT), OVERLAY_MIN_HEIGHT / 100);
  EXPECT_FLOAT_EQ(OverlayUnit(1080), 10.8f);
  for (float h = OVERLAY_MIN_HEIGHT; h < 4000; h += 0.5f)
    ASSERT_GE(std::floor(3 * OverlayUnit(h) + 0.5f), 1.0f) << h;
}

class OrcaUXOverlayDraw : public ::testing::Test
{
protected:
  void SetUp() override
  {
    m_context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |=
        ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    io.Fonts->AddFontDefault();
    InitOverlay();
    ShowToast("ada joined on port 2");
  }
  void TearDown() override
  {
    ShutdownOverlay();
    ImGui::DestroyContext(m_context);
  }
  static int Frame(float width, float height)
  {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(width, height);
    io.DeltaTime = 1.0f / 60;
    ImGui::NewFrame();
    OSD::DrawHostOverlay();
    ImGui::Render();
    return ImGui::GetDrawData()->TotalVtxCount;
  }
  ImGuiContext* m_context = nullptr;
};

TEST_F(OrcaUXOverlayDraw, AnyPictureSizeDrawsWithoutAnImGuiAssert)
{
  const std::size_t failures = ImGuiAssertFailures();
  for (const float h : {480.0f, 0.0f, 1.0f, 10.0f, 16.0f, 17.0f, 33.0f, 99.0f, 100.0f, 1080.0f})
  {
    SCOPED_TRACE(h);
    const int vertices = Frame(640, h);
    if (h >= OVERLAY_MIN_HEIGHT)
      EXPECT_GT(vertices, 0);
    else
      EXPECT_EQ(vertices, 0);
    EXPECT_EQ(Frame(0, h), 0);
  }
  EXPECT_EQ(ImGuiAssertFailures(), failures);
}

TEST_F(OrcaUXOverlayDraw, ACutWindowKeepsOrcasOwnElementsInIt)
{
  const std::size_t failures = ImGuiAssertFailures();
  constexpr float PIC_W = 1280, PIC_H = 720;
  const auto drawn = [&](float width, float cut_top) {
    SetPictureForTests(PictureArea{width - PIC_W, -cut_top, PIC_W, PIC_H});
    EXPECT_GT(Frame(width, PIC_H - cut_top), 0);
    ImVec2 min(0, 0), max(0, 0);
    for (const ImDrawList* list : ImGui::GetDrawData()->CmdLists)
    {
      for (const ImDrawVert& v : list->VtxBuffer)
      {
        min = ImVec2(std::min(min.x, v.pos.x), std::min(min.y, v.pos.y));
        max = ImVec2(std::max(max.x, v.pos.x), std::max(max.y, v.pos.y));
      }
    }
    return std::pair{min, max};
  };
  const auto banner = [](bool on) {
    if (!on)
      SetRulesLines("", "");
    else
      SetRulesLines("SET 1-0 \u00b7 GAME 2",
                    "Ada strikes a stage \u00b7 then Bo strikes two \u00b7 Ada picks from the "
                    "rest \u00b7 0:42");
  };
  const auto banner_depth = [&](float width) {
    banner(false);
    const float without = drawn(width, 0).second.y;
    banner(true);
    return drawn(width, 0).second.y - without;
  };
  const float floor_depth = banner_depth(300);
  EXPECT_GT(floor_depth, 0);
  for (const float width : {33.0f, 40.0f, 100.0f, 180.0f})
  {
    SCOPED_TRACE(fmt::format("window {} wide", width));
    EXPECT_LE(banner_depth(width), floor_depth + 1);
  }
  banner(true);
  for (const float cut_top : {0.0f, 120.0f})
  {
    for (const float width :
         {33.0f, 40.0f, 100.0f, 180.0f, 300.0f, 512.0f, 640.0f, 840.0f, 1280.0f})
    {
      SCOPED_TRACE(fmt::format("window {}x{}", width, PIC_H - cut_top));
      const auto [min, max] = drawn(width, cut_top);
      if (width < 0.4f * PIC_W)
        continue;
      EXPECT_GE(min.x, -1.0f);
      EXPECT_GE(min.y, -1.0f);
      EXPECT_LE(max.x, width + 1);
      EXPECT_LE(max.y, PIC_H - cut_top + 1);
    }
  }
  banner(false);
  SetPictureForTests(std::nullopt);
  EXPECT_EQ(ImGuiAssertFailures(), failures);
}

TEST_F(OrcaUXOverlayDraw, DimIsTheLastThingDrawnOverTheWholeDisplay)
{
  const auto dim_alpha = [](float width, float height) {
    Frame(width, height);
    const ImDrawData* data = ImGui::GetDrawData();
    if (data->CmdLists.Size == 0)
      return -1;
    const ImDrawList* list = data->CmdLists.back();
    if (list->VtxBuffer.Size < 4)
      return -1;
    const ImDrawVert* v = &list->VtxBuffer[list->VtxBuffer.Size - 4];
    float x0 = v[0].pos.x, y0 = v[0].pos.y, x1 = x0, y1 = y0;
    for (int i = 1; i < 4; ++i)
    {
      x0 = std::min(x0, v[i].pos.x);
      y0 = std::min(y0, v[i].pos.y);
      x1 = std::max(x1, v[i].pos.x);
      y1 = std::max(y1, v[i].pos.y);
    }
    const ImU32 col = v[0].col;
    if (x0 != 0 || y0 != 0 || x1 != width || y1 != height || (col & 0x00FFFFFF) != 0)
      return -1;
    return static_cast<int>(col >> IM_COL32_A_SHIFT);
  };
  EXPECT_EQ(Dim(), 0);
  EXPECT_EQ(dim_alpha(1280, 720), -1);

  SetDim(50);
  EXPECT_EQ(Dim(), 50);
  const int first = dim_alpha(1280, 720);
  EXPECT_GT(first, 0);
  EXPECT_LT(first, 64);
  int alpha = first;
  for (int i = 0; i < 20; ++i)
  {
    const int next = dim_alpha(1280, 720);
    EXPECT_GE(next, alpha);
    alpha = next;
  }
  EXPECT_EQ(alpha, 128);
  EXPECT_EQ(dim_alpha(640, 40), 128);

  SetDim(0);
  const int fading = dim_alpha(1280, 720);
  EXPECT_GT(fading, 0);
  EXPECT_LT(fading, 128);
  for (int i = 0; i < 20; ++i)
    Frame(1280, 720);
  EXPECT_EQ(dim_alpha(1280, 720), -1);

  SetDim(250);
  EXPECT_EQ(Dim(), 100);
  SetDim(-4);
  EXPECT_EQ(Dim(), 0);
}

TEST(OrcaUXOverlay, ReleaseBuildImGuiAssertCarriesOn)
{
#if defined(_DEBUG) || defined(DEBUGFAST)
  GTEST_SKIP() << "a debug build stops on an ImGui assert";
#else
  const std::size_t failures = ImGuiAssertFailures();
  ::testing::internal::CaptureStderr();
  for (int i = 0; i < 3; ++i)
    IM_ASSERT(i < 0 && "the test's own assert");
  const std::string said = ::testing::internal::GetCapturedStderr();
  EXPECT_EQ(ImGuiAssertFailures(), failures + 3);
  std::size_t times = 0;
  for (std::size_t at = said.find("ImGui assert failed"); at != std::string::npos;
       at = said.find("ImGui assert failed", at + 1))
  {
    ++times;
  }
  EXPECT_EQ(times, 1u) << said;
  EXPECT_NE(said.find("the test's own assert"), std::string::npos) << said;
#endif
}

TEST(OrcaUXOverlay, WaitingOnlyAfterAQuarterSecondOfStall)
{
  OverlayModel m;
  m.OnPorts(0, Ports({{0, "cy", false}, {1, "ada", true}}), 0);
  LinkView link;
  link.online = true;
  link.round_trip_ms = 40;
  link.stalled_ms = 200;
  link.waiting_seat = 1;
  m.Update(link, 100);
  EXPECT_EQ(m.View(100, Stats::Off).waiting, "");
  link.stalled_ms = 260;
  m.Update(link, 160);
  EXPECT_EQ(m.View(160, Stats::Off).waiting, "Waiting for ada\u2026");
  EXPECT_EQ(m.View(160, Stats::Off).signal, Signal::Bad);
  link.stalled_ms = -1;
  m.Update(link, 200);
  EXPECT_EQ(m.View(200, Stats::Off).waiting, "");
  link.online = false;
  link.stalled_ms = 900;
  m.Update(link, 300);
  EXPECT_FALSE(m.View(300, Stats::Off).show_signal);
  EXPECT_EQ(m.View(300, Stats::Off).waiting, "");
}

TEST(OrcaUXOverlay, SignalFromRoundTripAndRecentStalls)
{
  LinkView link;
  link.online = true;
  link.stalled_ms = 0;
  const auto signal_at = [](LinkView l, std::vector<std::pair<double, int>> stalls) {
    OverlayModel m;
    for (auto [t, n] : stalls)
    {
      l.stalls = n;
      m.Update(l, t);
    }
    return m.View(stalls.back().first, Stats::Off).signal;
  };
  link.round_trip_ms = 30;
  EXPECT_EQ(signal_at(link, {{0, 5}, {1000, 5}}), Signal::Good);
  EXPECT_EQ(signal_at(link, {{0, 5}, {1000, 7}}), Signal::Ok);
  EXPECT_EQ(signal_at(link, {{0, 5}, {1000, 15}}), Signal::Bad);
  EXPECT_EQ(signal_at(link, {{0, 5}, {1000, 15}, {4500, 15}}), Signal::Good);
  link.round_trip_ms = 90;
  EXPECT_EQ(signal_at(link, {{0, 0}}), Signal::Ok);
  link.round_trip_ms = 160;
  EXPECT_EQ(signal_at(link, {{0, 0}}), Signal::Bad);
  link.round_trip_ms = -1;
  EXPECT_EQ(signal_at(link, {{0, 0}}), Signal::Good);
}

TEST(OrcaUXOverlay, StatsReadoutOnlyWhenAskedAndOnline)
{
  OverlayModel m;
  LinkView link;
  link.online = true;
  link.round_trip_ms = 42;
  link.delay = 2;
  link.rollbacks_per_second = 1.25;
  m.Update(link, 0);
  EXPECT_EQ(m.View(0, Stats::Off).stats, "");
  EXPECT_EQ(m.View(0, Stats::Ping).stats, "42 ms");
  EXPECT_EQ(m.View(0, Stats::Full).stats, "42 ms \u00b7 delay 2 \u00b7 1.2 rb/s");
  EXPECT_TRUE(m.View(0, Stats::Ping).show_signal);
  link.round_trip_ms = -1;
  m.Update(link, 1);
  EXPECT_EQ(m.View(1, Stats::Ping).stats, "\u2013 ms");
  link.online = false;
  m.Update(link, 2);
  EXPECT_EQ(m.View(2, Stats::Ping).stats, "");
  EXPECT_EQ(m.View(2, Stats::Full).stats, "");
}

namespace
{
struct Pacer
{
  double t = 0;
  int k = 0;
  bool started = false;
  double Until(OverlayModel& m, double to)
  {
    if (!std::exchange(started, true))
      m.OnFrame(t, true);
    for (;;)
    {
      const double next = t + (k % 3 == 0 ? 16 : 17);
      if (next > to)
        return t;
      t = next;
      ++k;
      m.OnFrame(t, true);
    }
  }
  double Late(OverlayModel& m, double ms)
  {
    t += ms;
    m.OnFrame(t, true);
    return t;
  }
};

double RunEvery(OverlayModel& m, double ms, double to)
{
  double t = 0;
  for (; t + ms <= to; t += ms)
    m.OnFrame(t + ms, true);
  return t;
}
}

TEST(OrcaUXOverlay, FrameMeterTextSoloAfterItsFirstSecond)
{
  OverlayModel m;
  Pacer p;
  EXPECT_EQ(m.View(0, Stats::Ping, Perf::Fps).perf, "");
  double t = p.Until(m, 990);
  EXPECT_EQ(m.View(t, Stats::Ping, Perf::Fps).perf, "");
  t = p.Until(m, 3000);
  const auto f = m.View(t, Stats::Ping, Perf::Fps);
  EXPECT_FALSE(f.show_signal);
  EXPECT_EQ(f.stats, "");
  EXPECT_EQ(f.perf, "60 fps · 17 ms");
  EXPECT_EQ(f.perf_level, Signal::Good);
  EXPECT_TRUE(f.graph.empty());
  EXPECT_EQ(m.View(t, Stats::Ping, Perf::Off).perf, "");
  EXPECT_EQ(m.View(t + 9, Stats::Ping, Perf::Fps).perf, "60 fps · 17 ms");
}

TEST(OrcaUXOverlay, FrameMeterColourThresholds)
{
  const auto at_rate = [](double ms) {
    OverlayModel m;
    return m.View(RunEvery(m, ms, 4000), Stats::Off, Perf::Fps);
  };
  auto f = at_rate(18.2);
  EXPECT_EQ(f.perf, "55 fps · 18 ms");
  EXPECT_EQ(f.perf_level, Signal::Ok);
  f = at_rate(22.3);
  EXPECT_EQ(f.perf, "45 fps · 22 ms");
  EXPECT_EQ(f.perf_level, Signal::Bad);
  EXPECT_EQ(at_rate(33.4).perf_level, Signal::Bad);
  f = at_rate(1000 / 59.94);
  EXPECT_EQ(f.perf, "60 fps · 17 ms");
  EXPECT_EQ(f.perf_level, Signal::Good);

  const auto one_late = [](double late_ms) {
    OverlayModel m;
    Pacer p;
    p.Until(m, 3000);
    p.Late(m, late_ms);
    return m.View(p.Until(m, p.t + 200), Stats::Off, Perf::Fps);
  };
  f = one_late(19);
  EXPECT_EQ(f.perf, "60 fps · 19 ms");
  EXPECT_EQ(f.perf_level, Signal::Good);
  f = one_late(25);
  EXPECT_EQ(f.perf, "60 fps · 25 ms");
  EXPECT_EQ(f.perf_level, Signal::Ok);
  f = one_late(40);
  EXPECT_EQ(f.perf, "59 fps · 40 ms");
  EXPECT_EQ(f.perf_level, Signal::Bad);
}

TEST(OrcaUXOverlay, FrameMeterHoldsTheColourTwoSecondsAfterTheReadoutClears)
{
  OverlayModel m;
  Pacer p;
  p.Until(m, 3000);
  const double hitch = p.Late(m, 300);
  auto f = m.View(p.Until(m, hitch + 500), Stats::Off, Perf::Fps);
  EXPECT_EQ(f.perf, "43 fps · 300 ms");
  EXPECT_EQ(f.perf_level, Signal::Bad);
  f = m.View(p.Until(m, hitch + 1200), Stats::Off, Perf::Fps);
  EXPECT_EQ(f.perf, "60 fps · 17 ms");
  EXPECT_EQ(f.perf_level, Signal::Bad);
  EXPECT_EQ(m.View(p.Until(m, hitch + 2900), Stats::Off, Perf::Fps).perf_level, Signal::Bad);
  f = m.View(p.Until(m, hitch + 3100), Stats::Off, Perf::Fps);
  EXPECT_EQ(f.perf, "60 fps · 17 ms");
  EXPECT_EQ(f.perf_level, Signal::Good);

  const double dip = p.Late(m, 26);
  f = m.View(p.Until(m, dip + 400), Stats::Off, Perf::Fps);
  EXPECT_EQ(f.perf, "60 fps · 26 ms");
  EXPECT_EQ(f.perf_level, Signal::Ok);
  EXPECT_EQ(m.View(p.Until(m, dip + 2900), Stats::Off, Perf::Fps).perf_level, Signal::Ok);
  EXPECT_EQ(m.View(p.Until(m, dip + 3100), Stats::Off, Perf::Fps).perf_level, Signal::Good);
}

TEST(OrcaUXOverlay, FrameMeterCountsShownFramesOnlyAndTimesReRunsIntoThem)
{
  OverlayModel m;
  double t = 0;
  for (int k = 0; k < 240; ++k)
  {
    m.OnFrame(t, true);
    if (k % 6 == 5)
    {
      for (int r = 1; r <= 4; ++r)
        m.OnFrame(t + 1.5 * r, false);
      t += 6;
    }
    t += k % 3 == 0 ? 16 : 17;
  }
  const auto f = m.View(t - 1, Stats::Off, Perf::Fps);
  EXPECT_EQ(f.perf, "56 fps · 23 ms");
  EXPECT_EQ(f.perf_level, Signal::Ok);

  OverlayModel joiner;
  Pacer p;
  t = p.Until(joiner, 2000);
  for (int k = 1; k <= 20; ++k)
    joiner.OnFrame(t + k, false);
  EXPECT_EQ(joiner.View(p.Late(joiner, 60), Stats::Off, Perf::Fps).perf, "58 fps · 60 ms");
  t = p.Until(joiner, 5000);
  for (int k = 1; k <= 300; ++k)
    joiner.OnFrame(t + k, false);
  t = p.Late(joiner, 2000);
  EXPECT_EQ(joiner.View(t, Stats::Off, Perf::Fps).perf, "");
  const auto after = joiner.View(p.Until(joiner, t + 1500), Stats::Off, Perf::Fps);
  EXPECT_EQ(after.perf, "60 fps · 17 ms");
  EXPECT_EQ(after.perf_level, Signal::Good);
}

TEST(OrcaUXOverlay, FrameMeterStandsStillWhilePausedAndShowsAStallAsItLasts)
{
  OverlayModel m;
  Pacer p;
  double t = p.Until(m, 3000);
  const auto before = m.View(t + 5, Stats::Off, Perf::Detailed);
  ASSERT_EQ(before.perf, "60 fps · 17 ms");
  m.Pause(t + 5);
  for (double later : {5.0, 700.0, 1500.0, 60000.0})
  {
    const auto f = m.View(t + later, Stats::Off, Perf::Detailed);
    EXPECT_EQ(f.perf, before.perf);
    EXPECT_EQ(f.perf_level, Signal::Good);
    EXPECT_EQ(f.graph, before.graph);
  }
  m.Resume(t + 60000);
  p.t = t + 60000 + 12;
  m.OnFrame(p.t, true);
  auto f = m.View(p.t, Stats::Off, Perf::Fps);
  EXPECT_EQ(f.perf, "60 fps · 17 ms");
  EXPECT_EQ(f.perf_level, Signal::Good);

  t = p.Until(m, p.t + 2000);
  f = m.View(t + 400, Stats::Off, Perf::Fps);
  EXPECT_EQ(f.perf, "36 fps · 400 ms");
  EXPECT_EQ(f.perf_level, Signal::Bad);

  OverlayModel early;
  early.OnFrame(0, true);
  early.Pause(10);
  EXPECT_EQ(early.View(5000, Stats::Off, Perf::Fps).perf, "");
}

TEST(OrcaUXOverlay, FrameMeterTimesOnlyTheRunningPartOfAFrameAPauseCutsIn)
{
  const auto no_negative_bars = [](const OverlayModel::Frame& f) {
    return std::all_of(f.graph.begin(), f.graph.end(), [](float ms) { return ms > 0; });
  };
  {
    OverlayModel m;
    Pacer p;
    double t = p.Until(m, 3000);
    m.Pause(t + 5);
    p.t = t + 5000 + 12;
    m.OnFrame(p.t, true);
    m.Resume(p.t + 1);
    t = p.Until(m, p.t + 600);
    const auto f = m.View(t, Stats::Off, Perf::Detailed);
    EXPECT_EQ(f.perf, "61 fps · 17 ms");
    EXPECT_EQ(f.perf_level, Signal::Good);
    EXPECT_TRUE(no_negative_bars(f));
    EXPECT_LE(*std::max_element(f.graph.begin(), f.graph.end()), 17.0f);
  }
  {
    OverlayModel m;
    Pacer p;
    double t = p.Until(m, 3000);
    m.Pause(t + 10);
    m.OnFrame(t + 16, true);
    m.Resume(t + 5000);
    p.t = t + 5000 + 10;
    m.OnFrame(p.t, true);
    t = p.Until(m, p.t + 600);
    const auto f = m.View(t, Stats::Off, Perf::Detailed);
    EXPECT_EQ(f.perf, "61 fps · 17 ms");
    EXPECT_EQ(f.perf_level, Signal::Good);
    EXPECT_TRUE(no_negative_bars(f));
    EXPECT_LE(*std::max_element(f.graph.begin(), f.graph.end()), 17.0f);
  }
}

TEST(OrcaUXOverlay, FrameMeterStartsAfterTheFirstBoundarysWait)
{
  OverlayModel m;
  m.OnFrame(0, true);
  Pacer p;
  p.t = 450;
  const double t = p.Until(m, 1400);
  EXPECT_EQ(m.View(t, Stats::Off, Perf::Fps).perf, "");
  const auto f = m.View(p.Until(m, 2000), Stats::Off, Perf::Fps);
  EXPECT_EQ(f.perf, "60 fps · 17 ms");
  EXPECT_EQ(f.perf_level, Signal::Good);
}

TEST(OrcaUXOverlay, GameHeldTimeIsWhatTheGameTookPastOneFrame)
{
  using Orca::Events::GameHeldFrames;
  using Orca::Events::GameHeldMs;
  constexpr double frame = 1000 / 59.94;
  EXPECT_EQ(GameHeldMs(frame, 100, frame), 0);
  EXPECT_EQ(GameHeldMs(16.4, 100, frame), 0);
  EXPECT_EQ(GameHeldMs(24, 100, frame), 0);
  EXPECT_NEAR(GameHeldMs(2 * frame, 100, frame), frame, 1e-9);
  EXPECT_NEAR(GameHeldMs(306.1, 320, frame), 306.1 - frame, 1e-9);
  EXPECT_NEAR(GameHeldMs(314.5, 260.9, frame), 260.9 - frame, 1e-9);
  EXPECT_NEAR(GameHeldMs(60000, 17, frame), 17 - frame, 1e-9);
  EXPECT_EQ(GameHeldMs(300, 10, frame), 0);
  EXPECT_EQ(GameHeldMs(100, 100, 0), 0);

  double carry = 0;
  EXPECT_EQ(GameHeldFrames(306.1 - frame, frame, carry), 17);
  EXPECT_EQ(GameHeldFrames(0, frame, carry), 0);
  EXPECT_EQ(GameHeldFrames(60 - frame, frame, carry) + GameHeldFrames(122 - frame, frame, carry),
            9);
  carry = 0;
  EXPECT_EQ(GameHeldFrames(20, 20, carry), 1);
  EXPECT_EQ(GameHeldFrames(30, 20, carry), 2);
  EXPECT_EQ(GameHeldFrames(30, 20, carry), 1);
  EXPECT_EQ(GameHeldFrames(30, 20, carry), 2);
  EXPECT_EQ(GameHeldFrames(10, 20, carry), 0);
  EXPECT_EQ(GameHeldFrames(10, 20, carry), 1);
  EXPECT_EQ(GameHeldFrames(10, 20, carry), 0);
  EXPECT_EQ(carry, 0);
  EXPECT_EQ(GameHeldFrames(100, 0, carry), 0);
}

TEST(OrcaUXOverlay, FrameMeterLeavesOutWhatTheGameItselfHeld)
{
  constexpr double frame = 1000 / 59.94;
  double carry = 0;
  const auto boundary = [&](OverlayModel& m, Pacer& p, double emulated, double wall) {
    p.t += wall;
    const double held = Orca::Events::GameHeldMs(emulated, wall, frame);
    m.OnFrame(p.t, true, held, Orca::Events::GameHeldFrames(held, frame, carry));
  };
  const auto fps = [](const OverlayModel::Frame& f) { return std::stoi(f.perf); };
  {
    OverlayModel m;
    Pacer p;
    p.Until(m, 3000);
    boundary(m, p, 300, 300);
    const double load = p.t;
    for (double later : {200.0, 500.0, 900.0})
    {
      const auto f = m.View(p.Until(m, load + later), Stats::Off, Perf::Detailed);
      EXPECT_EQ(f.perf, "60 fps · 17 ms") << later;
      EXPECT_EQ(f.perf_level, Signal::Good) << later;
      EXPECT_LE(*std::max_element(f.graph.begin(), f.graph.end()), 17.0f) << later;
    }
  }
  {
    OverlayModel m;
    Pacer p;
    p.Until(m, 3000);
    for (double load : {306.0, 60.0, 122.0})
    {
      boundary(m, p, load, load);
      p.Until(m, p.t + 100);
    }
    for (double later : {0.0, 300.0, 800.0})
    {
      const auto f = m.View(p.Until(m, p.t + later), Stats::Off, Perf::Fps);
      EXPECT_GE(fps(f), 59) << later << ": " << f.perf;
      EXPECT_LE(fps(f), 61) << later << ": " << f.perf;
      EXPECT_EQ(f.perf_level, Signal::Good) << later << ": " << f.perf;
    }
  }
  {
    OverlayModel m;
    Pacer p;
    p.Until(m, 3000);
    boundary(m, p, 314.5, 260.9);
    auto f = m.View(p.Until(m, p.t + 500), Stats::Off, Perf::Fps);
    EXPECT_LE(fps(f), 61) << f.perf;
    EXPECT_EQ(f.perf_level, Signal::Good) << f.perf;
    boundary(m, p, 60000, 17);
    f = m.View(p.Until(m, p.t + 500), Stats::Off, Perf::Fps);
    EXPECT_EQ(f.perf, "60 fps · 17 ms");
  }
  {
    OverlayModel m;
    Pacer p;
    p.Until(m, 3000);
    boundary(m, p, 300, 500);
    const auto f = m.View(p.Until(m, p.t + 300), Stats::Off, Perf::Fps);
    EXPECT_EQ(f.perf, "48 fps · 217 ms");
    EXPECT_EQ(f.perf_level, Signal::Bad);
  }
  {
    OverlayModel m;
    Pacer p;
    double t = p.Until(m, 3000);
    m.OnFrame(t + 2, false, 200, 12);
    const auto f = m.View(p.Until(m, t + 500), Stats::Off, Perf::Fps);
    EXPECT_EQ(f.perf, "60 fps · 17 ms");
  }
}

TEST(OrcaUXOverlay, FrameMeterDetailedAddsTheGraphAndOnlineTheDelay)
{
  OverlayModel m;
  Pacer p;
  p.Until(m, 2500);
  p.Late(m, 40);
  const double t = p.Until(m, p.t + 500);
  auto f = m.View(t, Stats::Ping, Perf::Detailed);
  EXPECT_EQ(f.perf, "59 fps · 40 ms");
  EXPECT_EQ(f.graph.size(), 119u);
  EXPECT_EQ(*std::max_element(f.graph.begin(), f.graph.end()), 40.0f);
  EXPECT_LE(f.graph.back(), 17.0f);
  EXPECT_TRUE(m.View(t, Stats::Ping, Perf::Fps).graph.empty());

  LinkView link;
  link.online = true;
  link.round_trip_ms = 32;
  link.delay = 2;
  link.rollbacks_per_second = 1.25;
  m.Update(link, t);
  f = m.View(t, Stats::Ping, Perf::Detailed);
  EXPECT_TRUE(f.show_signal);
  EXPECT_EQ(f.stats, "32 ms");
  EXPECT_EQ(f.perf, "59 fps · 40 ms · delay 2 · 1.2 rb/s");
  f = m.View(t, Stats::Full, Perf::Detailed);
  EXPECT_EQ(f.stats, "32 ms · delay 2 · 1.2 rb/s");
  EXPECT_EQ(f.perf, "59 fps · 40 ms");
  EXPECT_EQ(m.View(t, Stats::Ping, Perf::Fps).perf, "59 fps · 40 ms");
}

namespace
{
class FakeBrawl final : public GuestMemory
{
public:
  static constexpr u32 SCENE = 0x90ff6340, NAME = 0x806ff278, TASK = 0x815e75e0,
                       GLOBAL = 0x90181300, RECORDS = 0x90172d40;
  static u32 Area(int i) { return 0x81500000 + static_cast<u32>(i) * 0x1000; }
  static u32 Entry(int i) { return RECORDS + static_cast<u32>(i) * 0x124; }
  static u32 Tag(int i) { return Entry(i) + 0xE0; }
  static constexpr u32 END = RECORDS + 120 * 0x124, GUARD = 0x100;
  static u8 GuardByte(u32 a) { return static_cast<u8>(0xC5 ^ (a & 0xFF)); }

  explicit FakeBrawl(const char* scene = "scSelctCharacter")
  {
    W32(0x805A0060, 0x805b8ba0);
    W32(0x805b8ba0 + 4, SCENE);
    W32(SCENE, NAME);
    for (size_t i = 0; i <= std::strlen(scene); ++i)
      bytes[NAME + static_cast<u32>(i)] = static_cast<u8>(scene[i]);
    W32(SCENE + 0x400, TASK);
    W32(0x805A00E0, GLOBAL);
    for (u32 i = 0; i < CONTROLS.size(); ++i)
      bytes[0x80406938 + i] = CONTROLS[i];
    W32(GLOBAL + 0x28, RECORDS);
    for (u32 a = RECORDS; a < END; a += 2)
      Write16(a, 0);
    for (u32 a = END; a < END + GUARD; ++a)
      bytes[a] = GuardByte(a);
    for (int i = 0; i < 4; ++i)
    {
      W32(TASK + 0x44 + 4 * static_cast<u32>(i), Area(i));
      for (u32 a = Area(i); a < Area(i) + 0x448; a += 4)
        W32(a, 0);
      W32(Area(i) + 0x1B0, static_cast<u32>(i));
      W32(Area(i) + 0x1B4, 0);
      W32(Area(i) + 0x1C8, 0xFFFFFFFF);
    }
  }
  void Join(int i) { W32(Area(i) + 0x1B4, 1); }
  static constexpr std::array<u8, 0x2D> CONTROLS{
      0x03, 0x03, 0x04, 0x0a, 0x0b, 0x0c, 0x00, 0x01, 0x05, 0x02, 0x02, 0x80, 0x01, 0x00, 0x05,
      0x09, 0x03, 0x04, 0x02, 0x04, 0x0a, 0x0c, 0x0b, 0x00, 0x01, 0x04, 0x03, 0x02, 0x02, 0x04,
      0x03, 0x40, 0x03, 0x03, 0x04, 0x04, 0x0a, 0x0b, 0x0c, 0x00, 0x01, 0x05, 0x02, 0x02, 0x80};
  void SetTag(int i, const std::u16string& name)
  {
    for (u32 k = 0; k < 6; ++k)
      Write16(Tag(i) + 2 * k, k < name.size() ? static_cast<u16>(name[k]) : 0);
  }
  std::u16string TagName(int i) const
  {
    std::u16string n;
    for (u32 k = 0; k < 5 && Read16(Tag(i) + 2 * k); ++k)
      n.push_back(static_cast<char16_t>(Read16(Tag(i) + 2 * k)));
    return n;
  }
  s32 NameId(int i) const { return static_cast<s32>(Read32(Area(i) + 0x1C8)); }

  bool Valid(u32 a) const override { return bytes.contains(a); }
  u8 Read8(u32 a) const override { return bytes.at(a); }
  u16 Read16(u32 a) const override { return static_cast<u16>(Read8(a) << 8 | Read8(a + 1)); }
  u32 Read32(u32 a) const override { return u32(Read16(a)) << 16 | Read16(a + 2); }
  void Write8(u32 a, u8 v) override
  {
    bytes[a] = v;
    ++writes;
    if (recording)
      written.insert(a);
  }
  void Write16(u32 a, u16 v) override
  {
    bytes[a] = static_cast<u8>(v >> 8);
    bytes[a + 1] = static_cast<u8>(v);
    ++writes;
    if (recording)
    {
      written.insert(a);
      written.insert(a + 1);
    }
  }
  void Write32(u32 a, u32 v) override
  {
    Write16(a, static_cast<u16>(v >> 16));
    Write16(a + 2, static_cast<u16>(v));
  }
  void W32(u32 a, u32 v) { Write32(a, v); }

  std::map<u32, u8> bytes;
  int writes = 0;
  bool recording = false;
  std::set<u32> written;
};
}

TEST(OrcaUXGamePatches, ParsesTheFileAndRefusesAnythingElse)
{
  std::string error;
  const auto ok = ParseGamePatches("# a comment\r\n\n806CAD50 41820010 48000010    # code\n"
                                   "9017B640 * 000001ff\n",
                                   &error);
  ASSERT_TRUE(ok.has_value()) << error;
  ASSERT_EQ(ok->size(), 2u);
  EXPECT_EQ((*ok)[0].address, 0x806CAD50u);
  EXPECT_EQ((*ok)[0].original, std::optional<u32>(0x41820010u));
  EXPECT_EQ((*ok)[0].value, 0x48000010u);
  EXPECT_EQ((*ok)[1].address, 0x9017B640u);
  EXPECT_FALSE((*ok)[1].original.has_value());
  EXPECT_EQ((*ok)[1].value, 0x1FFu);
  EXPECT_TRUE(ParseGamePatches("\xEF\xBB\xBF" "806CAD50 41820010 48000010\n", &error).has_value());
  for (const char* good : {"817FFFFC * 0", "93FFFFFC * 0", "80000000 * 0", "90000000 * 0"})
    EXPECT_TRUE(ParseGamePatches(good, &error).has_value()) << good;
  for (const char* bad : {"806CAD50 41820010", "806CAD51 * 0", "7FFFFFFC * 0", "818000000 * 0",
                          "81800000 * 0", "8FFFFFFC * 0", "806CAD50 4182001G 48000010",
                          "806CAD50 41820010 48000010 1", "94000000 * 0",
                          "9017B640 * 1\n9017B640 * 2"})
  {
    error.clear();
    EXPECT_FALSE(ParseGamePatches(bad, &error).has_value()) << bad;
    EXPECT_FALSE(error.empty()) << bad;
  }
}

TEST(OrcaUXGamePatches, ConsecutiveCodeLinesAreOneGroupWrittenWhole)
{
  const auto p = ParseGamePatches("811A6430 1 A\n811A6434 2 2\n811A6438 3 C\n"
                                  "811A6440 4 D\n"
                                  "9017B640 * 5\n9017B644 * 6\n",
                                  nullptr);
  ASSERT_TRUE(p.has_value());
  ASSERT_EQ(p->size(), 6u);
  EXPECT_FALSE((*p)[0].joins_previous);
  EXPECT_TRUE((*p)[1].joins_previous);
  EXPECT_TRUE((*p)[2].joins_previous);
  EXPECT_FALSE((*p)[3].joins_previous);
  EXPECT_FALSE((*p)[4].joins_previous);
  EXPECT_FALSE((*p)[5].joins_previous);
  const std::span<const GamePatch> group(p->data(), 3);
  const auto applies = [&](std::vector<u32> now) { return GroupApplies(group, now); };
  EXPECT_TRUE(applies({1, 2, 3}));
  EXPECT_FALSE(applies({0xA, 2, 0xC}));
  EXPECT_FALSE(applies({1, 2, 9}));
  EXPECT_FALSE(applies({1, 9, 3}));
  EXPECT_FALSE(applies({0xA, 2, 3}));
  EXPECT_FALSE(applies({1, 2}));
  const std::span<const GamePatch> data(p->data() + 4, 1);
  EXPECT_TRUE(GroupApplies(data, std::vector<u32>{0}));
  EXPECT_FALSE(GroupApplies(data, std::vector<u32>{5}));
  const auto guards = ParseGamePatches("80001000 7 7\n80001004 8 8\n", nullptr);
  ASSERT_TRUE(guards.has_value());
  EXPECT_FALSE(GroupApplies(*guards, std::vector<u32>{7, 8}));
}

namespace
{
std::vector<GamePatch> ShippedPatches(const char* name)
{
  const std::string path =
      (std::filesystem::path(__FILE__).parent_path() / "../../../../Data/Sys/Subspace" / name).string();
  std::string text;
  EXPECT_TRUE(File::ReadFileToString(path, text)) << path;
  std::string error;
  const auto patches = ParseGamePatches(text, &error);
  EXPECT_TRUE(patches.has_value()) << name << ": " << error;
  return patches.value_or(std::vector<GamePatch>{});
}

void ExpectBootSaveGroup(const std::vector<GamePatch>& patches)
{
  int words = 0;
  for (const GamePatch& p : patches)
  {
    if (p.address < 0x811A6430 || p.address > 0x811A6444)
      continue;
    ++words;
    EXPECT_TRUE(p.original.has_value()) << std::hex << p.address;
    EXPECT_EQ(p.joins_previous, p.address != 0x811A6430) << std::hex << p.address;
  }
  EXPECT_EQ(words, 6);
}

void ExpectItemDefaultGroup(const std::vector<GamePatch>& patches, u32 item_switch)
{
  const auto first = std::find_if(patches.begin(), patches.end(),
                                  [](const GamePatch& p) { return p.address == 0x9017BE50; });
  ASSERT_NE(first, patches.end());
  ASSERT_GE(patches.end() - first, 4);
  const std::span<const GamePatch> group(&*first, 4);
  for (size_t i = 0; i < group.size(); ++i)
  {
    EXPECT_EQ(group[i].address, 0x9017BE50u + 4 * i);
    EXPECT_TRUE(group[i].original.has_value()) << std::hex << group[i].address;
    EXPECT_EQ(group[i].joins_previous, i != 0) << std::hex << group[i].address;
  }
  if (first + 4 != patches.end())
    EXPECT_FALSE(first[4].joins_previous) << std::hex << first[4].address;
  constexpr u32 marker = 0x4F524341;
  std::vector<u32> landed;
  for (const GamePatch& p : group)
    landed.push_back(p.value);
  EXPECT_EQ(landed, (std::vector<u32>{0, marker, item_switch, 0xFFFFFFFF}));
  const auto applies = [&](std::vector<u32> now) { return GroupApplies(group, now); };
  EXPECT_TRUE(applies({0x02000000, 0, item_switch, 0xFFFFFFFF}));
  EXPECT_FALSE(applies(landed));
  EXPECT_FALSE(applies({0x02000000, marker, item_switch, 0xFFFFFFFF}));
  EXPECT_FALSE(applies({0x01000000, marker, item_switch, 0xFFFFFFFF}));
  for (const u32 frequency : {0x00000000u, 0x01000000u, 0x03000000u, 0x02000001u})
    EXPECT_FALSE(applies({frequency, 0, item_switch, 0xFFFFFFFF})) << std::hex << frequency;
  EXPECT_FALSE(applies({0x02000000, 0, item_switch & ~1u, 0xFFFFFFFF}));
}
}

TEST(OrcaUXGamePatches, TheShippedBrawlFileParses)
{
  const auto patches = ShippedPatches("RSBE01.patches");
  EXPECT_GT(patches.size(), 10u);
  ExpectBootSaveGroup(patches);
}

TEST(OrcaUXGamePatches, BrawlInputLagFixIsTheMagusCodeInPlace)
{
  // The "Controller Input Lag Fix" [Magus] (Gecko C202AD8C: add r3, r3, r0; subi r3, r3, 0x404)
  // as an in-place rewrite: the 22 loads of the pad copy at 0x8002AD90 read 0x404 lower, every
  // other word of the run only guards, and the 45 words land together once.
  const auto patches = ShippedPatches("RSBE01.patches");
  const auto first = std::find_if(patches.begin(), patches.end(),
                                  [](const GamePatch& p) { return p.address == 0x8002AD8C; });
  ASSERT_NE(first, patches.end());
  constexpr size_t WORDS = 45;
  ASSERT_GE(static_cast<size_t>(patches.end() - first), WORDS);
  const std::span<const GamePatch> group(&*first, WORDS);
  int loads = 0;
  std::vector<u32> originals, landed;
  for (size_t i = 0; i < group.size(); ++i)
  {
    const GamePatch& p = group[i];
    ASSERT_TRUE(p.original.has_value()) << std::hex << p.address;
    EXPECT_EQ(p.address, 0x8002AD8Cu + 4 * i);
    EXPECT_EQ(p.joins_previous, i != 0) << std::hex << p.address;
    const u32 op = *p.original >> 26, ra = (*p.original >> 16) & 31, d = *p.original & 0xFFFF;
    if ((op == 32 || op == 34 || op == 48) && ra == 3)
    {
      ++loads;
      EXPECT_GE(d, 0x444u) << std::hex << p.address;
      EXPECT_EQ(p.value, *p.original - 0x404) << std::hex << p.address;
    }
    else
    {
      EXPECT_EQ(p.value, *p.original) << std::hex << p.address;
    }
    originals.push_back(*p.original);
    landed.push_back(p.value);
  }
  EXPECT_EQ(loads, 22);
  EXPECT_EQ(group[0].value, 0x7C630214u);
  if (first + WORDS != patches.end())
    EXPECT_FALSE(first[WORDS].joins_previous);
  EXPECT_TRUE(GroupApplies(group, originals));
  EXPECT_FALSE(GroupApplies(group, landed));
  for (const GamePatch& p : ShippedPatches("PPLUS32.patches"))
    EXPECT_FALSE(p.address >= 0x8002AD8C && p.address <= 0x8002AE3C) << std::hex << p.address;
}

TEST(OrcaUXGamePatches, BrawlHasNoRandomTripping)
{
  // "No Tripping" [Phantom Wings] (Gecko 0489E910 FFE0F090): in the trip check (0x8089E820) the
  // trip rate param's float (0xDB3, fetched at 0x8089E90C) is replaced by the call's own argument,
  // so the chance is 0 and the check returns before it draws a random number. One changed word in a
  // group of six whose other words are the param fetch around it, read in RAM on scMelee.
  const auto patches = ShippedPatches("RSBE01.patches");
  const auto first = std::find_if(patches.begin(), patches.end(),
                                  [](const GamePatch& p) { return p.address == 0x8089E900; });
  ASSERT_NE(first, patches.end());
  constexpr size_t WORDS = 6;
  ASSERT_GE(static_cast<size_t>(patches.end() - first), WORDS);
  const std::span<const GamePatch> group(&*first, WORDS);
  const std::vector<u32> want_originals{0x7FA3EB78, 0x38800DB3, 0x38A00000,
                                        0x4BEF8361, 0xFFE00890, 0x2C1E0001};
  std::vector<u32> originals, landed;
  for (size_t i = 0; i < group.size(); ++i)
  {
    const GamePatch& p = group[i];
    ASSERT_TRUE(p.original.has_value()) << std::hex << p.address;
    EXPECT_EQ(p.address, 0x8089E900u + 4 * i);
    EXPECT_EQ(p.joins_previous, i != 0) << std::hex << p.address;
    EXPECT_TRUE(p.when.empty()) << std::hex << p.address;
    originals.push_back(*p.original);
    landed.push_back(p.value);
    if (p.address != 0x8089E910)
      EXPECT_EQ(p.value, *p.original) << std::hex << p.address;
  }
  EXPECT_EQ(originals, want_originals);
  EXPECT_EQ(group[4].value, 0xFFE0F090u);
  if (first + WORDS != patches.end())
    EXPECT_FALSE(first[WORDS].joins_previous);
  EXPECT_TRUE(GroupApplies(group, originals));
  EXPECT_FALSE(GroupApplies(group, landed));
  for (const GamePatch& p : ShippedPatches("PPLUS32.patches"))
    EXPECT_FALSE(p.address >= 0x8089E820 && p.address <= 0x8089EC1C) << std::hex << p.address;
}

TEST(OrcaUXGamePatches, BrawlRunsProjectPlusVSyncMove)
{
  for (const char* file : {"RSBE01.patches", "PPLUS32.patches"})
  {
    SCOPED_TRACE(file);
    const auto patches = ShippedPatches(file);
    const auto index_of = [&](u32 address) {
      return static_cast<size_t>(
          std::find_if(patches.begin(), patches.end(),
                       [&](const GamePatch& p) { return p.address == address; }) -
          patches.begin());
    };
    struct Move
    {
      u32 group, address, original, value;
    };
    for (const Move& move : {Move{0x80023B84, 0x80023B88, 0x4182FF7C, 0x60000000},
                             Move{0x80024020, 0x80024028, 0x4E800020, 0x481C4904}})
    {
      const size_t at = index_of(move.group);
      ASSERT_LE(at + 3, patches.size()) << std::hex << move.group;
      const std::span<const GamePatch> group(patches.data() + at, 3);
      EXPECT_FALSE(group[0].joins_previous);
      EXPECT_TRUE(group[1].joins_previous && group[2].joins_previous);
      if (at + 3 < patches.size())
        EXPECT_FALSE(patches[at + 3].joins_previous);
      std::vector<u32> originals;
      for (const GamePatch& p : group)
      {
        ASSERT_TRUE(p.original.has_value());
        EXPECT_TRUE(p.when.empty());
        originals.push_back(*p.original);
        if (p.address == move.address)
        {
          EXPECT_EQ(*p.original, move.original);
          EXPECT_EQ(p.value, move.value);
        }
        else
        {
          EXPECT_EQ(p.value, *p.original) << std::hex << p.address;
        }
      }
      EXPECT_TRUE(GroupApplies(group, originals));
    }
  }
  EXPECT_EQ(0x80024028u + (0x481C4904u & 0x03FFFFFC), 0x801E892Cu);
}

TEST(OrcaUXGamePatches, TheShippedProjectPlusFileGuardsEveryLine)
{
  const auto patches = ShippedPatches("PPLUS32.patches");
  ASSERT_FALSE(patches.empty());
  for (const GamePatch& p : patches)
    EXPECT_TRUE(p.original.has_value()) << std::hex << p.address;
  ExpectBootSaveGroup(patches);
}

TEST(OrcaUXGamePatches, ItemsOffIsOneGroupThatLandsOnlyOnTheGamesDefaults)
{
  {
    SCOPED_TRACE("RSBE01.patches");
    ExpectItemDefaultGroup(ShippedPatches("RSBE01.patches"), 0xFFFFFFFF);
  }
  {
    SCOPED_TRACE("PPLUS32.patches");
    ExpectItemDefaultGroup(ShippedPatches("PPLUS32.patches"), 0xFFE7FFFF);
  }
}

TEST(OrcaUXFreeSpace, TheBlockAndTheCavesFitAndNoShippedPatchStraysIntoThem)
{
  using namespace FreeSpace;
  EXPECT_GE(kMatchBlock.Size(), 96u);
  EXPECT_EQ(kMatchBlock.begin, MatchBlock::BASE);
  EXPECT_EQ(kMatchBlock.end, MatchBlock::FRIENDS_END);
  EXPECT_EQ(kMatchBlock.Size(), MatchBlock::FULL_SIZE);
  EXPECT_EQ(kMatchBlockHead.Size(), 512u);
  EXPECT_EQ(kMatchBlockHead.Size(), MatchBlock::SIZE);
  for (const FreeSpace::Range& cave : kCodeCaves)
    EXPECT_FALSE(cave.Overlaps(kMatchBlock)) << std::hex << cave.begin;
  u32 cave_bytes = 0;
  for (const FreeSpace::Range& cave : kCodeCaves)
    cave_bytes += cave.Size();
  EXPECT_EQ(cave_bytes, 16908u);
  constexpr std::array<FreeSpace::Range, 7> dead{{{0x806F22EC, 0x806F2D24},
                                                  {0x806F2D74, 0x806F30E0},
                                                  {0x806F3130, 0x806F3FA0},
                                                  {0x806F3FF0, 0x806F4C70},
                                                  {0x806F4CC0, 0x806F58B4},
                                                  {0x806F6C74, 0x806F76F8},
                                                  {0x806F7748, 0x806F82A4}}};
  const auto within_dead = [&](const FreeSpace::Range& range) {
    return std::any_of(dead.begin(), dead.end(), [&](const FreeSpace::Range& run) {
      return range.begin >= run.begin && range.end <= run.end;
    });
  };
  EXPECT_TRUE(within_dead(kMatchBlock));
  for (const FreeSpace::Range& cave : kCodeCaves)
    EXPECT_TRUE(within_dead(cave)) << std::hex << cave.begin;
  for (const char* name : {"RSBE01.patches", "PPLUS32.patches"})
  {
    SCOPED_TRACE(name);
    for (const GamePatch& p : ShippedPatches(name))
    {
      EXPECT_FALSE(kMatchBlock.Contains(p.address)) << std::hex << p.address;
      const bool in_dead = std::any_of(dead.begin(), dead.end(), [&](const FreeSpace::Range& run) {
        return run.Contains(p.address);
      });
      const bool in_cave =
          std::any_of(kCodeCaves.begin(), kCodeCaves.end(),
                      [&](const FreeSpace::Range& cave) { return cave.Contains(p.address); });
      EXPECT_TRUE(!in_dead || in_cave) << std::hex << p.address;
      EXPECT_FALSE(
          std::any_of(kSequenceCreates.begin(), kSequenceCreates.end(),
                      [&](const FreeSpace::Range& create) { return create.Contains(p.address); }))
          << std::hex << p.address;
    }
  }
}

TEST(OrcaUXNameTags, UsernamesBecomeBrawlTags)
{
  EXPECT_EQ(BrawlTag("ada"), u"ADA");
  EXPECT_EQ(BrawlTag("longusername"), u"LONGU");
  EXPECT_EQ(BrawlTag("sandbox-ada"), u"SANDB");
  EXPECT_EQ(BrawlTag("a_b c"), u"A-B-C");
  EXPECT_EQ(BrawlTag("Player 2", true), u"P2");
  EXPECT_EQ(BrawlTag("Player 12", true), u"P12");
  EXPECT_EQ(BrawlTag("Player X", true), u"PLAYE");
  EXPECT_EQ(BrawlTag("Player 2"), u"PLAYE");
  EXPECT_EQ(BrawlTag("Player 12"), u"PLAYE");
  EXPECT_EQ(BrawlTag("x.y!?"), u"X.Y!?");
  EXPECT_EQ(BrawlTag("H4l9x"), u"H4L9X");
  EXPECT_EQ(BrawlTag("j\xc3\xa9r\xc3\xb4me"), u"JRME");
  EXPECT_EQ(BrawlTag("@#$"), u"");
  EXPECT_EQ(BrawlTag(""), u"");
  EXPECT_EQ(BrawlTag("\xe5\xb0\x8f\xe6\x98\x8e" + std::string(58, 'q')), u"QQQQQ");
  EXPECT_EQ(BrawlTag(std::string(64, '\xff')), u"");
}

TEST(OrcaUXNameTags, TheRemoteFlagNeverChangesWhatIsWritten)
{
  FakeBrawl a, b;
  for (FakeBrawl* m : {&a, &b})
  {
    m->Join(0);
    m->Join(1);
  }
  ApplyNameTags(a, {{0, "sandbox-ada", false}, {1, "sandbox-bo", true}});
  ApplyNameTags(b, {{0, "sandbox-ada", true}, {1, "sandbox-bo", false}});
  EXPECT_EQ(a.bytes, b.bytes);
}

TEST(OrcaUXNameTags, JoinedPortsGetTheirTagOnTheCharacterSelect)
{
  FakeBrawl m;
  const std::vector<Orca::Events::PortInfo> ports{{0, "cy", false}, {1, "ada", true}};
  EXPECT_EQ(ApplyNameTags(m, ports), 0);
  m.Join(0);
  m.Join(1);
  EXPECT_EQ(ApplyNameTags(m, ports), 2);
  EXPECT_EQ(m.NameId(0), 0);
  EXPECT_EQ(m.TagName(0), u"CY");
  EXPECT_EQ(m.NameId(1), 1);
  EXPECT_EQ(m.TagName(1), u"ADA");
  EXPECT_EQ(m.NameId(2), -1);
  const int writes = m.writes;
  EXPECT_EQ(ApplyNameTags(m, ports), 0);
  EXPECT_EQ(m.writes, writes);
}

TEST(OrcaUXNameTags, ReusesTheSameNameAndLeavesAChosenTagAlone)
{
  FakeBrawl m;
  m.SetTag(0, u"MINE");
  m.SetTag(3, u"ADA");
  m.Join(0);
  m.Join(1);
  m.W32(FakeBrawl::Area(0) + 0x1C8, 0);
  EXPECT_EQ(ApplyNameTags(m, {{0, "cy", false}, {1, "ada", true}}), 1);
  EXPECT_EQ(m.NameId(0), 0);
  EXPECT_EQ(m.TagName(0), u"MINE");
  EXPECT_EQ(m.NameId(1), 3);
  for (int slot : {1, 2, 4, 119})
    EXPECT_EQ(m.TagName(slot), u"") << slot;
}

TEST(OrcaUXNameTags, ReplayViewNamesReplaceAWornTagAndLaterUpdates)
{
  FakeBrawl m;
  m.SetTag(0, u"FILE");
  m.Join(0);
  m.W32(FakeBrawl::Area(0) + 0x1C8, 0);
  EXPECT_EQ(ApplyNameTags(m, {{0, "Alice", false}}, true), 0);
  EXPECT_EQ(m.TagName(0), u"ALICE");
  EXPECT_EQ(m.NameId(0), 0);
  const int writes = m.writes;
  ApplyNameTags(m, {{0, "Alice", false}}, true);
  EXPECT_EQ(m.writes, writes);
  ApplyNameTags(m, {{0, "Bob", false}}, true);
  EXPECT_EQ(m.TagName(0), u"BOB");
  ApplyNameTags(m, {{0, "\xf0\x9f\x90\xac", false}}, true);
  EXPECT_EQ(m.TagName(0), u"P1");
}

TEST(OrcaUXNameTags, LiveNamesAndNameFreeReplayKeepTagDecisions)
{
  FakeBrawl live;
  live.SetTag(0, u"MINE");
  for (int port = 0; port < 3; ++port)
    live.Join(port);
  live.W32(FakeBrawl::Area(0) + 0x1C8, 0);
  const std::vector<Orca::Events::PortInfo> ports{{0, "Alice", false},
                                                {1, "\xf0\x9f\x90\xac", true},
                                                {2, "sandbox-ada", true}};
  const auto stored = Orca::Net::RecordingPorts(ports);
  FakeBrawl played = live;
  for (int i = 0; i < 60; ++i)
  {
    Orca::Net::ReplayFrame frame;
    {
      const Orca::Net::ReplayScope scope(&frame, false);
      NameTagsFrame(live, ports, ports, nullptr, {}, 0);
    }
    {
      const Orca::Net::ReplayScope scope(&frame, true, true, false, true);
      NameTagsFrame(played, stored, stored, nullptr, {}, 0);
    }
    for (int port = 0; port < 3; ++port)
      EXPECT_EQ(live.NameId(port), played.NameId(port));
  }
  EXPECT_EQ(live.TagName(live.NameId(2)), u"SANDB");
  EXPECT_EQ(played.TagName(played.NameId(2)), u"P3");
  EXPECT_EQ(played.NameId(1), -1);
  EXPECT_NE(live.bytes, played.bytes);
}

TEST(OrcaUXNameTags, RollbackReplacesPredictedTagAssignments)
{
  const std::vector<Orca::Events::PortInfo> ports{{0, "Alice", false}};
  for (const bool resimulating : {false, true})
  {
    SCOPED_TRACE(resimulating);
    FakeBrawl confirmed;
    confirmed.SetTag(3, u"MINE");
    confirmed.Join(0);
    confirmed.W32(FakeBrawl::Area(0) + 0x1C8, 3);
    FakeBrawl predicted = confirmed;
    predicted.W32(FakeBrawl::Area(0) + 0x1C8, 0xffffffff);
    Orca::Net::ReplayFrame frame;
    {
      const Orca::Net::ReplayScope scope(&frame, false);
      NameTagsFrame(predicted, ports, ports, nullptr, {}, 0);
    }
    ASSERT_TRUE(frame.tag_assignments);
    ASSERT_EQ(frame.tag_assignments->size(), 1u);
    auto rerun = confirmed;
    {
      const Orca::Net::ReplayScope scope(&frame, resimulating);
      NameTagsFrame(rerun, ports, ports, nullptr, {}, 0);
    }
    ASSERT_TRUE(frame.tag_assignments);
    EXPECT_TRUE(frame.tag_assignments->empty());
    Orca::Net::ReplayFrame clean_frame;
    auto clean = confirmed;
    {
      const Orca::Net::ReplayScope scope(&clean_frame, false);
      NameTagsFrame(clean, ports, ports, nullptr, {}, 0);
    }
    EXPECT_EQ(rerun.bytes, clean.bytes);
    EXPECT_EQ(frame.tag_assignments, clean_frame.tag_assignments);
    auto played = confirmed;
    const auto labeled = Orca::Net::RecordingPorts(ports);
    {
      const Orca::Net::ReplayScope scope(&frame, true, true, false, true);
      NameTagsFrame(played, labeled, labeled, nullptr, {}, 0);
    }
    EXPECT_EQ(played.bytes, clean.bytes);
    EXPECT_EQ(played.NameId(0), 3);
  }
}

TEST(OrcaUXNameTags, RollbackClearsAssignmentsBeforeCharacterSelectChecks)
{
  FakeBrawl memory;
  memory.Join(0);
  const std::vector<Orca::Events::PortInfo> ports{{0, "Alice", false}};
  Orca::Net::ReplayFrame frame;
  {
    const Orca::Net::ReplayScope scope(&frame, false);
    NameTagsFrame(memory, ports, ports, nullptr, {}, 0);
  }
  ASSERT_EQ(frame.tag_assignments->size(), 1u);
  memory.Write8(FakeBrawl::NAME, 'x');
  {
    const Orca::Net::ReplayScope scope(&frame, true);
    NameTagsFrame(memory, ports, ports, nullptr, {}, 0);
  }
  EXPECT_TRUE(frame.tag_assignments->empty());
}

TEST(OrcaUXNameTags, ANewTagIsMadeAsTheGameMakesOneAndNothingElseIsWritten)
{
  FakeBrawl m;
  const u32 entry = FakeBrawl::Entry(0);
  for (u32 a = entry + 0x60; a < entry + 0x80; a += 2)
    m.Write16(a, 0xAAAA);
  m.Write16(entry + 0xEE, 0xAAAA);
  m.W32(entry + 0xF0, 0x261E6EC2);
  m.Join(0);
  m.recording = true;
  ASSERT_EQ(ApplyNameTags(m, {{0, "cy", false}}), 1);
  const u32 tag = FakeBrawl::Tag(0);
  EXPECT_EQ(m.TagName(0), u"CY");
  EXPECT_EQ(m.Read8(tag + 0x0C), 1);
  for (u32 i = 0; i < FakeBrawl::CONTROLS.size(); ++i)
    EXPECT_EQ(m.Read8(tag + 0x14 + i), FakeBrawl::CONTROLS[i]) << i;
  for (u32 a = entry + 0x60; a < entry + 0x80; ++a)
    EXPECT_EQ(m.Read8(a), 0);
  EXPECT_EQ(m.Read16(entry + 0xEE), 0);
  EXPECT_EQ(m.Read32(entry + 0xF0), 0u);
  for (const u32 a : m.written)
  {
    const bool in_tag = a >= entry && a < entry + 0x124;
    const bool in_name_id = a >= FakeBrawl::Area(0) + 0x1C8 && a < FakeBrawl::Area(0) + 0x1CC;
    EXPECT_TRUE(in_tag || in_name_id) << std::hex << a;
  }
}

TEST(OrcaUXNameTags, ATagGoesInTheLowestFreeSlotAndWritesOnlyItsOwnEntry)
{
  {
    FakeBrawl m;
    m.Join(0);
    for (u32 a = FakeBrawl::Entry(0); a < FakeBrawl::Entry(0) + 0xE0; a += 2)
      m.Write16(a, 0xBBBB);
    for (u32 a = FakeBrawl::Entry(1); a < FakeBrawl::Entry(1) + 0xE0; a += 2)
      m.Write16(a, 0x5A5A);
    m.recording = true;
    ASSERT_EQ(ApplyNameTags(m, {{0, "ada", false}}), 1);
    ASSERT_EQ(m.NameId(0), 0);
    for (u32 a = FakeBrawl::Entry(0); a < FakeBrawl::Entry(0) + 0xE0; ++a)
      EXPECT_EQ(m.Read8(a), 0) << std::hex << a;
    for (u32 a = FakeBrawl::Entry(1); a < FakeBrawl::Entry(1) + 0xE0; ++a)
      EXPECT_EQ(m.Read8(a), 0x5A) << std::hex << a;
    for (const u32 a : m.written)
    {
      const bool in_entry = a >= FakeBrawl::Entry(0) && a < FakeBrawl::Entry(1);
      const bool in_name_id = a >= FakeBrawl::Area(0) + 0x1C8 && a < FakeBrawl::Area(0) + 0x1CC;
      EXPECT_TRUE(in_entry || in_name_id) << std::hex << a;
    }
  }
  {
    FakeBrawl m;
    m.SetTag(0, u"MINE");
    m.SetTag(2, u"ALT");
    m.Join(0);
    ASSERT_EQ(ApplyNameTags(m, {{0, "ada", false}}), 1);
    EXPECT_EQ(m.NameId(0), 1);
    EXPECT_EQ(m.TagName(1), u"ADA");
  }
  {
    FakeBrawl m;
    for (int slot = 0; slot < 119; ++slot)
      m.SetTag(slot, u"T");
    m.Join(0);
    m.recording = true;
    ASSERT_EQ(ApplyNameTags(m, {{0, "ada", false}}), 1);
    ASSERT_EQ(m.NameId(0), 119);
    EXPECT_EQ(m.TagName(119), u"ADA");
    for (u32 a = FakeBrawl::END; a < FakeBrawl::END + FakeBrawl::GUARD; ++a)
      EXPECT_EQ(m.Read8(a), FakeBrawl::GuardByte(a)) << std::hex << a;
    for (const u32 a : m.written)
    {
      const bool in_entry = a >= FakeBrawl::Entry(119) && a < FakeBrawl::END;
      const bool in_name_id = a >= FakeBrawl::Area(0) + 0x1C8 && a < FakeBrawl::Area(0) + 0x1CC;
      EXPECT_TRUE(in_entry || in_name_id) << std::hex << a;
    }
  }
  {
    FakeBrawl m;
    for (int slot = 0; slot < 120; ++slot)
      m.SetTag(slot, u"T");
    m.Join(0);
    m.recording = true;
    EXPECT_EQ(ApplyNameTags(m, {{0, "ada", false}}), 0);
    EXPECT_TRUE(m.written.empty());
  }
}

TEST(OrcaUXNameTags, TwoPortsNeverShareATag)
{
  FakeBrawl m;
  m.Join(0);
  m.Join(1);
  m.Join(2);
  EXPECT_EQ(ApplyNameTags(m, {{0, "sandbox-ada", false},
                              {1, "sandbox-bo", true},
                              {2, "sandbox-cy", true}}),
            3);
  EXPECT_EQ(m.TagName(m.NameId(0)), u"SANDB");
  EXPECT_EQ(m.TagName(m.NameId(1)), u"SAND2");
  EXPECT_EQ(m.TagName(m.NameId(2)), u"SAND3");
  FakeBrawl late;
  late.Join(1);
  EXPECT_EQ(ApplyNameTags(late, {{0, "ada", false}, {1, "ada", true}}), 1);
  EXPECT_EQ(late.TagName(late.NameId(1)), u"ADA");
  late.Join(0);
  EXPECT_EQ(ApplyNameTags(late, {{0, "ada", false}, {1, "ada", true}}), 1);
  EXPECT_EQ(late.TagName(late.NameId(0)), u"ADA1");
  FakeBrawl half;
  half.Join(1);
  EXPECT_EQ(ApplyNameTags(half, {{0, "cy", false}, {1, "ada", true}}), 1);
  EXPECT_EQ(half.NameId(0), -1);
  EXPECT_EQ(half.TagName(half.NameId(1)), u"ADA");
}

namespace
{
std::vector<u8> Profile(u8 rumble, std::array<u8, 0x2D> layout)
{
  std::vector<u8> profile{rumble};
  profile.insert(profile.end(), layout.begin(), layout.end());
  return profile;
}
std::array<u8, 0x2D> CustomLayout()
{
  std::array<u8, 0x2D> layout = FakeBrawl::CONTROLS;
  layout[6] = 0x02;
  layout[7] = 0x04;
  layout[11] = 0x00;
  layout[0x2C] = 0x00;
  return layout;
}
std::array<u8, 0x2D> TagLayout(const FakeBrawl& m, int slot)
{
  std::array<u8, 0x2D> layout{};
  for (u32 i = 0; i < layout.size(); ++i)
    layout[i] = m.Read8(FakeBrawl::Tag(slot) + 0x14 + i);
  return layout;
}
void GameMadeTag(FakeBrawl& m, int slot, const std::u16string& name)
{
  m.SetTag(slot, name);
  m.Write8(FakeBrawl::Tag(slot) + 0x0C, 1);
  for (u32 i = 0; i < 0x2D; ++i)
    m.Write8(FakeBrawl::Tag(slot) + 0x14 + i, FakeBrawl::CONTROLS[i]);
}
void Wear(FakeBrawl& m, int port, int slot)
{
  m.W32(FakeBrawl::Area(port) + 0x1C8, static_cast<u32>(slot));
}
}

TEST(OrcaUXNameTags, APortsOwnControlsGoIntoTheTagItGets)
{
  FakeBrawl m;
  m.Join(0);
  m.Join(1);
  const std::vector<Orca::Events::PortInfo> ports{{0, "cy", false, {}},
                                                  {1, "ada", true, Profile(0, CustomLayout())}};
  m.recording = true;
  EXPECT_EQ(ApplyNameTags(m, ports), 2);
  EXPECT_EQ(TagLayout(m, m.NameId(0)), FakeBrawl::CONTROLS);
  EXPECT_EQ(m.Read8(FakeBrawl::Tag(m.NameId(0)) + 0x0C), 1);
  EXPECT_EQ(m.TagName(m.NameId(1)), u"ADA");
  EXPECT_EQ(TagLayout(m, m.NameId(1)), CustomLayout());
  EXPECT_EQ(m.Read8(FakeBrawl::Tag(m.NameId(1)) + 0x0C), 0);
  for (const u32 a : m.written)
  {
    const bool in_tag = (a >= FakeBrawl::Entry(0) && a < FakeBrawl::Entry(2));
    const bool in_name_id = (a >= FakeBrawl::Area(0) + 0x1C8 && a < FakeBrawl::Area(0) + 0x1CC) ||
                            (a >= FakeBrawl::Area(1) + 0x1C8 && a < FakeBrawl::Area(1) + 0x1CC);
    EXPECT_TRUE(in_tag || in_name_id) << std::hex << a;
  }
  const int writes = m.writes;
  EXPECT_EQ(ApplyNameTags(m, ports), 0);
  EXPECT_EQ(m.writes, writes);
  FakeBrawl other;
  other.Join(0);
  other.Join(1);
  ApplyNameTags(other, {{0, "cy", true, {}}, {1, "ada", false, Profile(0, CustomLayout())}});
  EXPECT_EQ(m.bytes, other.bytes);
}

TEST(OrcaUXNameTags, ControlsReplaceThoseOfATagAlreadyHoldingTheName)
{
  FakeBrawl m;
  m.SetTag(7, u"ADA");
  for (u32 i = 0; i < 0x2D; ++i)
    m.Write8(FakeBrawl::Tag(7) + 0x14 + i, FakeBrawl::CONTROLS[i]);
  m.Write8(FakeBrawl::Tag(7) + 0x0C, 1);
  m.Join(1);
  EXPECT_EQ(ApplyNameTags(m, {{1, "ada", true, Profile(0, CustomLayout())}}), 1);
  EXPECT_EQ(m.NameId(1), 7);
  EXPECT_EQ(TagLayout(m, 7), CustomLayout());
  EXPECT_EQ(m.Read8(FakeBrawl::Tag(7) + 0x0C), 0);
  FakeBrawl chosen;
  chosen.SetTag(3, u"MINE");
  chosen.Join(0);
  chosen.W32(FakeBrawl::Area(0) + 0x1C8, 3);
  chosen.recording = true;
  EXPECT_EQ(ApplyNameTags(chosen, {{0, "cy", false, Profile(0, CustomLayout())}}), 0);
  EXPECT_TRUE(chosen.written.empty());
}

TEST(OrcaUXNameTags, ATagThePlayerMakesAgainKeepsItsSelectedDefaults)
{
  FakeBrawl m;
  GameMadeTag(m, 118, u"MYTAG");
  m.Join(0);
  Wear(m, 0, 118);
  const std::vector<Orca::Events::PortInfo> ports{{0, "zeke", false, Profile(0, CustomLayout())}};
  m.recording = true;
  EXPECT_EQ(ApplyNameTags(m, ports), 0);
  EXPECT_EQ(m.NameId(0), 118);
  EXPECT_EQ(TagLayout(m, 118), FakeBrawl::CONTROLS);
  EXPECT_EQ(m.Read8(FakeBrawl::Tag(118) + 0x0C), 1);
  EXPECT_EQ(m.TagName(118), u"MYTAG");
  EXPECT_EQ(m.TagName(0), u"");
  EXPECT_TRUE(m.written.empty());
  const int writes = m.writes;
  EXPECT_EQ(ApplyNameTags(m, ports), 0);
  EXPECT_EQ(m.writes, writes);
  FakeBrawl host, joiner;
  for (FakeBrawl* g : {&host, &joiner})
  {
    GameMadeTag(*g, 40, u"SPARE");
    g->Join(0);
    g->Join(1);
    Wear(*g, 1, 40);
  }
  ApplyNameTags(host, {{0, "cy", false, {}}, {1, "ada", true, Profile(1, CustomLayout())}});
  ApplyNameTags(joiner, {{0, "cy", true, {}}, {1, "ada", false, Profile(1, CustomLayout())}});
  EXPECT_EQ(TagLayout(host, 40), FakeBrawl::CONTROLS);
  EXPECT_EQ(host.Read8(FakeBrawl::Tag(40) + 0x0C), 1);
  EXPECT_EQ(host.TagName(host.NameId(0)), u"CY");
  EXPECT_EQ(TagLayout(host, host.NameId(0)), FakeBrawl::CONTROLS);
  EXPECT_EQ(host.bytes, joiner.bytes);
}

TEST(OrcaUXNameTags, WornTagsKeepEveryMenuChoice)
{
  const std::vector<Orca::Events::PortInfo> carries{{0, "zeke", false, Profile(0, CustomLayout())}};
  const auto untouched = [](FakeBrawl& m, const std::vector<Orca::Events::PortInfo>& ports) {
    m.recording = true;
    EXPECT_EQ(ApplyNameTags(m, ports), 0);
    EXPECT_TRUE(m.written.empty());
  };
  {
    FakeBrawl m;
    GameMadeTag(m, 118, u"MYTAG");
    m.Join(0);
    Wear(m, 0, 118);
    untouched(m, {{0, "zeke", false, {}}});
    std::vector<u8> short_profile = Profile(0, CustomLayout());
    short_profile.pop_back();
    untouched(m, {{0, "zeke", false, short_profile}});
  }
  {
    FakeBrawl rumble_off, one_byte;
    GameMadeTag(rumble_off, 118, u"MYTAG");
    rumble_off.Write8(FakeBrawl::Tag(118) + 0x0C, 0);
    GameMadeTag(one_byte, 118, u"MYTAG");
    one_byte.Write8(FakeBrawl::Tag(118) + 0x14 + 2, 0x02);
    for (FakeBrawl* m : {&rumble_off, &one_byte})
    {
      m->Join(0);
      Wear(*m, 0, 118);
      untouched(*m, carries);
    }
  }
  {
    FakeBrawl m;
    GameMadeTag(m, 100, u"ADAS");
    GameMadeTag(m, 101, u"THEIR");
    GameMadeTag(m, 102, u"NOONE");
    m.Join(0);
    m.Join(1);
    Wear(m, 0, 101);
    Wear(m, 1, 100);
    EXPECT_EQ(ApplyNameTags(m, {{1, "ada", true, Profile(0, CustomLayout())}}), 0);
    EXPECT_EQ(TagLayout(m, 100), FakeBrawl::CONTROLS);
    EXPECT_EQ(TagLayout(m, 101), FakeBrawl::CONTROLS);
    EXPECT_EQ(TagLayout(m, 102), FakeBrawl::CONTROLS);
    EXPECT_EQ(m.Read8(FakeBrawl::Tag(101) + 0x0C), 1);
  }
  {
    FakeBrawl past, unnamed;
    past.Join(0);
    Wear(past, 0, 120);
    untouched(past, carries);
    GameMadeTag(unnamed, 118, u"");
    unnamed.Join(0);
    Wear(unnamed, 0, 118);
    untouched(unnamed, carries);
  }
  {
    FakeBrawl m;
    GameMadeTag(m, 118, u"MYTAG");
    m.Join(0);
    Wear(m, 0, 118);
    untouched(m, {{0, "zeke", false, Profile(1, FakeBrawl::CONTROLS)}});
  }
  {
    FakeBrawl m;
    GameMadeTag(m, 118, u"MYTAG");
    m.Join(0);
    Wear(m, 0, 118);
    m.bytes.erase(0x80406938 + 0x2C);
    untouched(m, carries);
  }
}

TEST(OrcaUXNameTags, ControlsOnlyAsTheGamesMenusSetThem)
{
  std::array<u8, 0x2D> wild = CustomLayout();
  wild[0] = 0x0F;
  wild[8] = 0xFF;
  wild[11] = 0xFF;
  wild[0x1F] = 0xFF;
  wild[0x2C] = 0x7F;
  wild[0x20] = 0x0E;
  FakeBrawl m;
  m.Join(0);
  ASSERT_EQ(ApplyNameTags(m, {{0, "cy", false, Profile(5, wild)}}), 1);
  const int slot = m.NameId(0);
  std::array<u8, 0x2D> want = wild;
  want[0] = FakeBrawl::CONTROLS[0];
  want[8] = FakeBrawl::CONTROLS[8];
  want[11] = 0xF0;
  want[0x1F] = 0xC3;
  want[0x2C] = 0x00;
  EXPECT_EQ(TagLayout(m, slot), want);
  EXPECT_EQ(m.Read8(FakeBrawl::Tag(slot) + 0x0C), 1);
}

TEST(OrcaUXNameTags, AProfileOfAnotherSizeIsNoneAndANamelessPlayerWithOneWearsItsPortLabel)
{
  FakeBrawl m;
  m.Join(1);
  ASSERT_EQ(ApplyNameTags(m, {{1, "\xe5\xb0\x8f", true, Profile(1, CustomLayout())}}), 1);
  EXPECT_EQ(m.TagName(m.NameId(1)), u"P2");
  EXPECT_EQ(TagLayout(m, m.NameId(1)), CustomLayout());
  std::vector<u8> short_profile = Profile(0, CustomLayout());
  short_profile.pop_back();
  FakeBrawl n;
  n.Join(0);
  n.Join(1);
  EXPECT_EQ(ApplyNameTags(n, {{0, "cy", false, short_profile}, {1, "", true, short_profile}}), 1);
  EXPECT_EQ(TagLayout(n, n.NameId(0)), FakeBrawl::CONTROLS);
  EXPECT_EQ(n.NameId(1), -1);
}

TEST(OrcaUXNameTags, OwnControlsFollowTheTagThePlayerWears)
{
  FakeBrawl m;
  std::u16string last;
  EXPECT_TRUE(ReadOwnControls(m, 0, "cy", &last).empty());
  m.SetTag(119, u"CY");
  for (u32 i = 0; i < 0x2D; ++i)
    m.Write8(FakeBrawl::Tag(119) + 0x14 + i, FakeBrawl::CONTROLS[i]);
  m.Write8(FakeBrawl::Tag(119) + 0x0C, 1);
  EXPECT_EQ(ReadOwnControls(m, 0, "cy", &last), Profile(1, FakeBrawl::CONTROLS));
  m.SetTag(4, u"MINE");
  for (u32 i = 0; i < 0x2D; ++i)
    m.Write8(FakeBrawl::Tag(4) + 0x14 + i, CustomLayout()[i]);
  m.Join(0);
  m.W32(FakeBrawl::Area(0) + 0x1C8, 4);
  EXPECT_EQ(ReadOwnControls(m, 0, "cy", &last), Profile(0, CustomLayout()));
  EXPECT_EQ(last, u"MINE");
  FakeBrawl two;
  std::u16string none;
  two.SetTag(4, u"MINE");
  two.Join(1);
  two.W32(FakeBrawl::Area(1) + 0x1C8, 4);
  EXPECT_TRUE(ReadOwnControls(two, 0, "", &none).empty());
  EXPECT_TRUE(none.empty());
  FakeBrawl melee("scMelee");
  melee.SetTag(4, u"MINE");
  for (u32 i = 0; i < 0x2D; ++i)
    melee.Write8(FakeBrawl::Tag(4) + 0x14 + i, CustomLayout()[i]);
  melee.SetTag(119, u"CY");
  EXPECT_EQ(ReadOwnControls(melee, 0, "cy", &last), Profile(0, CustomLayout()));
  melee.SetTag(4, u"");
  melee.recording = true;
  EXPECT_EQ(ReadOwnControls(melee, 0, "cy", &last)[0], 0);
  EXPECT_EQ(ReadOwnControls(melee, 0, "cy", &last).size(), 46u);
  EXPECT_TRUE(melee.written.empty());
}

TEST(OrcaUXNameTags, AProfileAsTextRoundTripsAndOnlyAsTheMenusSetIt)
{
  const std::vector<u8> profile = Profile(0, CustomLayout());
  const std::string hex = ControlsHex(profile);
  EXPECT_EQ(hex.size(), 92u);
  EXPECT_EQ(hex.substr(0, 6), "000303");
  EXPECT_EQ(ParseControlsHex(hex), profile);
  std::string upper = hex;
  for (char& c : upper)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  EXPECT_EQ(ParseControlsHex(upper), profile);
  EXPECT_TRUE(ControlsValid(profile));
  EXPECT_TRUE(ControlsValid(Profile(1, FakeBrawl::CONTROLS)));
  EXPECT_FALSE(ParseControlsHex(hex.substr(2)));
  EXPECT_FALSE(ParseControlsHex(hex + "00"));
  EXPECT_FALSE(ParseControlsHex(""));
  EXPECT_FALSE(ParseControlsHex("zz" + hex.substr(2)));
  EXPECT_FALSE(ParseControlsHex("02" + hex.substr(2)));
  std::vector<u8> bad = profile;
  bad[1] = 0x0F;
  EXPECT_FALSE(ControlsValid(bad));
  bad = profile;
  bad[1 + 11] = 0x08;
  EXPECT_FALSE(ControlsValid(bad));
  bad[1 + 11] = 0xF0;
  EXPECT_TRUE(ControlsValid(bad));
  bad[1 + 0x1F] = 0xC3;
  EXPECT_TRUE(ControlsValid(bad));
}

TEST(OrcaUXNameTags, OnlyThePlayersOwnChangesToTheirTagArePublished)
{
  const std::vector<u8> defaults = Profile(1, FakeBrawl::CONTROLS);
  const std::vector<u8> custom = Profile(0, CustomLayout());
  std::vector<u8> other = custom;
  other[1] = 0x04;
  {
    OwnControlsWatch w;
    EXPECT_FALSE(w.Next({}, u"CY", {}, false));
    EXPECT_FALSE(w.Next(defaults, u"CY", {}, true));
    EXPECT_FALSE(w.Next(defaults, u"CY", {}, true));
    EXPECT_EQ(w.Next(custom, u"CY", {}, false), custom);
    EXPECT_FALSE(w.Next(custom, u"CY", custom, false));
    EXPECT_EQ(w.Next(defaults, u"CY", custom, true), defaults);
  }
  {
    OwnControlsWatch w;
    EXPECT_FALSE(w.Next(custom, u"CY", custom, false));
    EXPECT_EQ(w.Next(other, u"MINE", custom, false), other);
    EXPECT_FALSE(w.Next({}, u"MINE", other, false));
    EXPECT_FALSE(w.Next(other, u"MINE", other, false));
  }
  {
    OwnControlsWatch w;
    EXPECT_FALSE(w.Next(custom, u"CY", custom, false));
    w.Rebase();
    EXPECT_FALSE(w.Next({}, u"CY", other, false));
    EXPECT_FALSE(w.Next(custom, u"CY", other, false));
    EXPECT_FALSE(w.Next(custom, u"CY", other, false));
    EXPECT_FALSE(w.Next(other, u"CY", other, false));
    w.Rebase();
    EXPECT_FALSE(w.Next(defaults, u"CY", other, true));
    EXPECT_EQ(w.Next(custom, u"CY", other, false), custom);
  }
}

TEST(OrcaUXNameTags, ASwitchToATagAtTheDefaultsIsNoChange)
{
  const std::vector<u8> defaults = Profile(1, FakeBrawl::CONTROLS);
  const std::vector<u8> custom = Profile(0, CustomLayout());
  std::vector<u8> other = custom;
  other[1] = 0x04;
  {
    OwnControlsWatch w;
    EXPECT_FALSE(w.Next(custom, u"ZEKE", custom, false));
    EXPECT_FALSE(w.Next(defaults, u"MYTAG", custom, true));
    EXPECT_FALSE(w.Next(defaults, u"MYTAG", custom, true));
    EXPECT_FALSE(w.Next(custom, u"MYTAG", custom, false));
    EXPECT_EQ(w.Next(other, u"MYTAG", custom, false), other);
    EXPECT_EQ(w.Next(defaults, u"MYTAG", other, true), defaults);
  }
  {
    OwnControlsWatch w;
    EXPECT_FALSE(w.Next(defaults, u"MYTAG", custom, true));
    OwnControlsWatch r;
    r.Rebase();
    EXPECT_FALSE(r.Next(defaults, u"MYTAG", custom, true));
    EXPECT_FALSE(r.Next(defaults, u"OTHER", custom, true));
    EXPECT_EQ(r.Next(other, u"MINE", custom, false), other);
  }
  {
    FakeBrawl m;
    GameMadeTag(m, 119, u"ZEKE");
    for (u32 i = 0; i < 0x2D; ++i)
      m.Write8(FakeBrawl::Tag(119) + 0x14 + i, custom[1 + i]);
    m.Write8(FakeBrawl::Tag(119) + 0x0C, 0);
    m.Join(0);
    Wear(m, 0, 119);
    const std::vector<Orca::Events::PortInfo> ports{{0, "zeke", false, custom}};
    OwnControlsReader reader;
    EXPECT_FALSE(reader.Read(m, ports, custom, 0));
    GameMadeTag(m, 118, u"MYTAG");
    Wear(m, 0, 118);
    EXPECT_FALSE(reader.Read(m, ports, custom, 0));
    for (u32 i = 0; i < 0x2D; ++i)
      m.Write8(FakeBrawl::Tag(118) + 0x14 + i, custom[1 + i]);
    m.Write8(FakeBrawl::Tag(118) + 0x0C, 0);
    EXPECT_FALSE(reader.Read(m, ports, custom, 0));
    m.SetTag(118, u"");
    GameMadeTag(m, 119, u"ZEKE");
    m.W32(FakeBrawl::SCENE, 0x806ff300);
    EXPECT_FALSE(reader.Read(m, ports, custom, 0));
  }
}

TEST(OrcaUXNameTags, AcrossRestartsATagMadeAgainKeepsTheKeptControls)
{
  const std::vector<u8> custom = Profile(0, CustomLayout());
  std::vector<u8> kept = custom;
  for (int run = 0; run < 2; ++run)
  {
    FakeBrawl m;
    m.Join(0);
    const std::vector<Orca::Events::PortInfo> ports{{0, "zeke", false, kept}};
    OwnControlsReader reader;
    std::vector<std::vector<u8>> published;
    const auto frame = [&] {
      if (std::optional<std::vector<u8>> changed = NameTagsFrame(m, ports, ports, &reader, kept, 0))
        published.push_back(*changed);
    };
    for (int f = 0; f < 3; ++f)
      frame();
    EXPECT_EQ(m.TagName(m.NameId(0)), u"ZEKE");
    GameMadeTag(m, 118, u"MYTAG");
    Wear(m, 0, 118);
    for (int f = 0; f < 3; ++f)
      frame();
    EXPECT_TRUE(published.empty()) << "run " << run;
    EXPECT_EQ(TagLayout(m, 118), FakeBrawl::CONTROLS) << "run " << run;
    EXPECT_EQ(m.Read8(FakeBrawl::Tag(118) + 0x0C), 1) << "run " << run;
    if (!published.empty())
      kept = published.back();
  }
  EXPECT_EQ(kept, custom);
}

TEST(OrcaUXNameTags, TheOwnControlsReadComesAfterTheWrites)
{
  const std::vector<u8> custom = Profile(0, CustomLayout());
  const std::vector<Orca::Events::PortInfo> ports{{0, "zeke", false, custom}};
  {
    FakeBrawl m;
    m.Join(0);
    OwnControlsReader reader;
    EXPECT_FALSE(NameTagsFrame(m, ports, ports, &reader, custom, 0));
    EXPECT_EQ(m.TagName(0), u"ZEKE");
    EXPECT_EQ(TagLayout(m, 0), CustomLayout());
    GameMadeTag(m, 118, u"MYTAG");
    Wear(m, 0, 118);
    EXPECT_FALSE(NameTagsFrame(m, ports, ports, &reader, custom, 0));
    EXPECT_EQ(TagLayout(m, 118), FakeBrawl::CONTROLS);
    EXPECT_FALSE(NameTagsFrame(m, ports, ports, &reader, custom, 0));
    m.Write8(FakeBrawl::Tag(118) + 0x14, 0x04);
    const std::optional<std::vector<u8>> edited =
        NameTagsFrame(m, ports, ports, &reader, custom, 0);
    ASSERT_TRUE(edited);
    EXPECT_EQ((*edited)[1], 0x04);
  }
  {
    FakeBrawl m;
    m.Join(0);
    OwnControlsReader reader;
    EXPECT_FALSE(NameTagsFrame(m, ports, ports, &reader, custom, 0));
    GameMadeTag(m, 0, u"ZEKE");
    EXPECT_EQ(NameTagsFrame(m, ports, ports, &reader, custom, 0), Profile(1, FakeBrawl::CONTROLS));
    EXPECT_EQ(TagLayout(m, 0), FakeBrawl::CONTROLS);
    FakeBrawl old;
    old.Join(0);
    OwnControlsReader before;
    EXPECT_FALSE(NameTagsFrame(old, ports, ports, &before, custom, 0));
    GameMadeTag(old, 0, u"ZEKE");
    EXPECT_EQ(before.Read(old, ports, custom, 0), Profile(1, FakeBrawl::CONTROLS));
  }
  {
    FakeBrawl m;
    m.Join(0);
    EXPECT_FALSE(NameTagsFrame(m, ports, ports, nullptr, custom, 0));
    EXPECT_EQ(TagLayout(m, 0), CustomLayout());
  }
  {
    std::vector<u8> old = custom;
    old[1] = 0x04;
    FakeBrawl m;
    GameMadeTag(m, 119, u"ZEKE");
    for (u32 i = 0; i < 0x2D; ++i)
      m.Write8(FakeBrawl::Tag(119) + 0x14 + i, old[1 + i]);
    m.Write8(FakeBrawl::Tag(119) + 0x0C, 0);
    m.Join(0);
    Wear(m, 0, 119);
    OwnControlsReader reader;
    reader.Rebase();
    EXPECT_FALSE(reader.Read(m, {{0, "zeke", true, custom}}, custom, 0));
    EXPECT_FALSE(reader.Read(m, ports, custom, 0));
    EXPECT_FALSE(reader.Read(m, ports, custom, 0));
  }
}

namespace
{
std::u16string Typed(std::u16string_view ascii)
{
  std::u16string typed;
  for (const char16_t c : ascii)
    typed.push_back(static_cast<char16_t>(c + 0xFEE0));
  return typed;
}
void SavedTapJumpOff(FakeBrawl& m, int slot)
{
  m.Write8(FakeBrawl::Tag(slot) + 0x14 + 11, 0x01);
}
}

TEST(OrcaUXNameTags, ATagThePlayerTypedInTheGameIsTheirs)
{
  {
    FakeBrawl m;
    GameMadeTag(m, 0, Typed(u"SANDB"));
    SavedTapJumpOff(m, 0);
    const std::array<u8, 0x2D> saved = TagLayout(m, 0);
    m.Join(0);
    EXPECT_EQ(ApplyNameTags(m, {{0, "sandbox-ada", false, {}}}), 1);
    EXPECT_EQ(m.NameId(0), 0);
    EXPECT_EQ(TagLayout(m, 0), saved);
    EXPECT_EQ(m.TagName(119), u"");
    FakeBrawl a, b;
    for (FakeBrawl* x : {&a, &b})
    {
      GameMadeTag(*x, 0, Typed(u"sandb"));
      x->Join(0);
      x->Join(1);
    }
    ApplyNameTags(a, {{0, "sandbox-ada", false}, {1, "sandbox-bo", true}});
    ApplyNameTags(b, {{0, "sandbox-ada", true}, {1, "sandbox-bo", false}});
    EXPECT_EQ(a.bytes, b.bytes);
    EXPECT_EQ(a.NameId(0), 0);
    EXPECT_EQ(a.TagName(a.NameId(1)), u"SAND2");
  }
  {
    FakeBrawl m;
    GameMadeTag(m, 0, Typed(u"SAND"));
    GameMadeTag(m, 1, Typed(u"SANDC"));
    m.Join(0);
    EXPECT_EQ(ApplyNameTags(m, {{0, "sandbox-ada", false}}), 1);
    EXPECT_EQ(m.NameId(0), 2);
    EXPECT_EQ(m.TagName(2), u"SANDB");
  }
  {
    const std::vector<u8> custom = Profile(0, CustomLayout());
    FakeBrawl m;
    GameMadeTag(m, 0, Typed(u"SANDB"));
    m.Write8(FakeBrawl::Tag(0) + 0x14 + 11, 0x81);
    m.Join(0);
    EXPECT_EQ(ApplyNameTags(m, {{0, "sandbox-ada", false, custom}}), 1);
    EXPECT_EQ(m.NameId(0), 0);
    std::array<u8, 0x2D> want = CustomLayout();
    want[11] = 0x01;
    EXPECT_EQ(TagLayout(m, 0), want);
    EXPECT_EQ(m.Read8(FakeBrawl::Tag(0) + 0x0C), 0);
    EXPECT_EQ(m.TagName(119), u"");
    m.W32(FakeBrawl::Area(0) + 0x1C8, 0xFFFFFFFF);
    const int writes = m.writes;
    ApplyNameTags(m, {{0, "sandbox-ada", false, custom}});
    EXPECT_EQ(m.NameId(0), 0);
    EXPECT_EQ(m.writes, writes + 2);
  }
}

TEST(OrcaUXNameTags, ControlsSavedInTheGamesMenusArePublishedThere)
{
  const std::vector<Orca::Events::PortInfo> ports{{0, "sandbox-ada", false, {}}};
  {
    FakeBrawl m("muMenuMain");
    OwnControlsReader reader;
    EXPECT_FALSE(NameTagsFrame(m, ports, ports, &reader, {}, 0));
    GameMadeTag(m, 0, Typed(u"SANDB"));
    EXPECT_FALSE(NameTagsFrame(m, ports, ports, &reader, {}, 0));
    SavedTapJumpOff(m, 0);
    const std::optional<std::vector<u8>> saved = NameTagsFrame(m, ports, ports, &reader, {}, 0);
    ASSERT_TRUE(saved);
    EXPECT_EQ((*saved)[1 + 11], 0x00);
    EXPECT_TRUE(ControlsValid(*saved));
    EXPECT_FALSE(NameTagsFrame(m, ports, ports, &reader, *saved, 0));
  }
  {
    const std::vector<u8> custom = Profile(0, CustomLayout());
    FakeBrawl m;
    m.Join(0);
    OwnControlsReader reader;
    const std::vector<Orca::Events::PortInfo> carried{{0, "sandbox-ada", false, custom}};
    EXPECT_FALSE(NameTagsFrame(m, carried, carried, &reader, custom, 0));
    ASSERT_EQ(m.TagName(m.NameId(0)), u"SANDB");
    const std::array<u8, 0x2D> orcas = TagLayout(m, 119);
    m.W32(FakeBrawl::SCENE, 0x806ff300);
    GameMadeTag(m, 0, Typed(u"SANDB"));
    EXPECT_FALSE(NameTagsFrame(m, carried, carried, &reader, custom, 0));
    SavedTapJumpOff(m, 0);
    const std::optional<std::vector<u8>> edited =
        NameTagsFrame(m, carried, carried, &reader, custom, 0);
    ASSERT_TRUE(edited);
    std::array<u8, 0x2D> tap_jump_off = FakeBrawl::CONTROLS;
    tap_jump_off[11] = 0x00;
    EXPECT_EQ(*edited, Profile(1, tap_jump_off));
    m.W32(FakeBrawl::SCENE, FakeBrawl::NAME);
    m.W32(FakeBrawl::Area(0) + 0x1C8, 0xFFFFFFFF);
    const std::vector<Orca::Events::PortInfo> now{{0, "sandbox-ada", false, *edited}};
    EXPECT_FALSE(NameTagsFrame(m, now, now, &reader, *edited, 0));
    EXPECT_EQ(m.NameId(0), 0);
    EXPECT_EQ(m.TagName(0), Typed(u"SANDB"));
    EXPECT_EQ(m.Read8(FakeBrawl::Tag(0) + 0x14 + 11), 0x01);
    EXPECT_EQ(TagLayout(m, 119), orcas);
  }
}

TEST(OrcaUXNameTags, InARoomATagSavedInTheGamesMenusKeepsWhatWasSaved)
{
  std::array<u8, 0x2D> cy_layout = FakeBrawl::CONTROLS;
  cy_layout[3] = 0x02;
  const std::vector<u8> cy = Profile(1, cy_layout);
  const std::vector<u8> ada = Profile(0, CustomLayout());
  const auto room = [&](bool ada_remote) {
    return std::vector<Orca::Events::PortInfo>{{0, "cy", ada_remote ? false : true, cy},
                                               {1, "ada", ada_remote, ada}};
  };
  const auto saved_in_room = [](FakeBrawl& m, int slot) {
    m.Write8(FakeBrawl::Tag(slot) + 0x0C, 1);
    m.Write8(FakeBrawl::Tag(slot) + 0x14 + 2, 0x02);
    m.Write8(FakeBrawl::Tag(slot) + 0x14 + 11, 0x81);
  };
  const auto next_character_select = [](FakeBrawl& m) {
    m.W32(FakeBrawl::Area(0) + 0x1C8, 0xFFFFFFFF);
    m.W32(FakeBrawl::Area(1) + 0x1C8, 0xFFFFFFFF);
  };
  std::array<FakeBrawl, 2> machines;
  for (size_t i = 0; i < machines.size(); ++i)
  {
    FakeBrawl& m = machines[i];
    m.Join(0);
    m.Join(1);
    ASSERT_EQ(ApplyNameTags(m, room(i == 0)), 2);
    const int slot = m.NameId(1);
    ASSERT_EQ(m.TagName(slot), u"ADA");
    EXPECT_EQ(TagLayout(m, slot), CustomLayout());
    saved_in_room(m, slot);
    std::array<u8, 0x2D> edited = CustomLayout();
    edited[2] = 0x02;
    edited[11] = 0x81;
    ASSERT_EQ(TagLayout(m, slot), edited);
    next_character_select(m);
    EXPECT_EQ(ApplyNameTags(m, room(i == 0)), 2);
    EXPECT_EQ(m.NameId(1), slot);
    EXPECT_EQ(TagLayout(m, slot), edited);
    EXPECT_EQ(m.Read8(FakeBrawl::Tag(slot) + 0x0C), 1);
    next_character_select(m);
    ApplyNameTags(m, room(i == 0));
    EXPECT_EQ(TagLayout(m, slot), edited);
    EXPECT_EQ(TagLayout(m, m.NameId(0)), cy_layout);
  }
  EXPECT_EQ(machines[0].bytes, machines[1].bytes);
  {
    FakeBrawl m;
    m.Join(1);
    const std::vector<Orca::Events::PortInfo> solo{{1, "ada", false, ada}};
    ASSERT_EQ(ApplyNameTags(m, solo), 1);
    const int slot = m.NameId(1);
    saved_in_room(m, slot);
    m.W32(FakeBrawl::Area(1) + 0x1C8, 0xFFFFFFFF);
    ApplyNameTags(m, solo);
    std::array<u8, 0x2D> written = CustomLayout();
    written[11] = 0x01;
    EXPECT_EQ(TagLayout(m, slot), written);
    EXPECT_EQ(m.Read8(FakeBrawl::Tag(slot) + 0x0C), 0);
  }
  {
    FakeBrawl m;
    GameMadeTag(m, 0, u"ADA");
    m.Write8(FakeBrawl::Tag(0) + 0x14 + 6, 0x03);
    m.Join(0);
    m.Join(1);
    ApplyNameTags(m, room(true));
    EXPECT_EQ(m.NameId(1), 0);
    EXPECT_EQ(TagLayout(m, 0), CustomLayout());
  }
  {
    const std::vector<u8> bo = Profile(1, cy_layout);
    const std::vector<Orca::Events::PortInfo> pair{{0, "sandbox-ada", false, ada},
                                                   {1, "sandbox-bo", true, bo}};
    FakeBrawl m;
    m.Join(0);
    m.Join(1);
    ASSERT_EQ(ApplyNameTags(m, pair), 2);
    const int sandb = m.NameId(0);
    ASSERT_EQ(m.TagName(sandb), u"SANDB");
    saved_in_room(m, sandb);
    next_character_select(m);
    m.Join(1);
    m.W32(FakeBrawl::Area(0) + 0x1B4, 0);
    ApplyNameTags(m, pair);
    ASSERT_EQ(m.NameId(1), sandb);
    std::array<u8, 0x2D> bo_in_saved = cy_layout;
    bo_in_saved[11] |= 0x01;
    EXPECT_EQ(TagLayout(m, sandb), bo_in_saved);
    m.Join(0);
    ApplyNameTags(m, pair);
    ASSERT_EQ(m.TagName(m.NameId(0)), u"SAND1");
    EXPECT_EQ(TagLayout(m, m.NameId(0)), CustomLayout());
  }
}

namespace
{
void OnScene(FakeBrawl& m, const char* name)
{
  constexpr u32 at = 0x806ff400;
  for (size_t i = 0; i <= std::strlen(name); ++i)
    m.bytes[at + static_cast<u32>(i)] = static_cast<u8>(name[i]);
  m.W32(FakeBrawl::SCENE, at);
}
void SavedCustom(FakeBrawl& m, int slot)
{
  for (u32 i = 0; i < 0x2D; ++i)
    m.Write8(FakeBrawl::Tag(slot) + 0x14 + i, CustomLayout()[i]);
  m.Write8(FakeBrawl::Tag(slot) + 0x14 + 11, 0x01);
}
}

TEST(OrcaUXNameTags, RumbleWiiRemoteAndClassicEditsDoNotNeedTheGameCubeSaveBitToBeRead)
{
  struct Edit
  {
    const char* name;
    u32 offset;
    size_t profile_byte;
    u8 value;
  };
  for (const Edit edit : {Edit{"rumble", 0x0C, 0, 0},
                          Edit{"Wii Remote", 0x14 + 0x0C, 1 + 0x0C, 0x02},
                          Edit{"Classic", 0x14 + 0x20, 1 + 0x20, 0x02}})
  {
    SCOPED_TRACE(edit.name);
    for (const bool css : {false, true})
    {
      SCOPED_TRACE(css ? "worn CSS tag" : "Options Controls page");
      for (const bool saved_bit : {false, true})
      {
        SCOPED_TRACE(saved_bit);
        FakeBrawl m(css ? "scSelctCharacter" : "scButton");
        GameMadeTag(m, 0, Typed(u"MINE"));
        if (css)
        {
          m.Join(0);
          Wear(m, 0, 0);
        }
        const std::vector<u8> own = Profile(1, FakeBrawl::CONTROLS);
        const std::vector<Orca::Events::PortInfo> ports{{0, "ada", false, own}};
        OwnControlsReader reader;
        EXPECT_FALSE(reader.Read(m, ports, own, 0));
        m.Write8(FakeBrawl::Tag(0) + edit.offset, edit.value);
        if (saved_bit)
          m.Write8(FakeBrawl::Tag(0) + 0x14 + 11, 0x81);
        std::vector<u8> want = own;
        want[edit.profile_byte] = edit.value;
        m.written.clear();
        m.recording = true;
        const auto changed = reader.Read(m, ports, own, 0);
        ASSERT_TRUE(changed);
        EXPECT_EQ(*changed, want);
        EXPECT_TRUE(ControlsValid(*changed));
        EXPECT_FALSE(reader.Read(m, ports, want, 0));
        EXPECT_TRUE(m.written.empty());
      }
    }
  }
}

TEST(OrcaUXNameTags, RumbleWiiRemoteAndClassicRoomEditsKeepOnlyTagsWithTheSaveBit)
{
  for (const u32 offset : {0x0Cu, 0x14u + 0x0C, 0x14u + 0x20})
  {
    SCOPED_TRACE(offset);
    for (const bool saved_bit : {false, true})
    {
      SCOPED_TRACE(saved_bit);
      std::array<FakeBrawl, 2> machines;
      for (size_t i = 0; i < machines.size(); ++i)
      {
        FakeBrawl& m = machines[i];
        const auto own = Profile(1, FakeBrawl::CONTROLS);
        const std::vector<Orca::Events::PortInfo> ports{{0, "ada", i == 0, own},
                                                       {1, "cy", i != 0, own}};
        m.Join(0);
        m.Join(1);
        ASSERT_EQ(ApplyNameTags(m, ports), 2);
        const int slot = m.NameId(0);
        m.Write8(FakeBrawl::Tag(slot) + offset, offset == 0x0C ? 0 : 0x02);
        if (saved_bit)
          m.Write8(FakeBrawl::Tag(slot) + 0x14 + 11, 0x81);
        const auto edited_layout = TagLayout(m, slot);
        const u8 edited_rumble = m.Read8(FakeBrawl::Tag(slot) + 0x0C);
        Wear(m, 0, -1);
        Wear(m, 1, -1);
        ASSERT_EQ(ApplyNameTags(m, ports), 2);
        EXPECT_EQ(m.NameId(0), slot);
        EXPECT_EQ(TagLayout(m, slot), saved_bit ? edited_layout : FakeBrawl::CONTROLS);
        EXPECT_EQ(m.Read8(FakeBrawl::Tag(slot) + 0x0C), saved_bit ? edited_rumble : 1);
      }
      EXPECT_EQ(machines[0].bytes, machines[1].bytes);
    }
  }
}

TEST(OrcaUXNameTags, ATemplateWithTheSaveBitDoesNotMakeNewRoomTagsReadAsEdited)
{
  FakeBrawl m;
  m.Write8(0x80406938 + 11, 0x81);
  const auto custom = Profile(0, CustomLayout());
  const std::vector<Orca::Events::PortInfo> ports{{0, "ada", false, custom},
                                                 {1, "cy", true, custom}};
  m.Join(0);
  m.Join(1);
  ASSERT_EQ(ApplyNameTags(m, ports), 2);
  auto want = CustomLayout();
  want[11] |= 0x01;
  for (const int port : {0, 1})
  {
    EXPECT_EQ(TagLayout(m, m.NameId(port)), want);
    EXPECT_EQ(m.Read8(FakeBrawl::Tag(m.NameId(port)) + 0x0C), 0);
  }
}

TEST(OrcaUXNameTags, ButtonsSetOnTheControlsPageAreThePlayersOwnWhateverTheTagIsCalled)
{
  const std::vector<Orca::Events::PortInfo> ports{{0, "sandbox-ada", false, {}}};
  {
    FakeBrawl m;
    m.Join(0);
    OwnControlsReader reader;
    EXPECT_FALSE(NameTagsFrame(m, ports, ports, &reader, {}, 0));
    ASSERT_EQ(m.TagName(m.NameId(0)), u"SANDB");
    ASSERT_EQ(m.NameId(0), 0);
    OnScene(m, "muMenuMain");
    GameMadeTag(m, 1, Typed(u"MINE"));
    EXPECT_FALSE(NameTagsFrame(m, ports, ports, &reader, {}, 0));
    OnScene(m, "scButton");
    EXPECT_FALSE(NameTagsFrame(m, ports, ports, &reader, {}, 0));
    m.Write8(FakeBrawl::Tag(1) + 0x14 + 11, 0x81);
    EXPECT_FALSE(NameTagsFrame(m, ports, ports, &reader, {}, 0));
    SavedCustom(m, 1);
    const std::optional<std::vector<u8>> edited = NameTagsFrame(m, ports, ports, &reader, {}, 0);
    ASSERT_TRUE(edited);
    EXPECT_EQ(*edited, Profile(1, CustomLayout()));
    EXPECT_TRUE(ControlsValid(*edited));
    OnScene(m, "muMenuMain");
    EXPECT_FALSE(NameTagsFrame(m, ports, ports, &reader, *edited, 0));
    EXPECT_FALSE(NameTagsFrame(m, ports, ports, &reader, *edited, 0));
    m.W32(FakeBrawl::SCENE, FakeBrawl::NAME);
    m.W32(FakeBrawl::Area(0) + 0x1C8, 0xFFFFFFFF);
    const std::vector<Orca::Events::PortInfo> now{{0, "sandbox-ada", false, *edited}};
    EXPECT_FALSE(NameTagsFrame(m, now, now, &reader, *edited, 0));
    EXPECT_EQ(m.NameId(0), 0);
    EXPECT_EQ(TagLayout(m, 0), CustomLayout());
    EXPECT_EQ(m.Read8(FakeBrawl::Tag(1) + 0x14 + 11), 0x01);
    OnScene(m, "muMenuMain");
    EXPECT_FALSE(NameTagsFrame(m, now, now, &reader, *edited, 0));
  }
  {
    FakeBrawl m("scButton");
    GameMadeTag(m, 0, Typed(u"SANDB"));
    GameMadeTag(m, 1, Typed(u"OLD"));
    SavedCustom(m, 1);
    OwnControlsReader reader;
    const auto read = [&] {
      m.written.clear();
      m.recording = true;
      std::optional<std::vector<u8>> got = reader.Read(m, ports, {}, 0);
      m.recording = false;
      EXPECT_TRUE(m.written.empty());
      return got;
    };
    EXPECT_FALSE(read());
    m.SetTag(1, u"");
    GameMadeTag(m, 2, Typed(u"NEW"));
    SavedCustom(m, 2);
    EXPECT_FALSE(read());
    m.SetTag(2, Typed(u"NEWER"));
    m.Write8(FakeBrawl::Tag(2) + 0x14 + 2, 0x02);
    EXPECT_FALSE(read());
    m.Write8(FakeBrawl::Tag(2) + 0x14 + 3, 0x02);
    const std::optional<std::vector<u8>> edited = read();
    ASSERT_TRUE(edited);
    EXPECT_EQ((*edited)[1 + 3], 0x02);
  }
  {
    FakeBrawl m("scButton");
    GameMadeTag(m, 0, Typed(u"SANDB"));
    GameMadeTag(m, 5, Typed(u"MINE"));
    OwnControlsReader reader;
    EXPECT_FALSE(reader.Read(m, ports, {}, 0));
    OnScene(m, "muMenuMain");
    SavedCustom(m, 5);
    const std::optional<std::vector<u8>> edited = reader.Read(m, ports, {}, 0);
    ASSERT_TRUE(edited);
    EXPECT_EQ(*edited, Profile(1, CustomLayout()));
    m.Write8(FakeBrawl::Tag(0) + 0x14 + 2, 0x02);
    EXPECT_FALSE(reader.Read(m, ports, *edited, 0));
    EXPECT_FALSE(reader.Read(m, ports, *edited, 0));
    m.Write8(FakeBrawl::Tag(5) + 0x14 + 4, 0x02);
    EXPECT_FALSE(reader.Read(m, ports, *edited, 1));
  }
  for (const bool saved_bit : {false, true})
  {
    SCOPED_TRACE(saved_bit);
    FakeBrawl m("scButton");
    GameMadeTag(m, 0, Typed(u"SANDB"));
    SavedCustom(m, 0);
    GameMadeTag(m, 1, Typed(u"MINE"));
    SavedCustom(m, 1);
    const std::vector<u8> own = Profile(1, CustomLayout());
    OwnControlsReader reader;
    EXPECT_FALSE(reader.Read(m, ports, own, 0));
    for (u32 i = 0; i < 0x2D; ++i)
      m.Write8(FakeBrawl::Tag(1) + 0x14 + i, FakeBrawl::CONTROLS[i]);
    if (saved_bit)
      m.Write8(FakeBrawl::Tag(1) + 0x14 + 11, 0x81);
    const std::optional<std::vector<u8>> edited = reader.Read(m, ports, own, 0);
    ASSERT_TRUE(edited);
    EXPECT_EQ(*edited, Profile(1, FakeBrawl::CONTROLS));
  }
}

TEST(OrcaUXNameTags, UnmappedPointersWriteNothing)
{
  {
    FakeBrawl m;
    m.Join(0);
    m.W32(FakeBrawl::TASK + 0x44, 0x83000000);
    m.recording = true;
    EXPECT_EQ(ApplyNameTags(m, {{0, "cy", false}}), 0);
    EXPECT_TRUE(m.written.empty());
  }
  {
    FakeBrawl m;
    m.Join(0);
    m.W32(FakeBrawl::SCENE + 0x400, 0x83000000);
    EXPECT_EQ(ApplyNameTags(m, {{0, "cy", false}}), 0);
  }
  {
    FakeBrawl m;
    m.Join(0);
    m.bytes.erase(FakeBrawl::END - 1);
    EXPECT_EQ(ApplyNameTags(m, {{0, "cy", false}}), 0);
  }
}

TEST(OrcaUXNameTags, NothingOutsideTheCharacterSelectOrWhenItDoesNotLookRight)
{
  {
    FakeBrawl m("scMelee");
    m.Join(0);
    EXPECT_EQ(ApplyNameTags(m, {{0, "cy", false}}), 0);
  }
  {
    FakeBrawl m;
    m.Join(0);
    m.W32(FakeBrawl::Area(0) + 0x1B0, 2);
    EXPECT_EQ(ApplyNameTags(m, {{0, "cy", false}}), 0);
    EXPECT_EQ(m.NameId(0), -1);
  }
  {
    FakeBrawl m;
    m.Join(0);
    EXPECT_EQ(ApplyNameTags(m, {{0, "\xe2\x98\x83", false}, {5, "ada", true}}), 0);
  }
  {
    FakeBrawl m;
    for (int i = 0; i < 120; ++i)
      m.SetTag(i, u"T");
    m.Join(0);
    EXPECT_EQ(ApplyNameTags(m, {{0, "cy", false}}), 0);
  }
}

#ifndef _WIN32
namespace
{
class FakeBridge
{
public:
  explicit FakeBridge(int status = 200) : m_status(status)
  {
    m_listen = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    bind(m_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    listen(m_listen, 4);
    socklen_t len = sizeof(addr);
    getsockname(m_listen, reinterpret_cast<sockaddr*>(&addr), &len);
    m_port = ntohs(addr.sin_port);
    m_thread = std::thread([this] { Serve(); });
  }
  ~FakeBridge()
  {
    Close();
    if (m_thread.joinable())
      m_thread.join();
  }
  std::string Url() const { return fmt::format("http://127.0.0.1:{}", m_port); }
  bool WaitConnected()
  {
    for (int i = 0; i < 200 && m_client < 0; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return m_client >= 0;
  }
  void Send(const std::string& json) { SendRaw("data: " + json + "\n\n"); }
  void SendRaw(const std::string& bytes)
  {
    if (m_client >= 0)
      (void)!write(m_client, bytes.data(), bytes.size());
  }
  void Close()
  {
    if (m_listen >= 0)
    {
      shutdown(m_listen, SHUT_RDWR);
      close(m_listen);
      m_listen = -1;
    }
    if (const int fd = m_client.exchange(-1); fd >= 0)
    {
      shutdown(fd, SHUT_RDWR);
      close(fd);
    }
  }
  std::string Request()
  {
    std::lock_guard lk(m_request_lock);
    return request;
  }
  std::string request;
  std::atomic<int> connections{0};

private:
  void Serve()
  {
    for (;;)
    {
      const int fd = accept(m_listen, nullptr, nullptr);
      if (fd < 0)
        return;
      ++connections;
      char buf[4096];
      const ssize_t n = read(fd, buf, sizeof(buf));
      {
        std::lock_guard lk(m_request_lock);
        request.assign(buf, n > 0 ? size_t(n) : 0);
      }
      const std::string head =
          m_status == 200 ?
              "HTTP/1.1 200 OK\r\ncontent-type: text/event-stream\r\ncache-control: no-store\r\n\r\n" :
              fmt::format(
                  "HTTP/1.1 {} No\r\ncontent-type: application/json\r\ncontent-length: 2\r\n\r\n{{}}",
                  m_status);
      (void)!write(fd, head.data(), head.size());
      if (m_status != 200)
      {
        close(fd);
        continue;
      }
      if (m_client >= 0)
        close(fd);
      else
        m_client = fd;
    }
  }
  std::mutex m_request_lock;
  int m_status;
  int m_listen = -1;
  std::atomic<int> m_client{-1};
  int m_port = 0;
  std::thread m_thread;
};

template <typename F>
bool Eventually(F f, int ms = 2000)
{
  for (int waited = 0; waited < ms; waited += 5)
  {
    if (f())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return f();
}
}

TEST(OrcaUXControllerStream, CachesTheNewestSnapshotAndGoesNeutralThenFallsBack)
{
  FakeBridge bridge;
  StartControllerStream(bridge.Url(), "t0ken");
  ASSERT_TRUE(bridge.WaitConnected());
  EXPECT_NE(bridge.Request().find("GET /controllers/events "), std::string::npos);
  EXPECT_NE(bridge.Request().find("Authorization: Bearer t0ken"), std::string::npos);
  EXPECT_FALSE(LatestPad().has_value());

  const auto now_ms = [] {
    return std::chrono::duration<double, std::milli>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  };
  const auto send = [&](int buttons, int stick_x) {
    const double t = now_ms();
    bridge.Send(Event(true, FourPorts(PortJson(0, 0, "wired", buttons, {stick_x, 128, 128, 128})),
                      "", t, t));
  };
  const auto pad_is = [](u16 buttons, u8 stick_x) {
    return [buttons, stick_x] {
      const auto p = LatestPad();
      return p && p->button == buttons && p->stickX == stick_x;
    };
  };
  send(0x01, 170);
  ASSERT_TRUE(Eventually(pad_is(PAD_BUTTON_A, 170)));
  send(0x02, 100);
  send(0x04, 90);
  ASSERT_TRUE(Eventually(pad_is(PAD_BUTTON_X, 90)));
  bridge.Send(R"({"type":"controllers","snapshot":{"schema":9}})");
  ASSERT_TRUE(Eventually([] { return GetControllerTiming().refused >= 1; }));
  send(0x04, 90);
  ASSERT_TRUE(Eventually(pad_is(PAD_BUTTON_X, 90)));
  EXPECT_TRUE(GetControllerTiming().attached);
  EXPECT_GE(GetControllerTiming().samples, 2u);
  {
    const double t = now_ms();
    std::string restarted = Event(true, FourPorts(PortJson(0, 0, "wired", 0x08, {60, 128, 128, 128})),
                                  "", t, t);
    const std::string from = R"("session":"0b6f","sequence":7)";
    restarted.replace(restarted.find(from), from.size(), R"("session":"other","sequence":1)");
    bridge.Send(restarted);
  }
  ASSERT_TRUE(Eventually(pad_is(PAD_BUTTON_Y, 60)));

  ASSERT_TRUE(Eventually(pad_is(0, 128), 600));

  bridge.Close();
  ASSERT_TRUE(Eventually([] { return !GetControllerTiming().attached; }));
  ASSERT_TRUE(Eventually([] { return !LatestPad().has_value(); }, 2500));
  StopControllerStream();
}

TEST(OrcaUXControllerStream, CrlfCommentsAndMultiLineData)
{
  FakeBridge bridge;
  StartControllerStream(bridge.Url(), "t0ken");
  ASSERT_TRUE(bridge.WaitConnected());
  const auto now_ms = [] {
    return std::chrono::duration<double, std::milli>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  };
  const auto event = [&](int buttons) {
    const double t = now_ms();
    return Event(true, FourPorts(PortJson(0, 0, "wired", buttons)), "", t, t);
  };
  const auto pad_is = [](u16 buttons) {
    return [buttons] {
      const auto p = LatestPad();
      return p && p->button == buttons;
    };
  };
  bridge.SendRaw(": ping\n\n: ping\r\n\r\n");
  bridge.SendRaw("data: " + event(0x01) + "\r\n\r\n");
  ASSERT_TRUE(Eventually(pad_is(PAD_BUTTON_A)));
  const std::string json = event(0x02);
  const size_t half = json.find(",\"pads\"");
  bridge.SendRaw(": ping\ndata: " + json.substr(0, half) + "\ndata: " + json.substr(half) +
                 "\n\n");
  ASSERT_TRUE(Eventually(pad_is(PAD_BUTTON_B)));
  bridge.SendRaw("data: " + event(0x04) + "\r\r");
  ASSERT_TRUE(Eventually(pad_is(PAD_BUTTON_X)));
  EXPECT_EQ(GetControllerTiming().refused, 0u);
  std::string pings;
  for (int i = 0; i < 40000; ++i)
    pings += ": ping\n\n";
  bridge.SendRaw(pings);
  bridge.SendRaw("data: " + event(0x08) + "\n\n");
  ASSERT_TRUE(Eventually(pad_is(PAD_BUTTON_Y)));
  EXPECT_TRUE(GetControllerTiming().attached);
  StopControllerStream();
}

TEST(OrcaUXControllerStream, RefusedTokenStopsAndNonLoopbackNeverConnects)
{
  {
    FakeBridge bridge(401);
    StartControllerStream(bridge.Url(), "bad");
    ASSERT_TRUE(Eventually([&] { return bridge.connections.load() == 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    EXPECT_EQ(bridge.connections.load(), 1);
    EXPECT_FALSE(LatestPad().has_value());
    StopControllerStream();
  }
  StartControllerStream("http://example.com:80", "t0ken");
  EXPECT_FALSE(GetControllerTiming().attached);
  EXPECT_FALSE(LatestPad().has_value());
  StopControllerStream();
}

TEST(OrcaUXControllerStream, InputAgeAtFrameReads)
{
  FakeBridge bridge;
  StartControllerStream(bridge.Url(), "t0ken");
  ASSERT_TRUE(bridge.WaitConnected());
  const auto now_ms = [] {
    return std::chrono::duration<double, std::milli>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  };
  const auto send = [&](int buttons) {
    const double t = now_ms();
    bridge.Send(Event(true, FourPorts(PortJson(0, 0, "wired", buttons)), "", t, t - 3));
  };
  const auto pad_is = [](u16 buttons) {
    return [buttons] {
      const auto p = LatestPad();
      return p && p->button == buttons;
    };
  };
  send(0x01);
  ASSERT_TRUE(Eventually(pad_is(PAD_BUTTON_A)));
  EXPECT_TRUE(LocalPad(true).has_value());
  EXPECT_TRUE(LocalPad(true).has_value());
  EXPECT_TRUE(LocalPad(false).has_value());
  ControllerTiming t = GetControllerTiming();
  EXPECT_EQ(t.input.frames, 2u);
  EXPECT_EQ(t.input.reports, 1u);
  EXPECT_EQ(t.input.changes, 0u);
  EXPECT_EQ(t.age_samples, 0u);

  send(0x02);
  ASSERT_TRUE(Eventually(pad_is(PAD_BUTTON_B)));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  LocalPad(true);
  t = GetControllerTiming();
  EXPECT_EQ(t.input.frames, 3u);
  EXPECT_EQ(t.input.reports, 2u);
  EXPECT_EQ(t.input.changes, 1u);
  EXPECT_EQ(t.age_samples, 1u);
  EXPECT_NEAR(t.helper_p50_ms, 3.0, 0.01);
  EXPECT_GE(t.cache_p50_ms, 20.0);
  EXPECT_LT(t.cache_p50_ms, 250.0);
  EXPECT_NEAR(t.age_p50_ms, t.helper_p50_ms + t.cache_p50_ms, 0.01);
  EXPECT_NEAR(t.input.age_ms, t.age_p50_ms, 0.01);
  EXPECT_EQ(t.hop_samples, 1u);
  EXPECT_GE(t.hop_p50_ms, 0.0);
  EXPECT_LT(t.hop_p50_ms, 100.0);
  EXPECT_EQ(GetInputAge().changes, 1u);

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  LocalPad(true);
  EXPECT_EQ(GetInputAge().frames, 3u);
  send(0x04);
  ASSERT_TRUE(Eventually(pad_is(PAD_BUTTON_X)));
  LocalPad(true);
  EXPECT_EQ(GetInputAge().frames, 4u);
  EXPECT_EQ(GetInputAge().changes, 1u);

  {
    const double now = now_ms();
    bridge.Send(Event(false, "", PadJson(0, "standard", "[0,0,0,0]", Buttons17(0)), now, now));
  }
  ASSERT_TRUE(Eventually([] {
    const auto p = LatestPad();
    return p && p->button == PAD_BUTTON_A;
  }));
  LocalPad(true);
  EXPECT_EQ(GetInputAge().frames, 4u);

  ResetControllerTiming();
  EXPECT_EQ(GetInputAge().frames, 0u);
  EXPECT_EQ(GetControllerTiming().age_samples, 0u);
  StopControllerStream();
  EXPECT_EQ(GetInputAge().frames, 0u);
}

TEST(OrcaUXControllerStream, InputAgeIgnoresTheHelpersClock)
{
  FakeBridge bridge;
  StartControllerStream(bridge.Url(), "t0ken");
  ASSERT_TRUE(bridge.WaitConnected());
  const auto now_ms = [] {
    return std::chrono::duration<double, std::milli>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  };
  double skew = -60000;
  const auto send = [&](int buttons) {
    const double t = now_ms() + skew;
    bridge.Send(Event(true, FourPorts(PortJson(0, 0, "wired", buttons)), "", t, t - 3));
  };
  const auto read_after = [&](int buttons, u16 pad) {
    send(buttons);
    ASSERT_TRUE(Eventually([pad] {
      const auto p = LatestPad();
      return p && p->button == pad;
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    LocalPad(true);
  };
  read_after(0x01, PAD_BUTTON_A);
  read_after(0x02, PAD_BUTTON_B);
  skew = 60000;
  read_after(0x04, PAD_BUTTON_X);
  const ControllerTiming t = GetControllerTiming();
  EXPECT_EQ(t.input.changes, 2u);
  EXPECT_NEAR(t.helper_p50_ms, 3.0, 0.01);
  EXPECT_GE(t.input.age_ms / t.input.changes, 23.0);
  EXPECT_LT(t.input.age_ms / t.input.changes, 253.0);
  EXPECT_LT(t.age_max_ms, 253.0);
  EXPECT_EQ(t.hop_samples, 2u);
  EXPECT_LT(t.hop_p95_ms, 100.0);
  StopControllerStream();
}

TEST(OrcaUXLatency, BridgeTransport)
{
  const char* bridge = std::getenv("ORCA_UX_LATENCY_BRIDGE");
  const char* token = std::getenv("ORCA_UX_LATENCY_TOKEN");
  if (!bridge || !token)
    GTEST_SKIP() << "set ORCA_UX_LATENCY_BRIDGE/TOKEN (Tools/orca/controller-latency.mjs)";
  const char* seconds_env = std::getenv("ORCA_UX_LATENCY_SECONDS");
  const int seconds = seconds_env ? std::atoi(seconds_env) : 10;
  StartControllerStream(bridge, token);
  ASSERT_TRUE(Eventually([] { return GetControllerTiming().samples > 10; }, 5000));
  ResetControllerTiming();
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  const ControllerTiming t = GetControllerTiming();
  fmt::print("orca ux latency snapshots={} samples={} refused={} p50={:.3f} p95={:.3f} p99={:.3f} "
             "max={:.3f} ms\n",
             t.snapshots, t.samples, t.refused, t.p50_ms, t.p95_ms, t.p99_ms, t.max_ms);
  EXPECT_EQ(t.refused, 0u);
  EXPECT_GT(t.samples, 0u);
  StopControllerStream();
}

TEST(OrcaUXLatency, BoundaryInputAge)
{
  const char* bridge = std::getenv("ORCA_UX_LATENCY_BRIDGE");
  const char* token = std::getenv("ORCA_UX_LATENCY_TOKEN");
  if (!bridge || !token)
    GTEST_SKIP() << "set ORCA_UX_LATENCY_BRIDGE/TOKEN (Tools/orca/fake_bridge.py)";
  const char* seconds_env = std::getenv("ORCA_UX_LATENCY_SECONDS");
  const int seconds = seconds_env ? std::atoi(seconds_env) : 10;
  StartControllerStream(bridge, token);
  ASSERT_TRUE(Eventually([] { return GetControllerTiming().samples > 10; }, 5000));
  ResetControllerTiming();
  const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(1001.0 / 60000));
  auto next = std::chrono::steady_clock::now();
  const auto end = next + std::chrono::seconds(seconds);
  while (next < end)
  {
    next += period;
    std::this_thread::sleep_until(next);
    LocalPad(true);
  }
  const ControllerTiming t = GetControllerTiming();
  const Orca::Events::InputAge& in = t.input;
  fmt::print("orca ux input age frames={} changes={} reports/frame={:.2f} mean={:.3f} p50={:.3f} "
             "p95={:.3f} p99={:.3f} max={:.3f} ms (helper p50 {:.3f}, cache p50 {:.3f}); hop "
             "above its floor p50={:.3f} p95={:.3f} ms\n",
             in.frames, in.changes, in.frames ? double(in.reports) / in.frames : 0.0,
             in.changes ? in.age_ms / in.changes : 0.0, t.age_p50_ms, t.age_p95_ms, t.age_p99_ms,
             t.age_max_ms, t.helper_p50_ms, t.cache_p50_ms, t.hop_p50_ms, t.hop_p95_ms);
  EXPECT_GT(in.changes, 0u);
  StopControllerStream();
}
#endif

namespace
{
std::string Repeat(std::string_view piece, std::size_t n)
{
  std::string out;
  for (std::size_t i = 0; i < n; ++i)
    out += piece;
  return out;
}
}

TEST(OrcaUXChat, DecodesBothFieldsAsUtf8)
{
  using Orca::UX::Chat::Parse;
  auto line = Parse("chat Ada gg");
  ASSERT_TRUE(line);
  EXPECT_EQ(line->name, "Ada");
  EXPECT_EQ(line->text, "gg");
  line = Parse("chat Ada%20Lovelace gg%20wp!%20(one%20more%3F)%20~_.*'-");
  ASSERT_TRUE(line);
  EXPECT_EQ(line->name, "Ada Lovelace");
  EXPECT_EQ(line->text, "gg wp! (one more?) ~_.*'-");
  line = Parse("chat %C3%89lodie caf%c3%a9%20%E2%80%A6%20%F0%9F%98%80%20100%25%2541");
  ASSERT_TRUE(line);
  EXPECT_EQ(line->name, "\xC3\x89lodie");
  EXPECT_EQ(line->text, "caf\xC3\xA9 \xE2\x80\xA6 \xF0\x9F\x98\x80 100%%41");
}

TEST(OrcaUXChat, LengthLimitsInCodePointsAndEncodedCharacters)
{
  using namespace Orca::UX::Chat;
  EXPECT_TRUE(Parse("chat " + Repeat("a", 24) + " " + Repeat("b", 140)));
  EXPECT_FALSE(Parse("chat " + Repeat("a", 25) + " b"));
  EXPECT_FALSE(Parse("chat a " + Repeat("b", 141)));
  const std::string smile = "%F0%9F%98%80";
  const std::string name = Repeat(smile, 24), text = Repeat(smile, 140);
  EXPECT_EQ(name.size(), NAME_ENCODED);
  EXPECT_EQ(text.size(), TEXT_ENCODED);
  const auto line = Parse("chat " + name + " " + text);
  ASSERT_TRUE(line);
  EXPECT_EQ(line->name, Repeat("\xF0\x9F\x98\x80", 24));
  EXPECT_FALSE(Parse("chat " + name + "a b"));
  EXPECT_FALSE(Parse("chat a " + text + "b"));
  EXPECT_FALSE(DecodeField(Repeat("%41", 97), NAME_ENCODED, 1000));
  EXPECT_TRUE(DecodeField(Repeat("%41", 96), NAME_ENCODED, 1000));
  EXPECT_FALSE(DecodeField("", NAME_ENCODED, NAME_POINTS));
}

TEST(OrcaUXChat, RefusesControlCharactersNewlinesAndBadEncodings)
{
  using Orca::UX::Chat::Parse;
  const char* bad[] = {
      "chat a b%0Ac", "chat a b%0Dc", "chat a b%00c", "chat a b%09c", "chat a b%7Fc",
      "chat a%0A b", "chat a b%1B[2J",
      "chat a b%C2%85c",
      "chat a b%C2%9Bc",
      "chat a b%E2%80%A8c",
      "chat a b%E2%80%A9c",
      "chat a b c", "chat a  b", "chat  a b", "chat a b ", "chat a\tb", "chat a b\tc",
      "chat a b+c", "chat a b/c", "chat a b\xC3\xA9", "chat a b\nquit",
      "chat a %", "chat a b%", "chat a b%4", "chat a b%G0", "chat a b%zz", "chat a%2 b",
      "chat a %80", "chat a %C3", "chat a %E2%80", "chat a %C0%AF", "chat a %E0%80%AF",
      "chat a %F0%80%80%AF", "chat a %ED%A0%80", "chat a %F4%90%80%80", "chat a %FF", "chat a %FE",
      "chat", "chat ", "chat a", "chat a ", "Chat a b", "chatty a b", " chat a b",
  };
  for (const char* line : bad)
    EXPECT_FALSE(Parse(line)) << line;
  EXPECT_TRUE(Parse("chat a %F4%8F%BF%BF"));
}

TEST(OrcaUXOverlay, ChatLinesStayEightSecondsTheNewestThree)
{
  OverlayModel m;
  m.AddToast("ada joined on port 2", 0);
  m.AddChat("ada", "hi", 0);
  auto v = m.View(10, Stats::Off);
  ASSERT_EQ(v.chats.size(), 1u);
  EXPECT_EQ(v.chats[0].name, "ada");
  EXPECT_EQ(v.chats[0].text, "hi");
  EXPECT_FLOAT_EQ(v.chats[0].alpha, 1);
  ASSERT_EQ(v.toasts.size(), 1u);
  v = m.View(5000, Stats::Off);
  EXPECT_TRUE(v.toasts.empty());
  ASSERT_EQ(v.chats.size(), 1u);
  EXPECT_NEAR(m.View(7750, Stats::Off).chats[0].alpha, 0.5f, 1e-4);
  EXPECT_TRUE(m.View(8001, Stats::Off).chats.empty());
  for (int i = 0; i < 4; ++i)
    m.AddChat("bo", fmt::format("line {}", i), 9000 + i);
  v = m.View(9100, Stats::Off);
  ASSERT_EQ(v.chats.size(), 3u);
  EXPECT_EQ(v.chats[0].text, "line 1");
  EXPECT_EQ(v.chats[2].text, "line 3");
  LinkView link;
  m.Update(link, 20000);
  EXPECT_TRUE(m.View(20000, Stats::Off).chats.empty());
}

TEST(OrcaUXAppOrb, ParsesTheButtonsPlaceAndItsOffAndBlink)
{
  using namespace Orca::UX::AppOrb;
  auto c = ParseOrb("orb 32 32 80 0 1 3");
  ASSERT_TRUE(c);
  EXPECT_EQ(c->type, Command::Type::Place);
  EXPECT_EQ(c->place.x, 32);
  EXPECT_EQ(c->place.y, 32);
  EXPECT_EQ(c->place.size, 80);
  EXPECT_FALSE(c->place.lit);
  EXPECT_TRUE(c->place.away);
  EXPECT_EQ(c->place.badge, 3);
  c = ParseOrb("orb off");
  ASSERT_TRUE(c);
  EXPECT_EQ(c->type, Command::Type::Off);
  c = ParseOrb("orb blink");
  ASSERT_TRUE(c);
  EXPECT_EQ(c->type, Command::Type::Blink);
  for (const char* bad :
       {"orb", "orb ", "orb on", "orb 32 32 80 0 1", "orb 32 32 80 0 1 3 4", "orb -1 32 80 0 0 0",
        "orb 32 32 4 0 0 0", "orb 32 32 80 2 0 0", "orb 32 32 80 0 0 100", "orb 32  32 80 0 0 0",
        "orb 32 32 80 0 0 0 ", "orb 1e3 32 80 0 0 0", "orb 99999 32 80 0 0 0", "orbs off",
        "orb off blink"})
  {
    EXPECT_FALSE(ParseOrb(bad)) << bad;
  }
}

TEST(OrcaUXAppOrb, ParsesNoticesWithAndWithoutAHint)
{
  using namespace Orca::UX::AppOrb;
  auto n = ParseNotice("notice voice Ada is%20on%20voice");
  ASSERT_TRUE(n);
  EXPECT_EQ(n->kind, Kind::Voice);
  EXPECT_EQ(n->name, "Ada");
  EXPECT_EQ(n->text, "is on voice");
  EXPECT_TRUE(n->key.empty());
  n = ParseNotice("notice dm Bo%20Li hey%20%F0%9F%91%8B Shift%2BTab Reply");
  ASSERT_TRUE(n);
  EXPECT_EQ(n->kind, Kind::Dm);
  EXPECT_EQ(n->name, "Bo Li");
  EXPECT_EQ(n->text, "hey \xF0\x9F\x91\x8B");
  EXPECT_EQ(n->key, "Shift+Tab");
  EXPECT_EQ(n->verb, "Reply");
  for (const char* kind : {"join", "leave", "away", "back", "voice", "call", "dm", "invite"})
    EXPECT_TRUE(ParseNotice(std::string("notice ") + kind + " Ada x")) << kind;
  for (const char* bad :
       {"notice", "notice join", "notice join Ada", "notice wave Ada hi", "notice join Ada hi PS",
        "notice join Ada hi PS Join extra", "notice join Ada%0A hi", "notice join Ada hi%",
        "notice join Ada a b", "notice  join Ada hi", "Notice join Ada hi"})
  {
    EXPECT_FALSE(ParseNotice(bad)) << bad;
  }
}

TEST(OrcaUXAppOrb, LinesStayAsLongAsThePagesTheNewestThree)
{
  using namespace Orca::UX::AppOrb;
  Model m;
  const auto notice = [](Kind kind, std::string text) {
    Notice n;
    n.kind = kind;
    n.name = "Ada";
    n.text = std::move(text);
    return n;
  };
  EXPECT_EQ(DwellMs(notice(Kind::Voice, "is on voice")), 8000);
  EXPECT_EQ(DwellMs(notice(Kind::Leave, "left")), 2500);
  EXPECT_EQ(DwellMs(notice(Kind::Dm, "gg")), 4120);
  EXPECT_EQ(DwellMs(notice(Kind::Dm, std::string(200, 'a'))), 9000);
  const Notice join = notice(Kind::Join, "joined");
  EXPECT_EQ(LineAlpha(join, 0, 1000), 1.0f);
  EXPECT_NEAR(LineAlpha(join, 0, 3300), 0.5f, 0.01f);
  EXPECT_EQ(LineAlpha(join, 0, 3500), 0.0f);
  EXPECT_FALSE(m.At(0).showing);
  m.Add(join, 0);
  EXPECT_TRUE(m.At(1000).showing);
  EXPECT_FALSE(m.At(3500).showing);
  std::vector<Model::Entry> lines;
  std::uint64_t generation = m.Lines(0, &lines);
  ASSERT_EQ(lines.size(), 1u);
  lines.clear();
  EXPECT_EQ(m.Lines(generation, &lines), generation);
  EXPECT_TRUE(lines.empty());
  for (int i = 0; i < 5; ++i)
    m.Add(notice(Kind::Voice, std::to_string(i)), 4000 + i);
  EXPECT_EQ(m.At(4010).generation, generation + 5);
  generation = m.Lines(generation, &lines);
  ASSERT_EQ(lines.size(), Model::KEEP);
  EXPECT_EQ(lines.front().notice.text, "2");
  EXPECT_EQ(lines.back().notice.text, "4");
  EXPECT_EQ(lines.back().at, 4004);
}

TEST(OrcaUXAppOrb, HostControlHitTestsFollowBothDiscsAtEveryScale)
{
  using namespace Orca::UX::AppOrb;
  for (const float k : {0.5f, 1.0f, 2.0f, 3.0f})
  {
    SCOPED_TRACE(k);
    const Place p{32, 24, 40 * k};
    const auto hit = [&](float x, float y) {
      return ControlAt(p, p.x + x * k, p.y + y * k);
    };
    EXPECT_EQ(hit(20, 20), 0);
    EXPECT_EQ(hit(0, 20), 0);
    EXPECT_EQ(hit(40, 20), 0);
    EXPECT_EQ(hit(44, 20), -1);
    EXPECT_EQ(hit(68, 20), 1);
    EXPECT_EQ(hit(48, 20), 1);
    EXPECT_EQ(hit(88, 20), 1);
    EXPECT_EQ(hit(68, 0), 1);
    EXPECT_EQ(hit(68, 40), 1);
    EXPECT_EQ(hit(68, -0.1f), -1);
    EXPECT_EQ(hit(88.1f, 20), -1);
    for (const float x : {48.0f, 88.0f})
      for (const float y : {0.0f, 40.0f})
        EXPECT_EQ(hit(x, y), -1);
    EXPECT_EQ(hit(1, 1), -1);
    EXPECT_EQ(hit(20, 44), -1);
    for (int row = 0; row < 7; ++row)
      EXPECT_EQ(hit(20, 48 + row * 32), -1);
  }
  EXPECT_EQ(ControlAt({}, 0, 0), -1);
}

TEST(OrcaUXAppOrb, NativeHoverAndPressBelongToOnlyTheirOriginalControl)
{
  using namespace Orca::UX::AppOrb;
  Model m;
  m.Apply(*ParseOrb("orb 16 16 40 0 0 0"), 0);
  m.Apply(*ParseOrb("orb blink"), 100);
  const auto before = m.At(450);
  m.SetPointer(1, false);
  auto v = m.At(450);
  EXPECT_TRUE(v.feedback.hovered);
  EXPECT_FALSE(v.feedback.pressed);
  EXPECT_FALSE(v.orb.hovered);
  EXPECT_FALSE(v.orb.pressed);
  EXPECT_EQ(v.place.lit, before.place.lit);
  EXPECT_EQ(v.ring, before.ring);
  EXPECT_EQ(v.listening, before.listening);
  m.SetPointer(1, true);
  EXPECT_TRUE(m.At(450).feedback.pressed);
  EXPECT_FALSE(m.At(450).orb.pressed);
  m.SetPointer(0, true);
  EXPECT_TRUE(m.At(450).orb.hovered);
  EXPECT_FALSE(m.At(450).orb.pressed);
  EXPECT_FALSE(m.At(450).feedback.pressed);
  m.SetPointer(0, false);
  m.SetPointer(0, true);
  EXPECT_TRUE(m.At(450).orb.pressed);
  EXPECT_FALSE(m.At(450).feedback.hovered);
  EXPECT_FALSE(m.At(450).feedback.pressed);
  m.SetPointer(1, true);
  EXPECT_FALSE(m.At(450).feedback.pressed);
  m.SetPointer(-1, false);
  m.SetPointer(-1, true);
  m.SetPointer(1, true);
  EXPECT_FALSE(m.At(450).feedback.pressed);
  m.SetPointer(1, false);
  m.SetTalk({Talk::Phase::Listen, "Ada", "Listening"}, 3000);
  const auto listening = m.At(3300);
  ASSERT_TRUE(listening.listening);
  m.SetPointer(1, true);
  EXPECT_EQ(m.At(3300).ring, listening.ring);
  EXPECT_TRUE(m.At(3300).listening);
  m.Apply({}, 3400);
  EXPECT_FALSE(m.At(3400).feedback.hovered);
  EXPECT_FALSE(m.At(3400).feedback.pressed);
}

TEST(OrcaUXAppOrb, TheButtonShowsBlinksAndTakesPressesOnItsDisc)
{
  using namespace Orca::UX::AppOrb;
  Model m;
  EXPECT_FALSE(m.At(0).on);
  EXPECT_FALSE(m.Hit(50, 50));
  m.Apply(*ParseOrb("orb 32 32 80 1 0 0"), 0);
  auto v = m.At(0);
  EXPECT_TRUE(v.on);
  EXPECT_LT(v.ring, 0);
  EXPECT_TRUE(m.Hit(72, 72));
  EXPECT_TRUE(m.Hit(33, 72));
  EXPECT_FALSE(m.Hit(34, 34));
  EXPECT_FALSE(m.Hit(120, 72));
  m.Apply(*ParseOrb("orb blink"), 1000);
  EXPECT_NEAR(m.At(1350).ring, 0.5f, 0.01f);
  EXPECT_NEAR(m.At(2050).ring, 0.5f, 0.01f);
  EXPECT_LT(m.At(3100).ring, 0);
  Notice n;
  n.kind = Kind::Voice;
  n.name = "Ada";
  n.text = "is on voice";
  m.Add(n, 4000);
  m.Apply(*ParseOrb("orb blink"), 4000);
  const std::uint64_t before = m.At(4050).generation;
  EXPECT_TRUE(m.At(4050).showing);
  m.Apply(*ParseOrb("orb off"), 4100);
  EXPECT_FALSE(m.At(4200).on);
  EXPECT_LT(m.At(4200).ring, 0);
  EXPECT_FALSE(m.At(4200).showing);
  EXPECT_GT(m.At(4200).generation, before);
  EXPECT_FALSE(m.Hit(72, 72));
  const Kit::Box none = ButtonBox(m.At(4200));
  EXPECT_EQ(none.max.x, none.min.x);
}

TEST(OrcaUXOrbCombo, UpThenStartFiresOnceAndMasksStartAndTheDpadUntilBothAreLetGo)
{
  using Orca::UX::OrbCombo;
  constexpr u16 UP = PAD_BUTTON_UP, START = PAD_BUTTON_START, A = PAD_BUTTON_A,
                LEFT = PAD_BUTTON_LEFT;
  OrbCombo c;
  auto s = c.Next(UP);
  EXPECT_FALSE(s.fired);
  EXPECT_EQ(s.buttons, UP);
  s = c.Next(UP | START | A);
  EXPECT_TRUE(s.fired);
  EXPECT_EQ(s.buttons, A);
  s = c.Next(UP | START);
  EXPECT_FALSE(s.fired);
  EXPECT_EQ(s.buttons, 0);
  s = c.Next(UP | LEFT);
  EXPECT_FALSE(s.fired);
  EXPECT_EQ(s.buttons, 0);
  EXPECT_EQ(c.Next(0).buttons, 0);
  EXPECT_FALSE(c.Held());
  EXPECT_EQ(c.Next(LEFT).buttons, LEFT);
  c.Next(0);
  c.Next(UP);
  EXPECT_TRUE(c.Next(UP | START).fired);
  c.Next(0);
}

TEST(OrcaUXOrbCombo, StartFirstOrStartHeldIsNotTheShortcut)
{
  using Orca::UX::OrbCombo;
  constexpr u16 UP = PAD_BUTTON_UP, START = PAD_BUTTON_START, DOWN = PAD_BUTTON_DOWN;
  OrbCombo c;
  EXPECT_EQ(c.Next(START).buttons, START);
  auto s = c.Next(START | UP);
  EXPECT_FALSE(s.fired);
  EXPECT_EQ(s.buttons, START | UP);
  c.Next(0);
  c.Next(DOWN);
  s = c.Next(DOWN | START);
  EXPECT_FALSE(s.fired);
  EXPECT_EQ(s.buttons, DOWN | START);
}

TEST(OrcaUXOrbCombo, TheLocalPadKeepsItsStateWhileUnfocusedAndOnReRuns)
{
  using Orca::UX::FilterLocalPad;
  constexpr u16 UP = PAD_BUTTON_UP, START = PAD_BUTTON_START;
  GCPadStatus pad;
  const auto with = [&pad](u16 buttons) {
    GCPadStatus p = pad;
    p.button = buttons;
    return p;
  };
  EXPECT_EQ(FilterLocalPad(with(UP), true, true).button, UP);
  EXPECT_EQ(FilterLocalPad(with(UP | START), true, true).button, 0);
  EXPECT_EQ(FilterLocalPad(with(UP | START), false, true).button, 0);
  EXPECT_EQ(FilterLocalPad(with(0), false, true).button, 0);
  EXPECT_EQ(FilterLocalPad(with(START), true, true).button, 0);
  EXPECT_EQ(FilterLocalPad(with(0), true, false).button, 0);
  EXPECT_EQ(FilterLocalPad(with(START), true, true).button, 0);
  EXPECT_EQ(FilterLocalPad(with(UP | START), true, true).button, 0);
  EXPECT_EQ(FilterLocalPad(with(0), true, true).button, 0);
  EXPECT_EQ(FilterLocalPad(with(START), true, true).button, START);
  FilterLocalPad(with(0), true, true);
}

TEST(OrcaUXOrbCombo, LettingGoOfStartEndsAHoldOnceWithTheMaskUnchanged)
{
  using Orca::UX::OrbCombo;
  constexpr u16 UP = PAD_BUTTON_UP, START = PAD_BUTTON_START;
  OrbCombo c;
  c.Next(UP);
  auto s = c.Next(UP | START);
  EXPECT_TRUE(s.fired);
  EXPECT_FALSE(s.released);
  s = c.Next(UP | START);
  EXPECT_FALSE(s.fired);
  EXPECT_FALSE(s.released);
  s = c.Next(UP);
  EXPECT_TRUE(s.released);
  EXPECT_EQ(s.buttons, 0);
  EXPECT_TRUE(c.Held());
  s = c.Next(UP);
  EXPECT_FALSE(s.released);
  EXPECT_EQ(s.buttons, 0);
  s = c.Next(UP | START);
  EXPECT_FALSE(s.fired);
  EXPECT_EQ(s.buttons, 0);
  s = c.Next(0);
  EXPECT_FALSE(s.released);
  EXPECT_FALSE(c.Held());
  c.Next(UP);
  EXPECT_TRUE(c.Next(UP | START).fired);
  s = c.Next(0);
  EXPECT_TRUE(s.released);
  EXPECT_EQ(s.buttons, 0);
  EXPECT_FALSE(c.Held());
  EXPECT_FALSE(c.Next(START).released);
  EXPECT_FALSE(c.Next(0).released);
}

TEST(OrcaUXOverlayKey, APressIsADownThenOneUpOrCancelAndTheToggleWithoutTalk)
{
  using Orca::UX::OverlayKey::Press;
  using Orca::UX::OverlayKey::Source;
  Press keys(Source::Keys);
  EXPECT_EQ(keys.Down(false), "orca orb");
  EXPECT_FALSE(keys.IsDown());
  EXPECT_EQ(keys.Up(), "");
  EXPECT_EQ(keys.Cancel(), "");
  EXPECT_EQ(keys.Down(true), "orca overlay down keys");
  EXPECT_TRUE(keys.IsDown());
  EXPECT_EQ(keys.Down(true), "") << "a second press while down is the same press";
  EXPECT_EQ(keys.Up(), "orca overlay up keys");
  EXPECT_EQ(keys.Up(), "");
  EXPECT_EQ(keys.Cancel(), "");
  EXPECT_EQ(keys.Down(true), "orca overlay down keys");
  EXPECT_EQ(keys.Cancel(), "orca overlay cancel keys");
  EXPECT_EQ(keys.Cancel(), "");
  EXPECT_EQ(keys.Up(), "");
  Press pad(Source::Pad);
  EXPECT_EQ(pad.Down(true), "orca overlay down pad");
  EXPECT_EQ(pad.Down(false), "");
  EXPECT_EQ(pad.Up(), "orca overlay up pad");
  EXPECT_EQ(pad.Down(true), "orca overlay down pad");
  EXPECT_EQ(pad.Cancel(), "orca overlay cancel pad");
}

TEST(OrcaUXOrbCombo, TheLocalPadPrintsItsPressAndReleaseWithTalk)
{
  using Orca::UX::FilterLocalPad;
  constexpr u16 UP = PAD_BUTTON_UP, START = PAD_BUTTON_START;
  GCPadStatus pad;
  const auto with = [&pad](u16 buttons) {
    GCPadStatus p = pad;
    p.button = buttons;
    return p;
  };
  std::FILE* const out = std::tmpfile();
  ASSERT_NE(out, nullptr);
  const auto printed = [out] {
    std::fflush(out);
    std::rewind(out);
    std::string text;
    char buffer[256];
    while (std::fgets(buffer, sizeof(buffer), out))
      text += buffer;
    std::fseek(out, 0, SEEK_END);
    return text;
  };
  const char* const was = std::getenv("ORCA_SESSION");
  const std::string saved = was ? was : "";
  const auto session = [](const char* value) {
#ifdef _WIN32
    _putenv_s("ORCA_SESSION", value ? value : "");
#else
    if (value)
      setenv("ORCA_SESSION", value, 1);
    else
      unsetenv("ORCA_SESSION");
#endif
  };
  Orca::Status::SetAppCaps("talk");
  ASSERT_TRUE(Orca::Status::Cap(Orca::Status::TALK_CAP));
  session("1");
  Orca::Status::SetOutput(out);

  EXPECT_EQ(FilterLocalPad(with(UP), true, true).button, UP);
  EXPECT_EQ(FilterLocalPad(with(UP | START), true, true).button, 0);
  EXPECT_EQ(FilterLocalPad(with(UP), false, true).button, 0);
  EXPECT_EQ(FilterLocalPad(with(UP | START), true, true).button, 0);
  EXPECT_EQ(FilterLocalPad(with(UP), true, true).button, 0);
  EXPECT_EQ(FilterLocalPad(with(0), true, true).button, 0);
  EXPECT_EQ(printed(), "orca overlay down pad\norca overlay up pad\n");

  FilterLocalPad(with(UP), true, true);
  FilterLocalPad(with(UP | START), true, true);
  EXPECT_EQ(FilterLocalPad(with(0), true, false).button, 0);
  EXPECT_EQ(FilterLocalPad(with(0), true, false).button, 0);
  FilterLocalPad(with(0), true, true);
  FilterLocalPad(with(UP), true, true);
  FilterLocalPad(with(UP | START), true, true);
  Orca::UX::LocalPadStopped();
  Orca::UX::LocalPadStopped();
  FilterLocalPad(with(0), true, true);
  EXPECT_EQ(printed(), "orca overlay down pad\norca overlay up pad\n"
                       "orca overlay down pad\norca overlay cancel pad\n"
                       "orca overlay down pad\norca overlay cancel pad\n");

  Orca::Status::SetAppCaps("");
  FilterLocalPad(with(UP), true, true);
  FilterLocalPad(with(UP | START), true, true);
  FilterLocalPad(with(0), true, true);
  const std::string all = printed();
  EXPECT_EQ(all.substr(all.size() - 9), "orca orb\n");

  Orca::Status::SetOutput(nullptr);
  session(was ? saved.c_str() : nullptr);
  std::fclose(out);
  EXPECT_FALSE(Orca::UX::OverlayKey::Pad().IsDown());
}

TEST(OrcaUXOverlayKey, APadLetGoAtATimeOrcaCannotPlaceIsCancelledOncePastTheTapLine)
{
  using Orca::UX::OverlayKey::PAGE_HOLD_MS;
  using Orca::UX::OverlayKey::Press;
  using Orca::UX::OverlayKey::READ_GAP_MS;
  using Orca::UX::OverlayKey::Source;
  Press pad(Source::Pad);
  EXPECT_EQ(pad.Down(true, 1000), "orca overlay down pad");
  pad.Seen(1017);
  EXPECT_EQ(pad.Release(1034), "orca overlay up pad");
  EXPECT_EQ(pad.Down(true, 2000), "orca overlay down pad");
  for (Orca::UX::OverlayKey::Ms t = 2017; t <= 4000; t += 17)
    pad.Seen(t);
  EXPECT_EQ(pad.Release(4017), "orca overlay up pad");
  EXPECT_EQ(pad.Release(4034), "") << "once per down";
  pad.Down(true, 5000);
  EXPECT_EQ(pad.Release(5000 + PAGE_HOLD_MS - 1), "orca overlay up pad");
  pad.Down(true, 6000);
  pad.Seen(6017);
  EXPECT_EQ(pad.Release(6000 + PAGE_HOLD_MS), "orca overlay cancel pad");
  pad.Down(true, 7000);
  pad.Seen(9000);
  EXPECT_EQ(pad.Release(9000 + READ_GAP_MS + 1), "orca overlay cancel pad");
  pad.Down(true, 10000);
  pad.Seen(12000);
  EXPECT_EQ(pad.Release(12000 + READ_GAP_MS), "orca overlay up pad");
  pad.Down(true, 13000);
  pad.Seen(13400);
  pad.Seen(13100);
  EXPECT_EQ(pad.Release(13417), "orca overlay up pad");
  Press keys(Source::Keys);
  keys.Down(true, 0);
  EXPECT_EQ(keys.Up(), "orca overlay up keys");
}

namespace
{
class StatusLines
{
public:
  explicit StatusLines(const char* caps)
  {
    const char* const was = std::getenv("ORCA_SESSION");
    m_had = was != nullptr;
    m_saved = was ? was : "";
    Orca::Status::SetAppCaps(caps);
    SetSession("1");
    m_out = std::tmpfile();
    Orca::Status::SetOutput(m_out);
  }
  ~StatusLines()
  {
    Orca::Status::SetOutput(nullptr);
    Orca::Status::SetAppCaps("");
    SetSession(m_had ? m_saved.c_str() : nullptr);
    if (m_out)
      std::fclose(m_out);
  }
  StatusLines(const StatusLines&) = delete;
  StatusLines& operator=(const StatusLines&) = delete;
  bool Ok() const { return m_out != nullptr; }
  std::string Take()
  {
    std::fflush(m_out);
    std::fseek(m_out, m_taken, SEEK_SET);
    std::string text;
    char buffer[256];
    while (std::fgets(buffer, sizeof(buffer), m_out))
      text += buffer;
    std::fseek(m_out, 0, SEEK_END);
    m_taken = std::ftell(m_out);
    return text;
  }

private:
  static void SetSession(const char* value)
  {
#ifdef _WIN32
    _putenv_s("ORCA_SESSION", value ? value : "");
#else
    if (value)
      setenv("ORCA_SESSION", value, 1);
    else
      unsetenv("ORCA_SESSION");
#endif
  }
  std::FILE* m_out = nullptr;
  long m_taken = 0;
  bool m_had = false;
  std::string m_saved;
};
}

TEST(OrcaUXOrbCombo, AStallEndsATalkPressWhenStartIsLetGoWithTheSentPadsUnchanged)
{
  using Orca::UX::FilterLocalPad;
  using Orca::UX::LocalPadWaiting;
  using Orca::UX::OrbCombo;
  using Orca::UX::OverlayKey::Ms;
  constexpr u16 UP = PAD_BUTTON_UP, START = PAD_BUTTON_START, A = PAD_BUTTON_A;
  StatusLines lines("talk");
  ASSERT_TRUE(lines.Ok());
  ASSERT_TRUE(Orca::Status::Cap(Orca::Status::TALK_CAP));
  GCPadStatus pad;
  const auto with = [&pad](u16 buttons) {
    GCPadStatus p = pad;
    p.button = buttons;
    return p;
  };
  Orca::UX::LocalPadStopped();
  FilterLocalPad(with(0), true, true, 0);
  FilterLocalPad(with(0), true, true, 0);
  lines.Take();
  OrbCombo sent;
  Ms t = 100'000;
  const auto frame = [&](u16 buttons) {
    t += 17;
    const u16 got = FilterLocalPad(with(buttons), true, true, t).button;
    EXPECT_EQ(got, sent.Next(buttons).buttons) << "frame at " << t;
    return got;
  };
  const auto stall = [&](int ms, u16 buttons) {
    for (int i = 0; i < ms; ++i)
      LocalPadWaiting(buttons, ++t);
  };

  frame(UP);
  EXPECT_EQ(frame(UP | START), 0);
  EXPECT_EQ(lines.Take(), "orca overlay down pad\n");
  stall(3000, UP | START);
  EXPECT_EQ(lines.Take(), "");
  stall(1, UP);
  EXPECT_EQ(lines.Take(), "orca overlay up pad\n") << "within one stall tick";
  stall(7000, UP);
  EXPECT_EQ(frame(UP), 0);
  EXPECT_EQ(frame(UP | A), A);
  EXPECT_EQ(frame(A), A);
  EXPECT_EQ(lines.Take(), "") << "one up per down";

  frame(UP);
  frame(UP | START);
  stall(100, UP | START);
  stall(1, 0);
  stall(5000, 0);
  frame(0);
  EXPECT_EQ(lines.Take(), "orca overlay down pad\norca overlay up pad\n");

  frame(UP);
  frame(UP | START);
  for (int i = 0; i < 4000; ++i)
    LocalPadWaiting(std::nullopt, ++t);
  frame(UP);
  frame(0);
  EXPECT_EQ(lines.Take(), "orca overlay down pad\norca overlay cancel pad\n");

  frame(UP);
  frame(UP | START);
  stall(50, UP | START);
  stall(50, UP);
  stall(50, UP | START);
  EXPECT_EQ(frame(UP | START), 0);
  EXPECT_EQ(frame(0), 0);
  EXPECT_EQ(lines.Take(), "orca overlay down pad\norca overlay up pad\n");

  frame(UP);
  frame(UP | START);
  stall(20, UP | START);
  EXPECT_EQ(lines.Take(), "orca overlay down pad\n");
  t += 17;
  EXPECT_EQ(FilterLocalPad(with(UP | START), true, false, t).button, 0) << "still masked";
  EXPECT_EQ(lines.Take(), "orca overlay cancel pad\n");
  stall(20, 0);
  EXPECT_EQ(frame(0), 0);
  EXPECT_EQ(lines.Take(), "") << "one cancel per down";

  stall(50, 0);
  stall(50, UP | START);
  frame(0);
  EXPECT_EQ(lines.Take(), "");
  EXPECT_FALSE(Orca::UX::OverlayKey::Pad().IsDown());
}

TEST(OrcaUXAppOrb, ParsesTalkLines)
{
  using namespace Orca::UX::AppOrb;
  auto t = ParseTalk("talk listen Ada Listening%E2%80%A6%20let%20go%20to%20send");
  ASSERT_TRUE(t);
  EXPECT_EQ(t->phase, Talk::Phase::Listen);
  EXPECT_EQ(t->name, "Ada");
  EXPECT_EQ(t->text, "Listening\xE2\x80\xA6 let go to send");
  t = ParseTalk("talk sent Bo%20Li gg%20wp");
  ASSERT_TRUE(t);
  EXPECT_EQ(t->phase, Talk::Phase::Sent);
  EXPECT_EQ(t->name, "Bo Li");
  EXPECT_EQ(t->text, "gg wp");
  EXPECT_EQ(ParseTalk("talk send Ada hi")->phase, Talk::Phase::Send);
  EXPECT_EQ(ParseTalk("talk error Ada No%20mic")->phase, Talk::Phase::Error);
  t = ParseTalk("talk off");
  ASSERT_TRUE(t);
  EXPECT_EQ(t->phase, Talk::Phase::Off);
  for (const char* bad : {"talk", "talk ", "talk off now", "talk listen", "talk listen Ada",
                          "talk shout Ada hi", "talk off Ada hi", "talk listen Ada hi there",
                          "talk listen Ada%0A hi", "talk listen Ada hi%", "talk  listen Ada hi",
                          "talk listen Ada hi ", "Talk off", "talks off"})
  {
    EXPECT_FALSE(ParseTalk(bad)) << bad;
  }
}

TEST(OrcaUXAppOrb, TheTalkRowIsReplacedInPlaceAndOutlivesTheNotices)
{
  using namespace Orca::UX::AppOrb;
  const auto talk = [](Talk::Phase phase, std::string text) {
    Talk t;
    t.phase = phase;
    t.name = "Ada";
    t.text = std::move(text);
    return t;
  };
  Model m;
  m.Apply(*ParseOrb("orb 32 32 80 1 0 0"), 0);
  EXPECT_FALSE(m.At(0).showing);
  m.SetTalk(talk(Talk::Phase::Listen, "Listening"), 1000);
  auto v = m.At(1000);
  EXPECT_TRUE(v.showing);
  EXPECT_TRUE(v.listening);
  EXPECT_NEAR(v.ring, 0.0f, 0.01f);
  Model::TalkEntry row;
  std::uint64_t generation = m.TalkRow(0, &row);
  EXPECT_EQ(row.talk.text, "Listening");
  m.SetTalk(talk(Talk::Phase::Listen, "gg"), 1300);
  EXPECT_EQ(m.At(1300).talk_generation, generation + 1);
  generation = m.TalkRow(generation, &row);
  EXPECT_EQ(row.talk.text, "gg");
  EXPECT_EQ(row.at, 1300);
  EXPECT_NEAR(m.At(1600).ring, 0.5f, 0.01f);
  Notice n;
  n.kind = Kind::Leave;
  n.name = "Bo";
  n.text = "left";
  for (int i = 0; i < 5; ++i)
    m.Add(n, 2000 + i);
  std::vector<Model::Entry> lines;
  m.Lines(0, &lines);
  EXPECT_EQ(lines.size(), Model::KEEP);
  EXPECT_TRUE(m.At(30000).showing);
  EXPECT_EQ(TalkAlpha(row.talk, row.at, 30000), 1.0f);
  m.Apply(*ParseOrb("orb blink"), 30000);
  EXPECT_FALSE(m.At(30350).listening);
  EXPECT_NEAR(m.At(30350).ring, 0.5f, 0.01f);
  m.SetTalk(talk(Talk::Phase::Send, "gg"), 40000);
  EXPECT_FALSE(m.At(41000).listening);
  EXPECT_LT(m.At(41000).ring, 0);
  EXPECT_TRUE(m.At(90000).showing);
  const Talk sent = talk(Talk::Phase::Sent, "gg");
  EXPECT_EQ(TalkAlpha(sent, 0, 2000), 1.0f);
  EXPECT_NEAR(TalkAlpha(sent, 0, 2800), 0.5f, 0.01f);
  EXPECT_EQ(TalkAlpha(sent, 0, 3000), 0.0f);
  EXPECT_EQ(TalkAlpha(talk(Talk::Phase::Error, "No mic"), 0, 3900), 0.25f);
  m.SetTalk(sent, 100000);
  EXPECT_TRUE(m.At(102000).showing);
  EXPECT_FALSE(m.At(103000).showing);
  m.SetTalk(talk(Talk::Phase::Listen, "hi"), 200000);
  EXPECT_TRUE(m.At(200000 + TALK_STALE_MS - 1000).showing);
  EXPECT_FALSE(m.At(200000 + TALK_STALE_MS).showing);
  m.SetTalk(talk(Talk::Phase::Listen, "hi"), 300000);
  m.SetTalk(Talk{}, 300100);
  EXPECT_FALSE(m.At(300100).showing);
  EXPECT_LT(m.At(300100).ring, 0);
  m.SetTalk(talk(Talk::Phase::Listen, "hi"), 300200);
  generation = m.At(300200).talk_generation;
  m.Apply(*ParseOrb("orb off"), 300300);
  EXPECT_FALSE(m.At(300300).showing);
  EXPECT_GT(m.At(300300).talk_generation, generation);
  m.TalkRow(0, &row);
  EXPECT_EQ(row.talk.phase, Talk::Phase::Off);
}

TEST(OrcaUXAppOrb, ThePlaceScalesFromThePagesPixelsToTheBackbuffer)
{
  using namespace Orca::UX::AppOrb;
  Place p;
  p.x = 32;
  p.y = 32;
  p.size = 80;
  Place q = ScaledPlace(p, 3456, 1728);
  EXPECT_EQ(q.x, 16);
  EXPECT_EQ(q.y, 16);
  EXPECT_EQ(q.size, 40);
  q = ScaledPlace(p, 1920, 1920);
  EXPECT_EQ(q.size, 80);
  q = ScaledPlace(p, 0, 1280);
  EXPECT_EQ(q.x, 32);
  EXPECT_EQ(q.size, 80);
  Model m;
  m.SetView(3456, 2168);
  EXPECT_EQ(m.At(0).view_width, 3456);
  m.SetView(0, 0);
  EXPECT_EQ(m.At(0).view_width, 0);
}

TEST(OrcaUXAppOrb, ThePillsWordsAreCutAfterASpaceAndNeverInsideACharacter)
{
  using Orca::UX::AppOrb::FitPrefix;
  const auto w = [](std::string_view t) { return static_cast<float>(t.size()); };
  EXPECT_EQ(FitPrefix("gg wp", 10, w), 5u);
  EXPECT_EQ(FitPrefix("are you ready", 9, w), 7u);
  EXPECT_EQ(FitPrefix("supercalifragilistic", 6, w), 6u);
  EXPECT_EQ(FitPrefix("caf\xC3\xA9s", 4, w), 3u);
  EXPECT_EQ(FitPrefix("ab", 0, w), 0u);
  EXPECT_EQ(FitPrefix("abcde fg hij", 8, w), 8u);
}

TEST(OrcaUXAppOrb, LiveWordsKeepTheirEndAndAreCutBeforeAWord)
{
  using Orca::UX::AppOrb::FitSuffix;
  const auto w = [](std::string_view t) { return static_cast<float>(t.size()); };
  EXPECT_EQ(FitSuffix("gg wp", 10, w), 0u);
  EXPECT_EQ(FitSuffix("are you ready", 9, w), 4u);
  EXPECT_EQ(FitSuffix("are you ready", 7, w), 8u);
  EXPECT_EQ(FitSuffix("supercalifragilistic", 6, w), 14u);
  EXPECT_EQ(FitSuffix("caf\xC3\xA9s", 2, w), 5u);
  EXPECT_EQ(FitSuffix("ab", 0, w), 2u);
}

TEST(OrcaUXNameTags, SelectedSmashControlsSurviveAStaleAccountTiltProfile)
{
  FakeBrawl host, joiner;
  auto tilt = FakeBrawl::CONTROLS;
  tilt[8] = 0;
  for (auto* memory : {&host, &joiner})
  {
    GameMadeTag(*memory, 40, u"SMASH");
    memory->Join(0);
    memory->Join(1);
    Wear(*memory, 1, 40);
  }
  for (int frame = 0; frame < 60; ++frame)
  {
    ApplyNameTags(host, {{0, "host", false}, {1, "friend", true, Profile(1, tilt)}});
    ApplyNameTags(joiner, {{0, "host", true}, {1, "friend", false, Profile(1, tilt)}});
    EXPECT_EQ(TagLayout(host, 40), FakeBrawl::CONTROLS);
    EXPECT_EQ(host.bytes, joiner.bytes);
  }
  host.W32(FakeBrawl::SCENE, 0x806ff300);
  host.recording = true;
  ApplyNameTags(host, {{1, "friend", true, Profile(1, tilt)}});
  EXPECT_TRUE(host.written.empty());
}

class OrcaUXHostUI : public OrcaUXOverlayDraw
{
protected:
  void SetUp() override
  {
    OrcaUXOverlayDraw::SetUp();
    const char* was = std::getenv("SUBSPACE_STDIO");
    m_had_stdio = was != nullptr;
    m_stdio = was ? was : "";
    SetStdio("1");
    Orca::Bridge::ResetForTests();
    Orca::Bridge::SetWriter(+[](const std::string&) {});
    m_lines = std::make_unique<StatusLines>("");
    ASSERT_TRUE(m_lines->Ok());
    AppOrb::Current().Apply({}, AppOrb::NowMs());
    OverlayKey::Keys().Cancel();
    LocalPadStopped();
    Pad(0);
    m_lines->Take();
  }

  void TearDown() override
  {
    LocalPadStopped();
    Pad(0);
    AppOrb::Current().Apply({}, AppOrb::NowMs());
    m_lines.reset();
    Orca::Bridge::SetWriter(nullptr);
    Orca::Bridge::ResetForTests();
    SetStdio(m_had_stdio ? m_stdio.c_str() : nullptr);
    OrcaUXOverlayDraw::TearDown();
  }

  static void SetStdio(const char* value)
  {
#ifdef _WIN32
    _putenv_s("SUBSPACE_STDIO", value ? value : "");
#else
    if (value)
      setenv("SUBSPACE_STDIO", value, 1);
    else
      unsetenv("SUBSPACE_STDIO");
#endif
  }

  static u16 Pad(u16 buttons)
  {
    GCPadStatus pad{};
    pad.button = buttons;
    return FilterLocalPad(pad, true, true).button;
  }

  static int HostVertices()
  {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(640, 480);
    io.DeltaTime = 1.0f / 60;
    ImGui::NewFrame();
    AppOrb::DrawHostControls(ImGui::GetForegroundDrawList(), AppOrb::HostView(AppOrb::NowMs()));
    ImGui::Render();
    return ImGui::GetDrawData()->TotalVtxCount;
  }

  std::unique_ptr<StatusLines> m_lines;
  bool m_had_stdio = false;
  std::string m_stdio;
};

TEST_F(OrcaUXHostUI, WithoutAgreementTheOrbAndShortcutKeepTheirLegacyPaths)
{
  using namespace AppOrb;
  ASSERT_TRUE(Orca::Bridge::Ready());
  EXPECT_TRUE(Orca::Status::OfferedCaps().ends_with(" record hostui coseats qdoubles rdoubles"));
  for (const char* caps : {"", "orb", "orb talk"})
  {
    SCOPED_TRACE(caps);
    Orca::Status::SetAppCaps(caps);
    ASSERT_FALSE(HostEnabled());
    Current().Apply({}, NowMs());
    EXPECT_FALSE(HostView(NowMs()).on);
    Current().Apply(*ParseOrb("orb 16 16 40 1 0 0"), NowMs());
    EXPECT_TRUE(HostView(NowMs()).on);
    EXPECT_TRUE(Current().Hit(36, 36));
    EXPECT_FALSE(HitControls(36, 36));
    EXPECT_FALSE(HitControls(84, 36));
    EXPECT_FALSE(ClickControls(36, 36));
    ToggleHostMenu();
    for (const HostKey key : {HostKey::Feedback, HostKey::Menu})
      EXPECT_FALSE(HandleHostKey(key));
    FeedbackRequests().FeedbackFinished(NowMs() - 5001);
    ActivateHost(HostAction::Feedback);
    EXPECT_TRUE(FeedbackRequests().FeedbackDue(NowMs()))
        << "No screenshot request or cooldown without hostui";
    FeedbackRequests().FeedbackFinished(-5000);
    EXPECT_EQ(m_lines->Take(), "");
    EXPECT_EQ(HostVertices(), 0);

    EXPECT_EQ(Pad(PAD_BUTTON_UP), PAD_BUTTON_UP);
    EXPECT_EQ(Pad(PAD_BUTTON_UP | PAD_BUTTON_START), 0);
    EXPECT_EQ(Pad(0), 0);
    const bool talk = Orca::Status::Cap(Orca::Status::TALK_CAP);
    EXPECT_EQ(m_lines->Take(), talk ? "orca overlay down pad\norca overlay up pad\n" :
                                    "orca orb\n");
    OverlayKey::Press keys(OverlayKey::Source::Keys);
    EXPECT_EQ(keys.Down(talk), talk ? "orca overlay down keys" : "orca orb");
    EXPECT_EQ(keys.Up(), talk ? "orca overlay up keys" : "");
  }
}

TEST_F(OrcaUXHostUI, LogoAndHostMenuEmitExactlyThePagesShiftTabTapOnTheChosenChannel)
{
  using namespace AppOrb;
  for (const char* caps : {"orb hostui", "orb talk hostui"})
  {
    SCOPED_TRACE(caps);
    Orca::Status::SetAppCaps(caps);
    ASSERT_TRUE(HostEnabled());
    EXPECT_TRUE(HostView(NowMs()).on);
    EXPECT_FALSE(Current().At(NowMs()).on);
    EXPECT_TRUE(HitControls(36, 36));
    EXPECT_TRUE(HitControls(84, 36));
    EXPECT_FALSE(HitControls(100, 80));
    const int vertices = HostVertices();
    EXPECT_GT(vertices, 0);
    const bool talk = Orca::Status::Cap(Orca::Status::TALK_CAP);
    const std::string expected = talk ? "orca overlay down keys\norca overlay up keys\n" :
                                        "orca orb\n";
    for (const bool embedded : {false, true})
    {
      SCOPED_TRACE(embedded);
      static std::string embedded_lines;
      embedded_lines.clear();
      const OverlayKey::Output output = embedded ? +[](std::string_view line) {
        embedded_lines += std::string(line) + "\n";
      } : nullptr;
      const auto take = [&] {
        const std::string host_lines = m_lines->Take();
        if (!embedded)
          return host_lines;
        EXPECT_TRUE(host_lines.empty());
        return std::exchange(embedded_lines, {});
      };
      OverlayKey::Emit(OverlayKey::Keys().Down(talk), output);
      OverlayKey::Emit(OverlayKey::Keys().Up(), output);
      EXPECT_EQ(take(), expected);
      EXPECT_TRUE(ClickControls(36, 36, output));
      EXPECT_EQ(take(), expected);
      EXPECT_FALSE(OverlayKey::Keys().IsDown());
      ToggleHostMenu(output);
      EXPECT_EQ(take(), expected);
      EXPECT_TRUE(HandleHostKey(HostKey::Menu, output));
      EXPECT_EQ(take(), expected);
      EXPECT_FALSE(ClickControls(100, 80, output));
      EXPECT_EQ(take(), "");
      EXPECT_FALSE(HitControls(100, 80));
      EXPECT_EQ(HostVertices(), vertices);
    }
  }
}

TEST_F(OrcaUXHostUI, HostuiPadShortcutUsesThePagePressAndRelease)
{
  for (const char* caps : {"orb hostui", "orb talk hostui"})
  {
    SCOPED_TRACE(caps);
    Orca::Status::SetAppCaps(caps);
    ASSERT_TRUE(AppOrb::HostEnabled());
    EXPECT_EQ(Pad(PAD_BUTTON_UP), PAD_BUTTON_UP);
    EXPECT_EQ(Pad(PAD_BUTTON_UP | PAD_BUTTON_START), 0);
    const bool talk = Orca::Status::Cap(Orca::Status::TALK_CAP);
    EXPECT_EQ(m_lines->Take(), talk ? "orca overlay down pad\n" : "orca orb\n");
    EXPECT_EQ(Pad(PAD_BUTTON_START), 0);
    EXPECT_EQ(m_lines->Take(), "");
    EXPECT_EQ(Pad(0), 0);
    EXPECT_EQ(m_lines->Take(), talk ? "orca overlay up pad\n" : "");
    EXPECT_FALSE(OverlayKey::Pad().IsDown());
    EXPECT_EQ(Pad(PAD_BUTTON_A | PAD_BUTTON_DOWN), PAD_BUTTON_A | PAD_BUTTON_DOWN);
    EXPECT_EQ(Pad(PAD_BUTTON_B), PAD_BUTTON_B);
    EXPECT_EQ(Pad(0), 0);
    EXPECT_EQ(m_lines->Take(), "");
  }
}

TEST_F(OrcaUXHostUI, NativeDeltaDoesNotLightBlinkOrOpenTheOrb)
{
  using namespace AppOrb;
  Orca::Status::SetAppCaps("orb talk hostui");
  ASSERT_TRUE(HostEnabled());
  Current().Apply(*ParseOrb("orb 16 16 40 1 0 0"), NowMs());
  UpdateControlsPointer(84, 36, false);
  auto view = HostView(NowMs());
  EXPECT_TRUE(view.feedback.hovered);
  EXPECT_FALSE(view.orb.hovered);
  EXPECT_FALSE(view.orb.pressed);
  EXPECT_FALSE(view.place.lit);
  EXPECT_LT(view.ring, 0);
  UpdateControlsPointer(84, 36, true);
  EXPECT_TRUE(HostView(NowMs()).feedback.pressed);
  ScreenshotRequests().SetRenderer(nullptr);
  FeedbackRequests().FeedbackFinished(NowMs() - 5001);
  EXPECT_TRUE(ClickControls(84, 36));
  EXPECT_FALSE(FeedbackRequests().FeedbackDue(NowMs()));
  EXPECT_EQ(m_lines->Take(), "");
  UpdateControlsPointer(84, 36, false);
  UpdateControlsPointer(36, 36, false);
  view = HostView(NowMs());
  EXPECT_TRUE(view.orb.hovered);
  EXPECT_TRUE(view.place.lit);
  EXPECT_FALSE(view.feedback.hovered);
  EXPECT_FALSE(view.feedback.pressed);
  UpdateControlsPointer(36, 36, true);
  EXPECT_TRUE(HostView(NowMs()).orb.pressed);
  FeedbackRequests().FeedbackFinished(NowMs() - 5001);
  EXPECT_TRUE(ClickControls(36, 36));
  EXPECT_TRUE(FeedbackRequests().FeedbackDue(NowMs()));
  FeedbackRequests().FeedbackFinished(NowMs() - 5001);
  EXPECT_EQ(m_lines->Take(), "orca overlay down keys\norca overlay up keys\n");
  UpdateControlsPointer(36, 36, false);
  EXPECT_TRUE(ClickControls(84, 36));
  EXPECT_FALSE(FeedbackRequests().FeedbackDue(NowMs()));
  EXPECT_TRUE(ClickControls(36, 36));
  EXPECT_EQ(m_lines->Take(), "orca overlay down keys\norca overlay up keys\n");
  FeedbackRequests().FeedbackFinished(NowMs() - 5001);
  EXPECT_TRUE(HandleHostKey(HostKey::Feedback));
  EXPECT_FALSE(FeedbackRequests().FeedbackDue(NowMs()));
  EXPECT_EQ(m_lines->Take(), "");
  EXPECT_FALSE(HitControls(64, 16));
  EXPECT_FALSE(HitControls(60, 36));
  Orca::Status::SetAppCaps("talk");
  UpdateControlsPointer(84, 36, true);
  EXPECT_FALSE(Current().At(NowMs()).feedback.pressed);
  FeedbackRequests().FeedbackFinished(-5000);
}

TEST_F(OrcaUXHostUI, DeltaDrawsARedDiscAndWhiteOutlineWithIndependentAlphaAndPressScale)
{
  using namespace AppOrb;
  Orca::Status::SetAppCaps("orb hostui");
  for (const float k : {1.0f, 2.0f})
  {
    Current().Apply({Command::Type::Place, {16, 16, 40 * k, true, false, 0}}, NowMs());
    for (const int target : {-1, 0, 1})
    {
      for (const bool down : {false, true})
      {
        Current().SetPointer(-1, false);
        Current().SetPointer(target, down);
        ASSERT_GT(HostVertices(), 0);
        const int expected_alpha = target == 1 ? 255 : 153;
        const float scale = target == 1 && down ? 0.94f : 1.0f;
        const ImVec2 centre(16 + 68 * k, 16 + 20 * k);
        int red = 0, white = 0;
        float left = centre.x, right = centre.x;
        for (const ImDrawList* list : ImGui::GetDrawData()->CmdLists)
        {
          for (const ImDrawVert& v : list->VtxBuffer)
          {
            if ((v.col >> IM_COL32_A_SHIFT) == 0)
              continue;
            const ImU32 rgb = v.col & ~IM_COL32_A_MASK;
            EXPECT_TRUE(rgb == (IM_COL32(255, 0, 0, 0)) ||
                        rgb == (IM_COL32_WHITE & ~IM_COL32_A_MASK));
            EXPECT_EQ(static_cast<int>(v.col >> IM_COL32_A_SHIFT), expected_alpha);
            if (rgb == IM_COL32(255, 0, 0, 0))
            {
              ++red;
              left = std::min(left, v.pos.x);
              right = std::max(right, v.pos.x);
            }
            else
            {
              ++white;
              EXPECT_LE(std::abs(v.pos.x - centre.x), 11.0f * k * scale);
              EXPECT_LE(std::abs(v.pos.y - centre.y), 11.0f * k * scale);
            }
          }
        }
        EXPECT_GT(red, 0);
        EXPECT_GT(white, 0);
        EXPECT_NEAR(left, centre.x - 20 * k * scale, 1.0f);
        EXPECT_NEAR(right, centre.x + 20 * k * scale, 1.0f);
        const auto white_at = [](ImVec2 point) {
          const auto cross = [](ImVec2 a, ImVec2 b, ImVec2 c) {
            return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
          };
          for (const ImDrawList* list : ImGui::GetDrawData()->CmdLists)
          {
            for (int i = 0; i + 2 < list->IdxBuffer.Size; i += 3)
            {
              const auto& a = list->VtxBuffer[list->IdxBuffer[i]];
              const auto& b = list->VtxBuffer[list->IdxBuffer[i + 1]];
              const auto& c = list->VtxBuffer[list->IdxBuffer[i + 2]];
              if ((a.col & ~IM_COL32_A_MASK) != (IM_COL32_WHITE & ~IM_COL32_A_MASK) ||
                  std::abs(cross(a.pos, b.pos, c.pos)) < 0.0001f)
                continue;
              const float ab = cross(a.pos, b.pos, point), bc = cross(b.pos, c.pos, point),
                          ca = cross(c.pos, a.pos, point);
              if ((ab >= 0 && bc >= 0 && ca >= 0) || (ab <= 0 && bc <= 0 && ca <= 0))
                return true;
            }
          }
          return false;
        };
        EXPECT_FALSE(white_at(centre));
        EXPECT_TRUE(white_at(ImVec2(centre.x, centre.y - 8.4f * k * scale)));
        EXPECT_TRUE(white_at(ImVec2(centre.x, centre.y + 7.6f * k * scale)));
      }
    }
  }
}

TEST_F(OrcaUXHostUI, LaterCapsReplaceTheAgreementIncludingDuringAShortcutPress)
{
  using namespace AppOrb;
  Orca::Status::SetAppCaps("talk");
  Pad(PAD_BUTTON_UP | PAD_BUTTON_START);
  EXPECT_EQ(m_lines->Take(), "orca overlay down pad\n");
  Orca::Status::SetAppCaps("talk hostui");
  ASSERT_TRUE(HostEnabled());
  Pad(0);
  EXPECT_EQ(m_lines->Take(), "orca overlay up pad\n");
  Pad(PAD_BUTTON_UP | PAD_BUTTON_START);
  EXPECT_EQ(m_lines->Take(), "orca overlay down pad\n");
  Orca::Status::SetAppCaps("talk");
  EXPECT_FALSE(HostEnabled());
  EXPECT_FALSE(HostView(NowMs()).on);
  EXPECT_FALSE(HitControls(36, 36));
  EXPECT_FALSE(ClickControls(36, 36));
  EXPECT_EQ(HostVertices(), 0);
  ToggleHostMenu();
  ActivateHost(HostAction::Feedback);
  Pad(0);
  EXPECT_EQ(m_lines->Take(), "orca overlay up pad\n");
  Pad(PAD_BUTTON_UP | PAD_BUTTON_START);
  Pad(0);
  EXPECT_EQ(m_lines->Take(), "orca overlay down pad\norca overlay up pad\n");
  Orca::Status::SetAppCaps("talk hostui");
  ASSERT_TRUE(HostEnabled());
  ToggleHostMenu();
  EXPECT_EQ(m_lines->Take(), "orca overlay down keys\norca overlay up keys\n");
  Orca::Status::SetAppCaps("talk");
  Orca::Status::SetAppCaps("talk hostui");
  EXPECT_FALSE(HitControls(100, 80));
  EXPECT_EQ(m_lines->Take(), "");
}

TEST(OrcaUXAppOrb, FeedbackWaitsForCompletionAndThenFiveSeconds)
{
  using namespace Orca::UX::AppOrb;
  FeedbackCooldown feedback;
  ASSERT_TRUE(feedback.FeedbackDue(0));
  EXPECT_FALSE(feedback.FeedbackDue(100000));
  feedback.FeedbackFinished(100000);
  EXPECT_FALSE(feedback.FeedbackDue(104999));
  EXPECT_TRUE(feedback.FeedbackDue(105000));
  feedback.FeedbackFinished(105001);
  EXPECT_FALSE(feedback.FeedbackDue(110000));
}

TEST(OrcaUXAppOrb, FeedbackPublishesACompletePngBeforeReturningItsLine)
{
  using namespace Orca::UX::AppOrb;
  const std::string dir = File::CreateTempDir();
  ASSERT_FALSE(dir.empty());
  Common::ScopeGuard cleanup([&] { File::DeleteDirRecursively(dir); });
  const std::string directory = dir + "/", name = "feedback-test.png";
  const std::string final = directory + name, temp = final + ".tmp";
  const std::array<u8, 4> pixel{255, 64, 32, 255};
  ASSERT_TRUE(Common::ConvertRGBAToRGBAndSavePNG(temp, pixel.data(), 1, 1, 4, 6));
  EXPECT_FALSE(File::Exists(final));
  EXPECT_EQ(PublishFeedback(directory, name, true), "orca feedback feedback-test.png");
  EXPECT_FALSE(File::Exists(temp));
  File::IOFile png(final, "rb");
  std::array<u8, 8> signature{};
  ASSERT_TRUE(png.ReadBytes(signature.data(), signature.size()));
  EXPECT_EQ(signature, (std::array<u8, 8>{137, 80, 78, 71, 13, 10, 26, 10}));
  EXPECT_GT(png.GetSize(), signature.size());
  ASSERT_TRUE(png.Close());
  ASSERT_TRUE(File::Delete(final));
  ASSERT_TRUE(Common::ConvertRGBAToRGBAndSavePNG(temp, pixel.data(), 1, 1, 4, 6));
  EXPECT_FALSE(PublishFeedback(directory, name, false));
  EXPECT_FALSE(File::Exists(temp));
  EXPECT_FALSE(File::Exists(final));
  EXPECT_FALSE(PublishFeedback(directory, name, true));
  for (const std::string& invalid : std::array<std::string, 5>{
           "../frame.png", "dir/frame.png", "a b.png", ".png", std::string(65, 'a') + ".png"})
    EXPECT_FALSE(PublishFeedback(directory, invalid, true));
}

TEST(OrcaUXAppOrb, FeedbackRejectsAnUnavailableRenderer)
{
  using namespace Orca::UX::AppOrb;
  using namespace Common::Log;
  class CaptureLog final : public LogListener
  {
  public:
    explicit CaptureLog(std::string& text) : m_text(text) {}
    void Log(LogLevel, const char* message) override { m_text += message; }

  private:
    std::string& m_text;
  };
  class MemoryLayer final : public Config::ConfigLayerLoader
  {
  public:
    MemoryLayer() : ConfigLayerLoader(Config::LayerType::Base) {}
    void Load(Config::Layer*) override {}
    void Save(Config::Layer*) override {}
  };
  const std::string dir = File::CreateTempDir();
  ASSERT_FALSE(dir.empty());
  const std::string old_user = File::GetUserPath(D_USER_IDX);
  const char* old_stdio = std::getenv("SUBSPACE_STDIO");
  const bool had_stdio = old_stdio != nullptr;
  const std::string saved_stdio = old_stdio ? old_stdio : "";
  std::string logs;
  const auto set_stdio = [](const char* value) {
#ifdef _WIN32
    _putenv_s("SUBSPACE_STDIO", value ? value : "");
#else
    if (value)
      setenv("SUBSPACE_STDIO", value, 1);
    else
      unsetenv("SUBSPACE_STDIO");
#endif
  };
  Common::ScopeGuard cleanup([&] {
    LogManager::Shutdown();
    Config::Shutdown();
    Orca::Bridge::SetWriter(nullptr);
    Orca::Bridge::ResetForTests();
    set_stdio(had_stdio ? saved_stdio.c_str() : nullptr);
    File::SetUserPath(D_USER_IDX, old_user);
    File::DeleteDirRecursively(dir);
  });
  File::SetUserPath(D_USER_IDX, dir + "/");
  Config::Init();
  Config::AddLayer(std::make_unique<MemoryLayer>());
  Config::SetCurrent(LOGGER_VERBOSITY, LogLevel::LWARNING);
  LogManager::Init();
  auto& logger = *LogManager::GetInstance();
  logger.SetEnable(LogType::VIDEO, true);
  logger.EnableListener(LogListener::CONSOLE_LISTENER, false);
  logger.RegisterListener(LogListener::LOG_WINDOW_LISTENER, std::make_unique<CaptureLog>(logs));
  logger.EnableListener(LogListener::LOG_WINDOW_LISTENER, true);
  set_stdio("1");
  Orca::Bridge::ResetForTests();
  StatusLines lines("hostui");
  ASSERT_TRUE(lines.Ok());
  EXPECT_FALSE(HostEnabled());
  ActivateHost(HostAction::Feedback);
  EXPECT_EQ(lines.Take(), "");
  EXPECT_NE(logs.find("Feedback request rejected: host controls unavailable"), std::string::npos);
  Orca::Bridge::SetWriter(+[](const std::string&) {});
  ASSERT_TRUE(HostEnabled());
  ASSERT_FALSE(g_frame_dumper);
  ActivateHost(HostAction::Feedback);
  EXPECT_EQ(lines.Take(), "");
  EXPECT_NE(logs.find("Feedback request rejected: renderer unavailable"), std::string::npos);
  EXPECT_TRUE(std::filesystem::is_empty(dir));
}

TEST(OrcaUXAppOrb, FeedbackRejectsStartingStoppingAndFailedRenderers)
{
  const std::string dir = File::CreateTempDir();
  ASSERT_FALSE(dir.empty());
  Common::ScopeGuard cleanup([&] { File::DeleteDirRecursively(dir); });
  const std::string path = dir + "/feedback.png.tmp";
  ScreenshotRequestGate requests;
  int completions = 0;
  const auto completed = [&](bool saved) {
    EXPECT_FALSE(saved);
    ++completions;
  };
  EXPECT_FALSE(requests.Request(Core::State::Running, path, completed));
  auto dumper = std::make_unique<FrameDumper>();
  EXPECT_FALSE(requests.Request(Core::State::Running, path, completed));
  dumper.reset();
  EXPECT_FALSE(requests.Request(Core::State::Running, path, completed));

  dumper = std::make_unique<FrameDumper>();
  requests.SetRenderer(dumper.get());
  for (const auto state : {Core::State::Uninitialized, Core::State::Starting, Core::State::Stopping})
    EXPECT_FALSE(requests.Request(state, path, completed));
  EXPECT_EQ(completions, 0);
  ASSERT_TRUE(requests.Request(Core::State::Paused, path, completed));
  EXPECT_FALSE(requests.Request(Core::State::Running, path, completed));
  requests.SetRenderer(nullptr);
  EXPECT_FALSE(requests.Request(Core::State::Running, path, completed));
  dumper.reset();
  EXPECT_EQ(completions, 1);
  EXPECT_TRUE(std::filesystem::is_empty(dir));
}

TEST(OrcaUXAppOrb, FeedbackRequestsCanRaceRendererTeardown)
{
  const std::string dir = File::CreateTempDir();
  ASSERT_FALSE(dir.empty());
  Common::ScopeGuard cleanup([&] { File::DeleteDirRecursively(dir); });
  ScreenshotRequestGate requests;
  const std::string path = dir + "/feedback.png.tmp";
  std::atomic<int> accepted{0}, cancelled{0}, attempts{0};
  std::atomic<bool> done{false};
  const auto completed = [&](bool saved) {
    EXPECT_FALSE(saved);
    ++cancelled;
  };
  auto dumper = std::make_unique<FrameDumper>();
  requests.SetRenderer(dumper.get());
  ASSERT_TRUE(requests.Request(Core::State::Running, path, completed));
  ++accepted;
  std::thread requester([&] {
    while (!done.load())
    {
      if (requests.Request(Core::State::Running, path, completed))
        ++accepted;
      ++attempts;
      std::this_thread::yield();
    }
  });
  while (attempts.load() == 0)
    std::this_thread::yield();
  for (int iteration = 0; iteration < 100; ++iteration)
  {
    requests.SetRenderer(nullptr);
    dumper.reset();
    dumper = std::make_unique<FrameDumper>();
    requests.SetRenderer(dumper.get());
    std::this_thread::yield();
  }
  requests.SetRenderer(nullptr);
  dumper.reset();
  done = true;
  requester.join();
  EXPECT_GT(attempts.load(), 0);
  EXPECT_EQ(accepted.load(), cancelled.load());
  EXPECT_TRUE(std::filesystem::is_empty(dir));
}

TEST(OrcaUXControllers, OneOrdinalMatchesRecordedPreCoSeatsChoices)
{
  const std::array<std::string, 4> snapshots{{
      Event(false, "", ""),
      Event(false, "", PadJson(3, "standard", "[1,0,0,0]", Buttons17(0))),
      Event(true, FourPorts(PortJson(0, 2, "wired", 0x101)),
            PadJson(0, "standard", "[0,-1,0,0]", Buttons17(1))),
      Event(true, FourPorts(), PadJson(2, "standard", "[0,0,0,0]", Buttons17(2)))}};
  const std::array<std::string_view, 32> expected{{
      "-",
      "-",
      "-",
      "-",
      "-",
      "-",
      "-",
      "-",
      "1,3,false,256,255,128,128,128,0,0,0,0,0,true",
      "1,3,true,0,128,128,128,128,0,0,0,0,0,true",
      "1,3,false,256,255,128,128,128,0,0,0,0,0,true",
      "1,3,true,0,128,128,128,128,0,0,0,0,0,true",
      "1,3,false,256,255,128,128,128,0,0,0,0,0,true",
      "1,3,true,0,128,128,128,128,0,0,0,0,0,true",
      "1,3,true,0,128,128,128,128,0,0,0,0,0,true",
      "1,3,true,0,128,128,128,128,0,0,0,0,0,true",
      "0,0,false,4352,128,128,128,128,0,0,0,0,0,true",
      "0,0,true,0,128,128,128,128,0,0,0,0,0,true",
      "0,0,false,4352,128,128,128,128,0,0,0,0,0,true",
      "0,0,true,0,128,128,128,128,0,0,0,0,0,true",
      "0,0,true,0,128,128,128,128,0,0,0,0,0,true",
      "0,0,true,0,128,128,128,128,0,0,0,0,0,true",
      "0,0,true,0,128,128,128,128,0,0,0,0,0,true",
      "0,0,true,0,128,128,128,128,0,0,0,0,0,true",
      "1,2,false,512,128,128,128,128,0,0,0,0,0,true",
      "1,2,true,0,128,128,128,128,0,0,0,0,0,true",
      "1,2,false,512,128,128,128,128,0,0,0,0,0,true",
      "1,2,true,0,128,128,128,128,0,0,0,0,0,true",
      "1,2,false,512,128,128,128,128,0,0,0,0,0,true",
      "1,2,true,0,128,128,128,128,0,0,0,0,0,true",
      "1,2,true,0,128,128,128,128,0,0,0,0,0,true",
      "1,2,true,0,128,128,128,128,0,0,0,0,0,true",
  }};
  const auto record = [](const std::optional<LocalChoice>& choice) {
    if (!choice)
      return std::string("-");
    const auto& p = choice->pad;
    return fmt::format("{},{},{},{},{},{},{},{},{},{},{},{},{},{}",
        static_cast<int>(choice->source), choice->index, choice->neutral, p.button,
        p.stickX, p.stickY, p.substickX, p.substickY, p.triggerLeft, p.triggerRight,
        p.analogA, p.analogB, p.switches, p.isConnected);
  };
  size_t index = 0;
  for (const auto& json : snapshots)
  for (const double age : {0.0, 100.0, 250.0, 251.0})
  for (const bool suspended : {false, true})
  {
    auto snapshot = ParseSnapshotEvent(json);
    ASSERT_TRUE(snapshot);
    snapshot->suspended = suspended;
    const auto choices = ChooseLocalPads(*snapshot, age, 1);
    EXPECT_FALSE(choices[1]);
    const auto actual = record(choices[0]);
    EXPECT_EQ(actual, expected[index++]);
  }
}

TEST(OrcaUXControllers, MultipleOrdinalsRequireExplicitUniqueDevices)
{
  auto snapshot = ParseSnapshotEvent(Event(true, FourPorts(PortJson(0, 1, "wired", 1)),
      PadJson(0, "standard", "[0,-1,0,0]", Buttons17(1))));
  ASSERT_TRUE(snapshot);
  auto choices = ChooseLocalPads(*snapshot, 0, 2);
  EXPECT_FALSE(choices[0]);
  ASSERT_TRUE(choices[1]);
  EXPECT_EQ(choices[1]->source, PadSource::Adapter);
  snapshot->pads[0].seat = 0;
  choices = ChooseLocalPads(*snapshot, 0, 2);
  ASSERT_TRUE(choices[0]);
  EXPECT_EQ(choices[0]->source, PadSource::Gamepad);
  EXPECT_EQ(choices[0]->pad.button, PAD_BUTTON_X);
  EXPECT_EQ(choices[1]->pad.button, PAD_BUTTON_A);
  for (const bool suspended : {false, true})
  {
    snapshot->suspended = suspended;
    choices = ChooseLocalPads(*snapshot, suspended ? 0 : 251, 2);
    for (const auto& choice : choices)
    {
      ASSERT_TRUE(choice);
      ExpectSame(choice->pad, NeutralPad());
      EXPECT_TRUE(choice->neutral);
    }
  }
  snapshot->suspended = false;
  snapshot->owned = false;
  auto duplicate = snapshot->pads[0];
  duplicate.seat = 1;
  snapshot->pads.push_back(duplicate);
  choices = ChooseLocalPads(*snapshot, 0, 2);
  EXPECT_TRUE(choices[0]);
  EXPECT_FALSE(choices[1]);
}

TEST(OrcaUXControllers, ParsesExplicitPadSeatsAndIgnoresInvalidAssignments)
{
  const auto assigned = [](const char* seat) {
    auto pad = PadJson(3, "standard", "[0,0,0,0]", Buttons17(0));
    pad.insert(1, std::string("\"seat\":") + seat + ",");
    return Event(false, "", pad);
  };
  const auto snapshot = ParseSnapshotEvent(assigned("1"));
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(snapshot->pads[0].seat, 1);
  for (const char* bad : {"-1", "4", "0.5", "null", "true"})
  {
    const auto unassigned = ParseSnapshotEvent(assigned(bad));
    ASSERT_TRUE(unassigned);
    ASSERT_EQ(unassigned->pads.size(), 1u);
    EXPECT_FALSE(unassigned->pads[0].seat);
    ExpectSame(MapStandardPad(unassigned->pads[0]), MapStandardPad(snapshot->pads[0]));
  }
}

TEST(OrcaUXControllers, ExplicitSeatsIgnorePadIndexAndOrder)
{
  Snapshot snapshot;
  StandardPad first;
  first.index = 7;
  first.seat = 0;
  first.axes[0] = 1;
  StandardPad second;
  second.index = 0;
  second.seat = 1;
  second.axes[0] = -1;
  snapshot.pads = {second, first};
  const auto before = ChooseLocalPads(snapshot, 0, 2);
  snapshot.pads[0].index = 5;
  snapshot.pads[1].index = 0;
  std::reverse(snapshot.pads.begin(), snapshot.pads.end());
  const auto after = ChooseLocalPads(snapshot, 0, 2);
  for (int ordinal = 0; ordinal < MAX_LOCAL; ++ordinal)
  {
    ASSERT_TRUE(before[ordinal]);
    ASSERT_TRUE(after[ordinal]);
    ExpectSame(before[ordinal]->pad, after[ordinal]->pad);
  }
  EXPECT_EQ(after[0]->pad.stickX, 255);
  EXPECT_EQ(after[1]->pad.stickX, 1);
}

TEST(OrcaUXOrbCombo, CoSeatShortcutHasOnlyTheRecordedMask)
{
  OrbCombo combo;
  GCPadStatus pad = NeutralPad();
  pad.button = PAD_BUTTON_UP | PAD_BUTTON_START | PAD_BUTTON_A;
  testing::internal::CaptureStdout();
  EXPECT_EQ(FilterCoSeatPad(pad, combo, true, true).button, PAD_BUTTON_A);
  pad.button = PAD_BUTTON_START | PAD_BUTTON_A;
  EXPECT_EQ(FilterCoSeatPad(pad, combo, true, true).button, PAD_BUTTON_A);
  pad.button = 0;
  EXPECT_EQ(FilterCoSeatPad(pad, combo, true, true).button, 0);
  pad.button = PAD_BUTTON_START;
  EXPECT_EQ(FilterCoSeatPad(pad, combo, true, true).button, PAD_BUTTON_START);
  EXPECT_TRUE(testing::internal::GetCapturedStdout().empty());
}

TEST(OrcaUXControllers, InvalidOptionalSeatKeepsTheSnapshotAndItsOtherAssignments)
{
  for (const char* value : {"null", "-1", "4", "1.5", "false", "\"none\""})
  {
    auto unassigned = PadJson(1, "standard", "[1,0,0,0]", Buttons17(0));
    unassigned.insert(1, std::string("\"seat\":") + value + ",");
    auto assigned = PadJson(3, "standard", "[0,0,0,0]", Buttons17(1));
    assigned.insert(1, "\"seat\":0,");
    const auto snapshot = ParseSnapshotEvent(Event(false, "", unassigned + "," + assigned));
    ASSERT_TRUE(snapshot) << value;
    ASSERT_EQ(snapshot->pads.size(), 2u);
    EXPECT_FALSE(snapshot->pads[0].seat);
    const auto single = ChooseLocalPads(*snapshot, 0, 1);
    ASSERT_TRUE(single[0]);
    EXPECT_EQ(single[0]->index, 1);
    const auto multiple = ChooseLocalPads(*snapshot, 0, 2);
    ASSERT_TRUE(multiple[0]);
    EXPECT_EQ(multiple[0]->index, 3);
    EXPECT_FALSE(multiple[1]);
  }
}
