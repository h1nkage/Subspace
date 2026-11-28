// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Updater.h"

#include <cstdlib>
#include <utility>

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "Common/Version.h"

#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/RunOnObject.h"
#include "DolphinQt/Settings.h"

// Refer to docs/autoupdate_overview.md for a detailed overview of the autoupdate process

Updater::Updater(QWidget* parent, std::string update_track, std::string hash_override)
    : m_parent(parent), m_update_track(std::move(update_track)),
      m_hash_override(std::move(hash_override))
{
  connect(this, &QThread::finished, this, &QObject::deleteLater);
}

void Updater::run()
{
  AutoUpdateChecker::CheckForUpdate(m_update_track, m_hash_override,
                                    AutoUpdateChecker::CheckType::Automatic);
}

void Updater::CheckForUpdate()
{
  AutoUpdateChecker::CheckForUpdate(m_update_track, m_hash_override,
                                    AutoUpdateChecker::CheckType::Manual);
}

void Updater::OnUpdateAvailable(const NewVersionInformation& info, CheckType check_type)
{
  const bool in_netplay = RunOnObject(m_parent, [] {
                            return Settings::Instance().GetNetPlayClient() != nullptr ||
                                   Settings::Instance().GetNetPlayServer() != nullptr;
                          }).value_or(true);
  if (in_netplay)
  {
    if (check_type == CheckType::Manual)
    {
      RunOnObject(m_parent, [this] {
        ModalMessageBox::information(m_parent, tr("Update available"),
                                     tr("A Subspace update is available. Leave NetPlay, then use "
                                        "Help > Check for Updates to install it."));
        return 0;
      });
    }
    return;
  }

  bool later = false;

  std::optional<int> choice = RunOnObject(m_parent, [&] {
    QDialog* dialog = new QDialog(m_parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    dialog->setWindowTitle(tr("Update available"));

    auto* label = new QLabel(
        tr("<h2>A Subspace update is available!</h2>%1 is available for download. "
           "You are running %2.<br> Would you like to update?")
            .arg(QString::fromStdString(info.new_shortrev))
            .arg(QString::fromStdString(Common::GetScmDescStr())));
    label->setTextFormat(Qt::RichText);

    auto* update_later_check = new QCheckBox(tr("Update after closing Subspace"));

    connect(update_later_check, &QCheckBox::toggled, [&](bool checked) { later = checked; });

    auto* buttons = new QDialogButtonBox;

    auto* never_btn =
        buttons->addButton(tr("Don't Check on Startup"), QDialogButtonBox::DestructiveRole);
    buttons->addButton(tr("Remind Me Later"), QDialogButtonBox::RejectRole);
    buttons->addButton(tr("Install Update"), QDialogButtonBox::AcceptRole);

    auto* layout = new QVBoxLayout;
    dialog->setLayout(layout);

    layout->addWidget(label);
    layout->addWidget(update_later_check);
    layout->addWidget(buttons);

    connect(never_btn, &QPushButton::clicked, [dialog] {
      Settings::Instance().SetAutoUpdateTrack(QString{});
      dialog->reject();
    });

    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);

    return dialog->exec();
  });

  if (choice && *choice == QDialog::Accepted)
  {
    TriggerUpdate(info, later ? AutoUpdateChecker::RestartMode::NO_RESTART_AFTER_UPDATE :
                                AutoUpdateChecker::RestartMode::RESTART_AFTER_UPDATE);

    if (!later)
    {
      RunOnObject(m_parent, [this] {
        m_parent->close();
        return 0;
      });
    }
  }
}
