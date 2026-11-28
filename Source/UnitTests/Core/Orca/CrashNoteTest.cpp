// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Core/Orca/CrashNote.h"

#ifdef _WIN32
#include <thread>
#include <windows.h>

#include "Core/Core.h"
#endif

namespace
{
constexpr std::string_view ALERT =
    "Invalid write to 0x000002c8, PC = 0x801e17bc.\n\nThe game probably would have crashed on real "
    "hardware. Enable MMU in advanced settings to accurately emulate game crashes.";

class RefusingStream
{
public:
  RefusingStream()
  {
    m_dir = File::CreateTempDir();
    const std::string path = m_dir + "/refuses.txt";
    File::WriteStringToFile(path, "");
    m_file = std::fopen(path.c_str(), "rb");
    if (m_file)
      std::setvbuf(m_file, nullptr, _IONBF, 0);
  }
  ~RefusingStream()
  {
    if (m_file)
      std::fclose(m_file);
    File::DeleteDirRecursively(m_dir);
  }
  std::FILE* get() const { return m_file; }

private:
  std::string m_dir;
  std::FILE* m_file = nullptr;
};

void Put32(std::vector<u8>& image, size_t at, s32 value)
{
  std::memcpy(image.data() + at, &value, sizeof(value));
}

void PutName(std::vector<u8>& image, size_t at, std::string_view name)
{
  std::memcpy(image.data() + at + 2 * sizeof(void*), name.data(), name.size());
}
}

TEST(OrcaCrashNote, AlertToAStreamThatRefusesWritesDoesNotThrow)
{
  RefusingStream stream;
  ASSERT_NE(stream.get(), nullptr);
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
  EXPECT_THROW(fmt::print(stream.get(), "{}\n", ALERT), std::system_error);
#else
  EXPECT_DEATH(fmt::print(stream.get(), "{}\n", ALERT), "cannot write to file");
#endif
  EXPECT_FALSE(Orca::CrashNote::WriteLine(stream.get(), ALERT));
  EXPECT_FALSE(Orca::CrashNote::WriteLine(nullptr, ALERT));
}

TEST(OrcaCrashNote, WriteLineWritesTheTextAndANewline)
{
  const std::string dir = File::CreateTempDir();
  const std::string path = dir + "/alert.txt";
  std::FILE* file = std::fopen(path.c_str(), "wb");
  ASSERT_NE(file, nullptr);
  EXPECT_TRUE(Orca::CrashNote::WriteLine(file, ALERT));
  std::fclose(file);
  std::string text;
  ASSERT_TRUE(File::ReadFileToString(path, text));
  EXPECT_EQ(text, std::string(ALERT) + "\n");
  File::DeleteDirRecursively(dir);
}

TEST(OrcaCrashNote, TypeNames)
{
  using Orca::CrashNote::TypeName;
  EXPECT_EQ(TypeName(".?AVsystem_error@std@@"), "std::system_error");
  EXPECT_EQ(TypeName(".?AVformat_error@v12@fmt@@"), "fmt::v12::format_error");
  EXPECT_EQ(TypeName(".?AVhresult_error@winrt@@"), "winrt::hresult_error");
  EXPECT_EQ(TypeName(".?AUplain@@"), "plain");
  EXPECT_EQ(TypeName(".?AV?$basic_string@DU?$char_traits@D@std@@@std@@"),
            ".?AV?$basic_string@DU?$char_traits@D@std@@@std@@");
  EXPECT_EQ(TypeName(".H"), ".H");
  EXPECT_EQ(TypeName(".?AV@@"), ".?AV@@");
  EXPECT_EQ(TypeName(".?AVa@@b@@"), ".?AVa@@b@@");
}

TEST(OrcaCrashNote, ReadThrowInfoListsTheTypesAndFindsStdException)
{
  std::vector<u8> image(0x200);
  Put32(image, 0x0C, 0x20);
  Put32(image, 0x20, 3);
  Put32(image, 0x24, 0x40);
  Put32(image, 0x28, 0x60);
  Put32(image, 0x2C, 0x80);
  const std::pair<size_t, size_t> types[] = {{0x40, 0x100}, {0x60, 0x140}, {0x80, 0x180}};
  for (const auto& [catchable, descriptor] : types)
  {
    Put32(image, catchable + 4, static_cast<s32>(descriptor));
    Put32(image, catchable + 8, catchable == 0x80 ? 0x10 : 0);
    Put32(image, catchable + 12, -1);
  }
  PutName(image, 0x100, ".?AVsystem_error@std@@");
  PutName(image, 0x140, ".?AVruntime_error@std@@");
  PutName(image, 0x180, ".?AVexception@std@@");

  const Orca::CrashNote::ThrownTypes thrown =
      Orca::CrashNote::ReadThrowInfo(image.data(), image.data());
  ASSERT_EQ(thrown.decorated.size(), 3u);
  EXPECT_EQ(thrown.decorated[0], ".?AVsystem_error@std@@");
  EXPECT_EQ(thrown.decorated[2], ".?AVexception@std@@");
  ASSERT_TRUE(thrown.std_exception_offset.has_value());
  EXPECT_EQ(*thrown.std_exception_offset, 0x10);

  Put32(image, 0x20, 1);
  const Orca::CrashNote::ThrownTypes first =
      Orca::CrashNote::ReadThrowInfo(image.data(), image.data());
  EXPECT_EQ(first.decorated.size(), 1u);
  EXPECT_FALSE(first.std_exception_offset.has_value());
}

TEST(OrcaCrashNote, ReadThrowInfoWithoutTypesIsEmpty)
{
  std::vector<u8> image(0x40);
  EXPECT_TRUE(Orca::CrashNote::ReadThrowInfo(image.data(), image.data()).decorated.empty());
  EXPECT_TRUE(Orca::CrashNote::ReadThrowInfo(nullptr, image.data()).decorated.empty());
  EXPECT_TRUE(Orca::CrashNote::ReadThrowInfo(image.data(), nullptr).decorated.empty());
}

TEST(OrcaCrashNote, FixedLineCutsToFitAndStaysTerminated)
{
  char out[8];
  Orca::CrashNote::FixedLine line(out, sizeof(out));
  line << "abc" << "defghij";
  EXPECT_EQ(line.Length(), 7u);
  EXPECT_STREQ(out, "abcdefg");
  line.Hex(0xFF);
  EXPECT_STREQ(out, "abcdefg");

  char hex[40];
  Orca::CrashNote::FixedLine digits(hex, sizeof(hex));
  digits.Hex(0x887A0006, 8) << " ";
  digits.Hex(0x10) << " ";
  digits.Hex(0) << " ";
  digits.Hex(0x6, 8) << " ";
  digits.Dec(0) << " ";
  digits.Dec(4242);
  EXPECT_STREQ(hex, "0x887A0006 0x10 0x0 0x00000006 0 4242");

  char wide[24];
  Orca::CrashNote::FixedLine full(wide, sizeof(wide));
  full.Hex(0xFFFFFFFFFFFFFFFFull);
  EXPECT_STREQ(wide, "0xFFFFFFFFFFFFFFFF");

  Orca::CrashNote::FixedLine none(nullptr, 0);
  none << "text";
  EXPECT_EQ(none.Length(), 0u);
}

TEST(OrcaCrashNote, FaultNamesOnlyTheFatalCodes)
{
  EXPECT_STREQ(Orca::CrashNote::FaultName(0xC0000005), "access violation");
  EXPECT_STREQ(Orca::CrashNote::FaultName(0x80000003), "breakpoint");
  EXPECT_STREQ(Orca::CrashNote::FaultName(0xC00000FD), "stack overflow");
  EXPECT_EQ(Orca::CrashNote::FaultName(0xE06D7363), nullptr);
  EXPECT_EQ(Orca::CrashNote::FaultName(0x406D1388), nullptr);
  EXPECT_EQ(Orca::CrashNote::FaultName(0x40010006), nullptr);
}

TEST(OrcaCrashNote, FaultStackFormatsModuleOffsetsAndStripsPaths)
{
  using namespace Orca::CrashNote;
  const StackFrame frames[] = {{"C:\\build\\subspace.exe", 0x1234},
                               {"/system/ntdll.dll", 0x118095},
                               {{}, 0x7FFA12345678},
                               {"bad\nname\t.dll", 0}};
  char out[512];
  const size_t length = FormatFaultStack(out, sizeof(out), frames);
  EXPECT_EQ(std::string(out, length),
            "Orca: exception stack subspace.exe+0x1234 ntdll.dll+0x118095 "
            "?+0x7FFA12345678 bad?name?.dll+0x0");
  EXPECT_EQ(length, std::strlen(out));
  FormatFaultStack(out, sizeof(out), {});
  EXPECT_STREQ(out, "Orca: exception stack unavailable");
}

TEST(OrcaCrashNote, FaultStackLimitsFramesAndKeepsCompleteTokensWithinAppLimit)
{
  using namespace Orca::CrashNote;
  std::array<StackFrame, 30> frames;
  frames.fill({"a", 1});
  char out[512];
  const size_t length = FormatFaultStack(out, sizeof(out), frames);
  EXPECT_EQ(std::string(out, length), "Orca: exception stack" +
                                        std::string(" a+0x1 a+0x1 a+0x1 a+0x1 a+0x1 a+0x1") +
                                        " a+0x1 a+0x1 a+0x1 a+0x1 a+0x1 a+0x1" +
                                        " a+0x1 a+0x1 a+0x1 a+0x1 a+0x1 a+0x1" +
                                        " a+0x1 a+0x1 a+0x1 a+0x1 a+0x1 a+0x1 ...");
  frames.fill({"subspace.exe", 0x123456789ABCDEF0});
  const size_t capped = FormatFaultStack(out, sizeof(out), frames);
  EXPECT_LE(capped + 1, MAX_FAULT_STACK_LINE);
  EXPECT_TRUE(std::string_view(out).ends_with("subspace.exe+0x123456789ABCDEF0 ..."));
  char small[32];
  FormatFaultStack(small, sizeof(small), frames);
  EXPECT_STREQ(small, "Orca: exception stack ...");
  char tiny[5];
  EXPECT_EQ(FormatFaultStack(tiny, sizeof(tiny), frames), 4u);
  EXPECT_STREQ(tiny, "Orca");
  EXPECT_EQ(FormatFaultStack(nullptr, 0, frames), 0u);
  char one = 'x';
  EXPECT_EQ(FormatFaultStack(&one, 1, frames), 0u);
  EXPECT_EQ(one, '\0');
  const std::string huge(400, 'm');
  const StackFrame huge_frame[] = {{huge, 1}};
  FormatFaultStack(out, sizeof(out), huge_frame);
  EXPECT_STREQ(out, "Orca: exception stack ...");
}

TEST(OrcaCrashNote, FaultLineNamesTheModuleOffsetAndGraphicsDevice)
{
  Orca::CrashNote::Fault fault;
  fault.code = 0xC0000005;
  fault.address = 0x7FFA12345678;
  fault.module = "nvwgf2umx.dll";
  fault.offset = 0x1A2B3C;
  fault.has_access = true;
  fault.access_kind = 0;
  fault.access_address = 0x10;
  fault.thread = 4242;
  fault.graphics = "lost (D3D11: device hung, 0x887A0006) at a 16x16x1 texture";
  char out[512];
  const size_t length = Orca::CrashNote::FormatFault(out, sizeof(out), fault);
  EXPECT_EQ(std::string(out, length),
            "Orca: exception 0xC0000005 (access violation reading 0x10) at nvwgf2umx.dll+0x1A2B3C "
            "on thread 4242; graphics device: lost (D3D11: device hung, 0x887A0006) at a 16x16x1 "
            "texture");

  fault.module = {};
  fault.access_kind = 1;
  fault.access_address = 0;
  fault.graphics = {};
  Orca::CrashNote::FormatFault(out, sizeof(out), fault);
  EXPECT_STREQ(out, "Orca: exception 0xC0000005 (access violation writing 0x0) at 0x7FFA12345678 "
                    "(in no module) on thread 4242; graphics device: none");

  fault.first_chance = true;
  Orca::CrashNote::FormatFault(out, sizeof(out), fault);
  EXPECT_STREQ(out, "Orca: exception 0xC0000005 (access violation writing 0x0) (first chance) at "
                    "0x7FFA12345678 (in no module) on thread 4242; graphics device: none");
  fault.first_chance = false;

  Orca::CrashNote::Fault breakpoint;
  breakpoint.code = 0x80000003;
  breakpoint.module = "Orca.exe";
  breakpoint.offset = 0x4F00;
  breakpoint.thread = 7;
  breakpoint.graphics = "D3D11 running";
  Orca::CrashNote::FormatFault(out, sizeof(out), breakpoint);
  EXPECT_STREQ(out, "Orca: exception 0x80000003 (breakpoint) at Orca.exe+0x4F00 on thread 7; "
                    "graphics device: D3D11 running");

  char small[16];
  EXPECT_EQ(Orca::CrashNote::FormatFault(small, sizeof(small), breakpoint), 15u);
  EXPECT_STREQ(small, "Orca: exception");
}

#if defined(_WIN32) && GTEST_HAS_DEATH_TEST
namespace
{
__declspec(noinline) void RaiseStackTestFault()
{
  RaiseException(0xC0000374, 0, 0, nullptr);
  volatile int returned = 1;
  (void)returned;
}

void CatchStackTestFault()
{
  __try
  {
    RaiseStackTestFault();
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
  }
}

void PrepareStackTestFault()
{
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  SetUnhandledExceptionFilter(nullptr);
  Orca::CrashNote::InstallFaultReporter();
}
}

TEST(OrcaCrashNoteDeathTest, WorkerFaultIncludesNativeCaller)
{
  EXPECT_EXIT(
      {
        PrepareStackTestFault();
        std::thread worker(RaiseStackTestFault);
        worker.join();
        ExitProcess(0);
      },
      ::testing::ExitedWithCode(static_cast<int>(0xC0000374)),
      "Orca: exception stack .*tests\\.exe\\+0x\\w+");
}

TEST(OrcaCrashNoteDeathTest, CaughtCpuFirstChanceIncludesNativeCaller)
{
  EXPECT_EXIT(
      {
        PrepareStackTestFault();
        Core::DeclareAsCPUThread();
        CatchStackTestFault();
        ExitProcess(0);
      },
      ::testing::ExitedWithCode(0),
      "\\(first chance\\).*\r?\nOrca: exception stack .*tests\\.exe\\+0x\\w+");
}
#endif
