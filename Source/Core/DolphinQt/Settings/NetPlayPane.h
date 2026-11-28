// Copyright 2026 Subspace
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QWidget>

template <typename T>
class ConfigChoiceMap;
class ConfigRadioInt;
class QLabel;

class NetPlayPane final : public QWidget
{
public:
  explicit NetPlayPane(QWidget* parent = nullptr);

private:
  void CreateLayout();

  ConfigRadioInt* m_ping_sprite;
  ConfigRadioInt* m_ping_number;
  QLabel* m_ping_image;
  QLabel* m_ping_text;
  ConfigChoiceMap<int>* m_hud_names;
  QLabel* m_names_image;
  QLabel* m_names_text;
};
