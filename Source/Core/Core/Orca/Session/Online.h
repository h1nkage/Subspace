// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <map>
#include <string>
#include <optional>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Session.h"
#include "Core/Orca/Session/RelayRoom.h"

namespace Orca::Online
{
bool Enabled();
bool LocalRoom();

void Start();
void StartRoom(const std::string& code, bool joining, bool friends_room = true,
               bool join_if_taken = false);
std::string Code();
std::string SeatName(int seat);
std::vector<u8> HelloControls();
std::vector<u8> HelloQueue();

void RequestJoin(const std::string& code);
std::optional<std::string> TakeJoinRequest();
void RequestLeave();
bool TakeLeaveRequest();
void RequestHost(const std::string& code);
std::optional<std::string> TakeHostRequest();

std::string RoomQueue();
bool RoomDoubles();
std::map<int, int> RoomTeams();
bool MatchLive();
bool SetOver();
void SetOpponentPlugged(bool plugged);
void ReportGame(const Net::GameReport& report);
void ReportGameStart(int start);
int KnownSetGames();
void ReportDesync();
void ReportPeerFault();
void KeepAlive();
void SendReplayStream(const std::vector<u8>& bytes);
void SendReplayPacked(const std::vector<std::vector<u8>>& pieces, int frames, u64 generation);
u64 RoomGeneration();
void ReportStall();
void SetChosenDelay(std::optional<int> frames);
std::optional<int> ChosenDelay();
struct DelayRequest
{
  int frames = 0;
  int at = -1;
};
std::optional<DelayRequest> TakeDelayRequest();
std::optional<int> TakeVolumeRequest();
bool ShowPing();
int HudNames();
bool ShowCornerSprite();
bool ShowCornerFps();
bool ShowFrameMs();
bool ShowCornerPing();
void SetDirectWanted(bool on);
bool DirectWanted();
enum class RecordChoice
{
  Auto,
  On,
  Off,
};
void SetRecordChoice(RecordChoice choice);
bool RecordWanted();
std::optional<bool> Joining();
Net::Transport* Transport();
int Seat();
int HostPort();
int SeatPort(int seat);
int PortSeat(int port);
std::vector<int> CoSeats();
std::optional<std::vector<int>> CoSeatsForSolo();
void SetSessionActive(bool active);
bool TryStartSession(const std::vector<int>& co_seats);
int LocalIndex(int seat);
std::vector<int> GroupOf(int seat);
std::vector<Net::PeerEvent> TakePeerEvents();
bool HostLeftPending();
std::vector<int> LeftSeatsPending();
std::optional<std::chrono::steady_clock::time_point> HostAwayDeadline();
void RetryPeer(int seat);
bool HoldInputSilence(std::chrono::steady_clock::time_point since,
                       std::chrono::steady_clock::time_point now,
                       std::optional<std::vector<int>> missing = std::nullopt);
bool RoomRecovery();
bool RoomRecovering(std::chrono::steady_clock::time_point now);
bool RoomEnded();
bool InRoom();
bool FreshTicket(bool fresh, std::string* ticket, std::string* store_url, std::string* error);
void DropPeer(int seat, const std::string& reason);
void HoldJoins(bool holding, bool fight = false);
bool IsDesync(const std::string& session_error);
bool IsRollbackFailure(const std::string& session_error);
void BroadcastNames(const std::vector<Net::KeyframeInfo::Name>& names, int version);
void ShutdownInBackground();
bool RoomsLeaving();
void OfferKeyframe(int seat, const Net::KeyframeInfo& info);

void PrepareJoin();
bool TakePrepareJoin();
void LetIn();
bool TakeLetIn();

bool DropInPending();
bool ArrivalPending();

int RoundTripMs(u32* sequence = nullptr);
std::vector<int> RecentRoundTrips(u32* sequence);
Net::DirectSummary Direct();
std::string RoomErrorCode();
bool RoomTaken();
std::string StatusLine();

bool ReportSessionEnd(const std::string& session_error, bool from_peer = false);

void SetLeaveReason(const std::string& reason);

void ReportPeerStalled(std::string_view sentence);

void TestDropRoom();
void TestDirect(const std::string& command);

void Shutdown();

std::string CompatibilityKey();
std::string CompatibilityKeyParts();
std::string UxKeyPart(int engine, int game);
}
