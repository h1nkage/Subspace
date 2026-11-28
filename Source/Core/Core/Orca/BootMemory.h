// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <type_traits>

namespace Orca::BootMemory
{
void BeginBoot();
void FirstFrame();
void LogMemory();
bool AllocationFailed();

bool Run(void (*step)(void*), void* context);

template <typename Step>
bool Run(Step&& step)
{
  return Run([](void* context) { (*static_cast<std::remove_reference_t<Step>*>(context))(); },
             &step);
}
}
