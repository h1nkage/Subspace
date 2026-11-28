// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/AutoUpdate.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <picojson.h>

#include "Common/HttpRequest.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Network.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "Common/Version.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <sys/stat.h>
#endif

#if defined(_WIN32) || defined(__APPLE__)
#define OS_SUPPORTS_UPDATER
#include "Common/CommonFuncs.h"
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#endif

// Refer to docs/autoupdate_overview.md for a detailed overview of the autoupdate process

namespace
{
std::atomic_bool s_check_in_progress = false;
bool s_update_triggered = false;

#ifdef __APPLE__
const char UPDATER_CONTENT_PATH[] = "/Contents/MacOS/Dolphin Updater";
#endif

#ifdef OS_SUPPORTS_UPDATER

const char UPDATER_LOG_FILE[] = "Updater.log";

std::string UpdaterPath(bool relocated = false)
{
#ifdef __APPLE__
  if (relocated)
    return File::GetExeDirectory() + DIR_SEP + ".Dolphin Updater.2.app";
  else
    return File::GetBundleDirectory() + DIR_SEP + "Contents/Helpers/Dolphin Updater.app";
#else
  return File::GetExeDirectory() + DIR_SEP + "Updater.exe";
#endif
}

std::string MakeUpdaterCommandLine(const std::map<std::string, std::string>& flags)
{
#ifdef __APPLE__
  std::string cmdline = "\"" + UpdaterPath(true) + UPDATER_CONTENT_PATH + "\"";
#else
  std::string cmdline = UpdaterPath();
#endif

  cmdline += " ";

  for (const auto& pair : flags)
  {
    std::string value = "--" + pair.first + "=" + pair.second;
    value = ReplaceAll(value, "\"", "\\\"");  // Escape double quotes.
    value = "\"" + value + "\" ";
    cmdline += value;
  }
  return cmdline;
}

void CleanupFromPreviousUpdate()
{
#ifdef __APPLE__
  // Remove the relocated updater file.
  File::DeleteDirRecursively(UpdaterPath(true));

  // Remove the old (non-embedded) updater app bundle.
  // While the update process will delete the files within the old bundle after updating to a
  // version with an embedded updater, it won't delete the folder structure of the bundle, so
  // we should clean those leftovers up.
  File::DeleteDirRecursively(File::GetExeDirectory() + DIR_SEP + "Dolphin Updater.app");
#endif

  // Updater.log was moved from GetExeDirectory() to GetUserPath(D_LOGS_IDX) in 5.0-14529.
  File::Delete(File::GetExeDirectory() + DIR_SEP + "Updater.log",
               File::IfAbsentBehavior::NoConsoleWarning);
}
#endif

constexpr std::string_view UPDATE_BASE_URL = "https://h1nkage.github.io/Subspace-Updates/";
constexpr std::string_view RELEASE_PREFIX = "Subspace-";

std::optional<std::array<u32, 3>> ReleaseNumbers(std::string_view version)
{
  if (!version.starts_with(RELEASE_PREFIX))
    return std::nullopt;
  version.remove_prefix(RELEASE_PREFIX.size());
  std::array<u32, 3> numbers{};
  for (std::size_t i = 0; i < numbers.size(); ++i)
  {
    const std::size_t dot = version.find('.');
    const bool last = i + 1 == numbers.size();
    if (last == (dot != std::string_view::npos))
      return std::nullopt;
    const std::string_view part = last ? version : version.substr(0, dot);
    if (part.empty() || part.size() > 6 ||
        !std::ranges::all_of(part, [](char c) { return c >= '0' && c <= '9'; }))
    {
      return std::nullopt;
    }
    const auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), numbers[i]);
    if (ec != std::errc{} || end != part.data() + part.size())
      return std::nullopt;
    if (!last)
      version.remove_prefix(dot + 1);
  }
  return numbers;
}

std::optional<std::string> UpdateUrl(const picojson::value& path)
{
  if (!path.is<std::string>())
    return std::nullopt;
  const std::string& text = path.get<std::string>();
  if (text.empty() || text.find("..") != std::string::npos)
    return std::nullopt;
  if (text.starts_with("https://"))
    return text;
  if (text.find(':') != std::string::npos || text.starts_with('/'))
    return std::nullopt;
  return std::string(UPDATE_BASE_URL) + text;
}

}  // namespace

bool AutoUpdateChecker::SystemSupportsAutoUpdates()
{
#if defined(AUTOUPDATE) && defined(OS_SUPPORTS_UPDATER)
  return true;
#else
  return false;
#endif
}

static u32 GetOwnProcessId()
{
#ifdef _WIN32
  return GetCurrentProcessId();
#else
  return getpid();
#endif
}

void AutoUpdateChecker::CheckForUpdate(std::string_view update_track,
                                       std::string_view hash_override, const CheckType check_type)
{
  if (Common::OfflineReplay())
    return;

  bool expected_check_in_progress = false;
  if (!s_check_in_progress.compare_exchange_strong(expected_check_in_progress, true))
    return;

  Common::ScopeGuard guard([]() { s_check_in_progress.store(false); });

  const bool is_manual_check = check_type == CheckType::Manual;

  if (s_update_triggered)
  {
    if (is_manual_check)
      SuccessAlertFmtT("A Subspace update is already scheduled for the next time it closes.");

    return;
  }

  if (!SystemSupportsAutoUpdates() || (!is_manual_check && update_track.empty()))
    return;

#ifdef OS_SUPPORTS_UPDATER
  CleanupFromPreviousUpdate();
#endif

  const std::string own_version =
      hash_override.empty() ? Common::GetScmDescStr() : std::string(hash_override);
  const std::optional<std::array<u32, 3>> own_numbers = ReleaseNumbers(own_version);
  if (!own_numbers)
  {
    if (is_manual_check)
    {
      SuccessAlertFmtT("Updates are only available for release versions of Subspace. This is a "
                       "development build ({0}).",
                       own_version);
    }
    INFO_LOG_FMT(COMMON, "Auto-update: {} is not a release version, not checking.", own_version);
    return;
  }

  Common::HttpRequest req{std::chrono::seconds{10}};
  auto resp = req.Get(std::string(UPDATE_BASE_URL) + "latest.json");
  if (!resp)
  {
    if (is_manual_check)
      CriticalAlertFmtT("Unable to contact update server.");
    return;
  }
  const std::string contents(reinterpret_cast<char*>(resp->data()), resp->size());
  INFO_LOG_FMT(COMMON, "Auto-update JSON response: {}", contents);

  picojson::value json;
  const std::string err = picojson::parse(json, contents);
  if (!err.empty() || !json.is<picojson::object>())
  {
    if (is_manual_check)
      CriticalAlertFmtT("Invalid JSON received from auto-update service : {0}", err);
    return;
  }
  const picojson::object& obj = json.get<picojson::object>();
  const auto field = [&obj](const char* name) {
    const auto it = obj.find(name);
    return it == obj.end() ? picojson::value{} : it->second;
  };

  const picojson::value latest = field("version");
  const std::optional<std::array<u32, 3>> latest_numbers =
      latest.is<std::string>() ? ReleaseNumbers(latest.get<std::string>()) : std::nullopt;
  const std::optional<std::string> next_manifest = UpdateUrl(field("manifest"));
  const std::optional<std::string> content_store = UpdateUrl(field("content-store"));
  if (!latest_numbers || !next_manifest || !content_store || !content_store->ends_with('/'))
  {
    if (is_manual_check)
      CriticalAlertFmtT("The update information on the server is incomplete.");
    return;
  }

  if (*latest_numbers <= *own_numbers)
  {
    if (is_manual_check)
      SuccessAlertFmtT("You are running the latest version of Subspace.");
    INFO_LOG_FMT(COMMON, "Auto-update status: we are up to date.");
    return;
  }

  const std::string this_manifest =
      std::string(UPDATE_BASE_URL) + "manifests/" + own_version + ".manifest";
  if (!req.Get(this_manifest))
  {
    if (is_manual_check)
    {
      CriticalAlertFmtT("{0} can't update itself because it was never published as an update. "
                        "Download {1} instead.",
                        own_version, latest.get<std::string>());
    }
    WARN_LOG_FMT(COMMON, "Auto-update: no manifest for {} at {}", own_version, this_manifest);
    return;
  }

  NewVersionInformation nvi;
  nvi.this_manifest_url = this_manifest;
  nvi.next_manifest_url = *next_manifest;
  nvi.content_store_url = *content_store;
  nvi.new_shortrev = latest.get<std::string>();
  nvi.new_hash = latest.get<std::string>();

  OnUpdateAvailable(nvi, check_type);
}

void AutoUpdateChecker::TriggerUpdate(const AutoUpdateChecker::NewVersionInformation& info,
                                      const AutoUpdateChecker::RestartMode restart_mode)
{
  if (Common::OfflineReplay())
    return;

  // Check to make sure we don't already have an update triggered
  if (s_update_triggered)
  {
    WARN_LOG_FMT(COMMON, "Auto-update: received a redundant trigger request, ignoring");
    return;
  }

#ifdef OS_SUPPORTS_UPDATER
  std::map<std::string, std::string> updater_flags;
  updater_flags["this-manifest-url"] = info.this_manifest_url;
  updater_flags["next-manifest-url"] = info.next_manifest_url;
  updater_flags["content-store-url"] = info.content_store_url;
  updater_flags["parent-pid"] = std::to_string(GetOwnProcessId());
  updater_flags["install-base-path"] = File::GetExeDirectory();
  updater_flags["log-file"] = File::GetUserPath(D_LOGS_IDX) + UPDATER_LOG_FILE;

  if (restart_mode == RestartMode::RESTART_AFTER_UPDATE)
    updater_flags["binary-to-restart"] = File::GetExePath();

#ifdef __APPLE__
  // Copy the updater so it can update itself if needed.
  const std::string reloc_updater_path = UpdaterPath(true);
  if (!File::Copy(UpdaterPath(), reloc_updater_path))
  {
    CriticalAlertFmtT("Unable to create updater copy.");
    return;
  }
  if (chmod((reloc_updater_path + UPDATER_CONTENT_PATH).c_str(), 0700) != 0)
  {
    CriticalAlertFmtT("Unable to set permissions on updater copy.");
    return;
  }
#endif

  // Run the updater!
  std::string command_line = MakeUpdaterCommandLine(updater_flags);
  INFO_LOG_FMT(COMMON, "Updater command line: {}", command_line);

#ifdef _WIN32
  STARTUPINFO sinfo{.cb = sizeof(sinfo)};
  sinfo.dwFlags = STARTF_FORCEOFFFEEDBACK;  // No hourglass cursor after starting the process.
  PROCESS_INFORMATION pinfo;
  if (CreateProcessW(UTF8ToWString(UpdaterPath()).c_str(), UTF8ToWString(command_line).data(),
                     nullptr, nullptr, FALSE, 0, nullptr, nullptr, &sinfo, &pinfo))
  {
    CloseHandle(pinfo.hThread);
    CloseHandle(pinfo.hProcess);
    s_update_triggered = true;
  }
  else
  {
    const std::string error = Common::GetLastErrorString();
    CriticalAlertFmtT("Could not start updater process: {0}", error);
  }
#else
  if (popen(command_line.c_str(), "r") == nullptr)
  {
    const std::string error = Common::LastStrerrorString();
    CriticalAlertFmtT("Could not start updater process: {0}", error);
  }
  else
  {
    s_update_triggered = true;
  }
#endif

#endif
}
