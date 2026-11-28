// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/FS/HostBackend/FS.h"
#include "Core/Rollback/Rollback.h"
#include "UICommon/UICommon.h"

namespace
{
using namespace IOS::HLE::FS;
constexpr Modes modes{Mode::ReadWrite, Mode::ReadWrite, Mode::ReadWrite};
constexpr u32 CLUSTER_BYTES = CLUSTER_SIZE;

class OrcaNandQuota : public testing::Test
{
protected:
  void SetUp() override
  {
    m_root = File::CreateTempDir();
    ASSERT_FALSE(m_root.empty());
    UICommon::SetUserDirectory(m_root);
  }
  void TearDown() override
  {
    m_fs.reset();
    File::DeleteDirRecursively(m_root);
  }
  void Make(u64 cluster_quota)
  {
    m_fs = std::make_unique<HostFileSystem>(m_root + "/nand", std::vector<NandRedirect>{},
                                            cluster_quota);
  }
  Result<u32> Write(const std::string& path, u32 offset, u32 size, u8 fill = 0x5A)
  {
    Result<FileHandle> file = m_fs->OpenFile(0, 0, path, Mode::ReadWrite);
    if (!file)
      return std::unexpected{file.error()};
    if (offset != 0 && !file->Seek(offset, SeekMode::Set))
      return std::unexpected{ResultCode::Invalid};
    const std::vector<u8> data(size, fill);
    const Result<size_t> written = file->Write(data.data(), data.size());
    if (!written)
      return std::unexpected{written.error()};
    return static_cast<u32>(*written);
  }
  u32 Size(const std::string& path)
  {
    const Result<Metadata> metadata = m_fs->GetMetadata(0, 0, path);
    return metadata ? metadata->size : 0xFFFFFFFF;
  }
  std::vector<u8> Snapshot()
  {
    Rollback::SnapshotScope scope;
    std::vector<u8> buffer(4096);
    u8* ptr = buffer.data();
    PointerWrap p(&ptr, buffer.size(), PointerWrap::Mode::Write);
    m_fs->DoState(p);
    EXPECT_TRUE(p.IsWriteMode());
    buffer.resize(ptr - buffer.data());
    return buffer;
  }
  void Load(std::vector<u8> buffer)
  {
    Rollback::SnapshotScope scope;
    u8* ptr = buffer.data();
    PointerWrap p(&ptr, buffer.size(), PointerWrap::Mode::Read);
    m_fs->DoState(p);
    EXPECT_TRUE(p.IsReadMode());
  }

  std::string m_root;
  std::unique_ptr<HostFileSystem> m_fs;
};
}

TEST_F(OrcaNandQuota, AWriteThatWouldOverfillTheNandFails)
{
  Make(4);
  ASSERT_EQ(m_fs->CreateDirectory(0, 0, "/tmp", 0, modes), ResultCode::Success);
  ASSERT_EQ(m_fs->CreateFile(0, 0, "/tmp/a", 0, modes), ResultCode::Success);
  ASSERT_EQ(m_fs->CreateFile(0, 0, "/tmp/b", 0, modes), ResultCode::Success);

  EXPECT_EQ(Write("/tmp/a", 0, 3 * CLUSTER_BYTES), 3 * CLUSTER_BYTES);
  EXPECT_EQ(Write("/tmp/a", 100, 2 * CLUSTER_BYTES), 2 * CLUSTER_BYTES);
  EXPECT_EQ(Write("/tmp/b", 0, CLUSTER_BYTES + 1).error(), ResultCode::NoFreeSpace);
  EXPECT_EQ(Size("/tmp/b"), 0u);
  EXPECT_EQ(Write("/tmp/a", 3 * CLUSTER_BYTES, CLUSTER_BYTES + 1).error(), ResultCode::NoFreeSpace);
  EXPECT_EQ(Size("/tmp/a"), 3 * CLUSTER_BYTES);
  EXPECT_EQ(Write("/tmp/b", 0, CLUSTER_BYTES), CLUSTER_BYTES);
  EXPECT_EQ(Write("/tmp/b", CLUSTER_BYTES, 1).error(), ResultCode::NoFreeSpace);

  ASSERT_EQ(m_fs->Delete(0, 0, "/tmp/a"), ResultCode::Success);
  EXPECT_EQ(Write("/tmp/b", CLUSTER_BYTES, 3 * CLUSTER_BYTES), 3 * CLUSTER_BYTES);
  EXPECT_EQ(Size("/tmp/b"), 4 * CLUSTER_BYTES);
}

TEST_F(OrcaNandQuota, FilesAlreadyThereCount)
{
  Make(0);
  ASSERT_EQ(m_fs->CreateDirectory(0, 0, "/tmp", 0, modes), ResultCode::Success);
  ASSERT_EQ(m_fs->CreateFile(0, 0, "/tmp/a", 0, modes), ResultCode::Success);
  ASSERT_EQ(Write("/tmp/a", 0, 6 * CLUSTER_BYTES), 6 * CLUSTER_BYTES);
  m_fs.reset();

  Make(4);
  ASSERT_EQ(m_fs->CreateFile(0, 0, "/tmp/b", 0, modes), ResultCode::Success);
  EXPECT_EQ(Write("/tmp/b", 0, 1).error(), ResultCode::NoFreeSpace);
  EXPECT_EQ(Write("/tmp/a", 0, 6 * CLUSTER_BYTES), 6 * CLUSTER_BYTES);
}

TEST_F(OrcaNandQuota, NoQuotaOutsideASession)
{
  Make(0);
  ASSERT_EQ(m_fs->CreateDirectory(0, 0, "/tmp", 0, modes), ResultCode::Success);
  ASSERT_EQ(m_fs->CreateFile(0, 0, "/tmp/a", 0, modes), ResultCode::Success);
  EXPECT_EQ(Write("/tmp/a", 0, 64 * CLUSTER_BYTES), 64 * CLUSTER_BYTES);
}

TEST_F(OrcaNandQuota, RollingBackForgetsTheGrowth)
{
  Make(4);
  ASSERT_EQ(m_fs->CreateDirectory(0, 0, "/tmp", 0, modes), ResultCode::Success);
  ASSERT_EQ(m_fs->CreateFile(0, 0, "/tmp/a", 0, modes), ResultCode::Success);
  ASSERT_EQ(Write("/tmp/a", 0, CLUSTER_BYTES), CLUSTER_BYTES);
  const std::vector<u8> snapshot = Snapshot();
  ASSERT_EQ(Write("/tmp/a", CLUSTER_BYTES, 3 * CLUSTER_BYTES), 3 * CLUSTER_BYTES);
  EXPECT_EQ(Write("/tmp/a", 4 * CLUSTER_BYTES, 1).error(), ResultCode::NoFreeSpace);
  Load(snapshot);
  EXPECT_FALSE(m_fs->TakeJournalError());
  EXPECT_EQ(Size("/tmp/a"), CLUSTER_BYTES);
  EXPECT_EQ(Write("/tmp/a", CLUSTER_BYTES, 3 * CLUSTER_BYTES), 3 * CLUSTER_BYTES);
  EXPECT_EQ(Write("/tmp/a", 4 * CLUSTER_BYTES, 1).error(), ResultCode::NoFreeSpace);
}

TEST_F(OrcaNandQuota, TheJournalKeepsBoundedBytes)
{
  Make(0);
  constexpr u32 MIB = 1 << 20;
  ASSERT_EQ(m_fs->CreateDirectory(0, 0, "/tmp", 0, modes), ResultCode::Success);
  ASSERT_EQ(m_fs->CreateFile(0, 0, "/tmp/a", 0, modes), ResultCode::Success);
  ASSERT_EQ(Write("/tmp/a", 0, MIB, 0x11), MIB);

  std::vector<u8> snapshot = Snapshot();
  ASSERT_EQ(Write("/tmp/a", 0, MIB, 0x22), MIB);
  EXPECT_EQ(m_fs->JournalBytes(), MIB);
  Load(snapshot);
  EXPECT_FALSE(m_fs->TakeJournalError());
  EXPECT_EQ(m_fs->JournalBytes(), 0u);
  {
    Result<FileHandle> file = m_fs->OpenFile(0, 0, "/tmp/a", Mode::Read);
    ASSERT_TRUE(file.has_value());
    std::array<u8, 4> head{};
    ASSERT_TRUE(file->Read(head.data(), head.size()).has_value());
    EXPECT_EQ(head[0], 0x11);
  }

  snapshot = Snapshot();
  const u32 rewrites = HostFileSystem::JOURNAL_MAX_BYTES / MIB + 2;
  for (u32 i = 0; i < rewrites; ++i)
    ASSERT_EQ(Write("/tmp/a", 0, MIB, static_cast<u8>(i)), MIB);
  EXPECT_LE(m_fs->JournalBytes(), HostFileSystem::JOURNAL_MAX_BYTES);
  Load(snapshot);
  EXPECT_TRUE(m_fs->TakeJournalError());

  snapshot = Snapshot();
  for (u32 i = 0; i < rewrites; ++i)
    ASSERT_EQ(Write("/tmp/a", 0, MIB, static_cast<u8>(i)), MIB);
  const std::vector<u8> later = Snapshot();
  m_fs->JournalTrim(m_fs->LastJournalMark());
  EXPECT_EQ(m_fs->JournalBytes(), 0u);
  ASSERT_EQ(Write("/tmp/a", 0, 16, 0x33), 16u);
  Load(later);
  EXPECT_FALSE(m_fs->TakeJournalError());
}
