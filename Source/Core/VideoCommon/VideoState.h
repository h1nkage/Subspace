// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

class PointerWrap;

void VideoCommon_DoState(PointerWrap& p);

void VideoCommon_SetSkipRender(bool skip);
bool VideoCommon_IsSkippingRender();

void VideoCommon_SetRollbackSnapshot(bool snapshot);
bool VideoCommon_IsRollbackSnapshot();

void VideoCommon_SetSnapshotRedisplays(bool redisplays);
bool VideoCommon_LoadRedisplays();
