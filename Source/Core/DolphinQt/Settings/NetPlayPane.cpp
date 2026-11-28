// Copyright 2026 Subspace
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Settings/NetPlayPane.h"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

#include <QGridLayout>
#include <QComboBox>
#include <QEvent>
#include <QGroupBox>
#include <QHelpEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QRadioButton>
#include <QStyle>
#include <QStyleOption>
#include <QToolTip>
#include <QVBoxLayout>

#include "Common/FileUtil.h"
#include "Core/Config/NetplaySettings.h"

#include "DolphinQt/Config/ConfigControls/ConfigBool.h"
#include "DolphinQt/Config/ConfigControls/ConfigChoice.h"
#include "DolphinQt/Config/ConfigControls/ConfigRadio.h"

namespace
{
class TitleToolTip final : public QObject
{
public:
  using QObject::QObject;

  bool eventFilter(QObject* object, QEvent* event) override
  {
    if (event->type() != QEvent::ToolTip)
      return false;
    auto* box = static_cast<QGroupBox*>(object);
    QStyleOptionGroupBox option;
    option.initFrom(box);
    option.text = box->title();
    option.textAlignment = box->alignment();
    option.subControls = QStyle::SC_GroupBoxFrame | QStyle::SC_GroupBoxLabel;
    option.features = box->isFlat() ? QStyleOptionFrame::Flat : QStyleOptionFrame::None;
    option.lineWidth = 1;
    const QRect title =
        box->style()->subControlRect(QStyle::CC_GroupBox, &option, QStyle::SC_GroupBoxLabel, box);
    if (title.contains(static_cast<QHelpEvent*>(event)->pos()))
      return false;
    QToolTip::hideText();
    event->accept();
    return true;
  }
};
}

NetPlayPane::NetPlayPane(QWidget* parent) : QWidget(parent)
{
  CreateLayout();
}

void NetPlayPane::CreateLayout()
{
  auto* netplay_box = new QGroupBox(tr("Display Name Settings"));
  auto* netplay_layout = new QGridLayout();
  netplay_box->setLayout(netplay_layout);

  m_hud_names = new ConfigChoiceMap<int>({{tr("Below Character Names"), 3},
                                          {tr("Below Character Names, Add Background"), 2},
                                          {tr("Replace Character Names"), 0},
                                          {tr("Replace Character Names/Background"), 1},
                                          {tr("Do Not Display"), 4}},
                                         Config::NETPLAY_SUBSPACE_HUD_NAMES);
  m_hud_names->SetDescription(tr("Choose how/if you would like NetPlay Display Names displayed."));
  const std::array<std::pair<QString, QString>, 5> examples{{
      {tr("NetPlay Display Names are added below the HUD."),
       QStringLiteral("sd-below.png")},
      {tr("NetPlay Display Names are added below the HUD with a black Brawl logo as "
          "background."),
       QStringLiteral("sd-below-bg.png")},
      {tr("NetPlay Display Names replace character names on the HUD."),
       QStringLiteral("sd-replace.png")},
      {tr("NetPlay Display Names replace character names on the HUD, and a port-colored "
          "Brawl logo replaces the character name background texture."),
       QStringLiteral("sd-replace-bg.png")},
      {tr("NetPlay Display Names are not shown."), QStringLiteral("sd-proper.png")},
  }};
  m_names_image = new QLabel;
  m_names_text = new QLabel;
  m_names_text->setWordWrap(true);
  const auto show_example = [this, examples](int choice) {
    const auto& [text, image] = examples[static_cast<std::size_t>(std::clamp(choice, 0, 4))];
    m_names_image->setPixmap(QPixmap(
        QString::fromStdString(File::GetSysDirectory() + "Resources/") + image));
    m_names_text->setText(text);
  };
  show_example(m_hud_names->currentIndex());
  connect(m_hud_names, &QComboBox::currentIndexChanged, this, show_example);
  netplay_layout->addWidget(m_hud_names, 0, 0);
  netplay_layout->addWidget(m_names_image, 1, 0);
  netplay_layout->addWidget(m_names_text, 2, 0);

  auto* corner_box = new QGroupBox(tr("Personal Connection Display"));
  corner_box->setToolTip(
      tr("Refers to how your connection information in the top left is displayed."));
  corner_box->installEventFilter(new TitleToolTip(corner_box));
  auto* corner_layout = new QVBoxLayout();
  corner_box->setLayout(corner_layout);
  const auto add_check = [corner_layout](const QString& label, const Config::Info<bool>& setting,
                                         const QString& help) {
    auto* box = new ConfigBool(label, setting);
    box->SetTitle({});
    box->setToolTip(help);
    corner_layout->addWidget(box);
  };
  add_check(tr("Show Sprite"), Config::NETPLAY_SUBSPACE_CORNER_SPRITE,
            tr("Displays your ping with the classic Brawl sprite representation."));
  add_check(tr("Show FPS"), Config::NETPLAY_SUBSPACE_CORNER_FPS,
            tr("Displays how fast your emulator is running in frames per second (should be "
               "60fps)."));
  add_check(tr("Show Frame Time"), Config::NETPLAY_SUBSPACE_FRAME_MS,
            tr("Displays in milliseconds how long the slowest frame of the last second took to "
               "appear on your screen (~17ms is best).\nHigher values mean a frame was late, either "
               "from your computer slowing down or from waiting on another player's connection."));
  add_check(tr("Show Ping"), Config::NETPLAY_SUBSPACE_CORNER_PING,
            tr("Displays your connection speed with the slowest opponent in the room."));

  auto* ping_box = new QGroupBox(tr("HUD Connection Display"));
  ping_box->setToolTip(
      tr("Refers to the area below the HUD that shows you other players' connection strength."));
  ping_box->installEventFilter(new TitleToolTip(ping_box));
  auto* ping_layout = new QVBoxLayout();
  ping_box->setLayout(ping_layout);
  auto* ping_choices = new QHBoxLayout();
  m_ping_sprite = new ConfigRadioInt(tr("Show Sprite"), Config::NETPLAY_SUBSPACE_PING_DISPLAY, 0);
  m_ping_number = new ConfigRadioInt(tr("Show Number"), Config::NETPLAY_SUBSPACE_PING_DISPLAY, 1);
  m_ping_sprite->SetTitle({});
  m_ping_number->SetTitle({});
  ping_choices->addWidget(m_ping_sprite);
  ping_choices->addWidget(m_ping_number);
  ping_choices->addStretch();
  ping_layout->addLayout(ping_choices);
  m_ping_image = new QLabel;
  ping_layout->addWidget(m_ping_image);
  m_ping_text = new QLabel;
  m_ping_text->setWordWrap(true);
  ping_layout->addWidget(m_ping_text);
  const auto show_ping_text = [this] {
    m_ping_image->setPixmap(QPixmap(
        QString::fromStdString(File::GetSysDirectory() + "Resources/") +
        (m_ping_number->isChecked() ? QStringLiteral("sd-connection-value.png") :
                                      QStringLiteral("sd-connection-sprite.png"))));
    m_ping_text->setText(
        m_ping_number->isChecked() ?
            tr("Shows you/your opponents' exact ping value in milliseconds.") :
            tr("Shows you/your opponents' ping with the classic Brawl sprite representation."));
  };
  show_ping_text();
  connect(m_ping_sprite, &QRadioButton::toggled, this, show_ping_text);
  connect(m_ping_number, &QRadioButton::toggled, this, show_ping_text);

  auto* link_box = new QGroupBox(tr("Connection Settings"));
  auto* link_layout = new QVBoxLayout();
  link_box->setLayout(link_layout);
  auto* direct = new ConfigBool(tr("Connect directly to other players"),
                                Config::NETPLAY_SUBSPACE_DIRECT_LINKS);
  direct->SetTitle({});
  direct->setToolTip(tr("Connecting directly to other players (when possible) improves latency. "
                        "Disabling this option will force all your inputs to be relayed through "
                        "the room host, which may add lag."));
  link_layout->addWidget(direct);

  auto* main_layout = new QVBoxLayout;
  main_layout->addWidget(netplay_box);
  main_layout->addWidget(corner_box);
  main_layout->addWidget(ping_box);
  main_layout->addWidget(link_box);
  main_layout->addStretch();
  setLayout(main_layout);
}
