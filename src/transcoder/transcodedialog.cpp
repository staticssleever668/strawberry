/*
 * Strawberry Music Player
 * This file was part of Clementine.
 * Copyright 2010, David Sansome <me@davidsansome.com>
 * Copyright 2018-2021, Jonas Kvinge <jonas@jkvinge.net>
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <algorithm>

#include <QtGlobal>
#include <QWidget>
#include <QDialog>
#include <QScreen>
#include <QWindow>
#include <QGuiApplication>
#include <QMainWindow>
#include <QAbstractItemModel>
#include <QtAlgorithms>
#include <QDir>
#include <QList>
#include <QMap>
#include <QDirIterator>
#include <QFileDialog>
#include <QVariant>
#include <QString>
#include <QStringList>
#include <QIcon>
#include <QDateTime>
#include <QComboBox>
#include <QGroupBox>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTreeWidget>
#include <QDialogButtonBox>
#include <QSettings>
#include <QTimerEvent>
#include <QShowEvent>
#include <QCloseEvent>

#include "core/iconloader.h"
#include "core/mainwindow.h"
#include "widgets/fileview.h"
#include "transcodedialog.h"
#include "transcoder.h"
#include "transcoderoptionsdialog.h"
#include "ui_transcodedialog.h"
#include "ui_transcodelogdialog.h"

// winspool.h defines this :(
#ifdef AddJob
#  undef AddJob
#endif

const char *TranscodeDialog::kSettingsGroup = "Transcoder";
const int TranscodeDialog::kProgressInterval = 500;
const int TranscodeDialog::kMaxDestinationItems = 10;

static bool ComparePresetsByName(const TranscoderPreset &left, const TranscoderPreset &right) {
  return left.name_ < right.name_;
}

TranscodeDialog::TranscodeDialog(QMainWindow *mainwindow, QWidget *parent)
    : QDialog(parent),
      mainwindow_(mainwindow),
      ui_(new Ui_TranscodeDialog),
      log_ui_(new Ui_TranscodeLogDialog),
      log_dialog_(new QDialog(this)),
      transcoder_(new Transcoder(this)),
      queued_(0),
      finished_success_(0),
      finished_failed_(0) {

  ui_->setupUi(this);

  setWindowFlags(windowFlags() | Qt::WindowMaximizeButtonHint);

  ui_->files->header()->setSectionResizeMode(QHeaderView::ResizeToContents);

  log_ui_->setupUi(log_dialog_);
  QPushButton *clear_button = log_ui_->buttonBox->addButton(tr("Clear"), QDialogButtonBox::ResetRole);
  QObject::connect(clear_button, &QPushButton::clicked, log_ui_->log, &QPlainTextEdit::clear);

  // Get presets
  QList<TranscoderPreset> presets = Transcoder::GetAllPresets();
  std::sort(presets.begin(), presets.end(), ComparePresetsByName);
  for (const TranscoderPreset &preset : presets) {
    ui_->format->addItem(QString("%1 (.%2)").arg(preset.name_, preset.extension_), QVariant::fromValue(preset));
  }

  // Load settings
  QSettings s;
  s.beginGroup(kSettingsGroup);
  last_add_dir_ = s.value("last_add_dir", QDir::homePath()).toString();
  last_import_dir_ = s.value("last_import_dir", QDir::homePath()).toString();
  QString last_output_format = s.value("last_output_format", "audio/x-vorbis").toString();
  s.endGroup();

  for (int i = 0; i < ui_->format->count(); ++i) {
    if (last_output_format == ui_->format->itemData(i).value<TranscoderPreset>().codec_mimetype_) {
      ui_->format->setCurrentIndex(i);
      break;
    }
  }

  // Add a start button
  start_button_ = ui_->button_box->addButton(tr("Start transcoding"), QDialogButtonBox::ActionRole);
  cancel_button_ = ui_->button_box->button(QDialogButtonBox::Cancel);
  close_button_ = ui_->button_box->button(QDialogButtonBox::Close);

  close_button_->setShortcut(QKeySequence::Close);

  // Hide elements
  cancel_button_->hide();
  ui_->progress_group->hide();

  // Connect stuff
  QObject::connect(ui_->add, &QPushButton::clicked, this, &TranscodeDialog::Add);
  QObject::connect(ui_->import, &QPushButton::clicked, this, &TranscodeDialog::Import);
  QObject::connect(ui_->remove, &QPushButton::clicked, this, &TranscodeDialog::Remove);
  QObject::connect(start_button_, &QPushButton::clicked, this, &TranscodeDialog::Start);
  QObject::connect(cancel_button_, &QPushButton::clicked, this, &TranscodeDialog::Cancel);
  QObject::connect(close_button_, &QPushButton::clicked, this, &TranscodeDialog::hide);
  QObject::connect(ui_->details, &QPushButton::clicked, log_dialog_, &QDialog::show);
  QObject::connect(ui_->options, &QPushButton::clicked, this, &TranscodeDialog::Options);
  QObject::connect(ui_->select, &QPushButton::clicked, this, &TranscodeDialog::AddDestination);

  QObject::connect(transcoder_, &Transcoder::JobComplete, this, &TranscodeDialog::JobComplete);
  QObject::connect(transcoder_, &Transcoder::LogLine, this, &TranscodeDialog::LogLine);
  QObject::connect(transcoder_, &Transcoder::AllJobsComplete, this, &TranscodeDialog::AllJobsComplete);

}

TranscodeDialog::~TranscodeDialog() {
  delete log_ui_;
  delete ui_;
}

void TranscodeDialog::showEvent(QShowEvent *e) {

  if (!e->spontaneous()) LoadGeometry();

  QDialog::showEvent(e);

}

void TranscodeDialog::closeEvent(QCloseEvent *e) {

  SaveGeometry();

  QDialog::closeEvent(e);

}

void TranscodeDialog::accept() {

  SaveGeometry();
  QDialog::accept();

}

void TranscodeDialog::reject() {

  SaveGeometry();
  QDialog::reject();

}

void TranscodeDialog::LoadGeometry() {

  QSettings s;
  s.beginGroup(kSettingsGroup);
  if (s.contains("geometry")) {
    restoreGeometry(s.value("geometry").toByteArray());
  }
  s.endGroup();

  // Center the window on the same screen as the mainwindow.
#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
  QScreen *screen = mainwindow_->screen();
#else
  QScreen *screen = (mainwindow_->window() && mainwindow_->window()->windowHandle() ? mainwindow_->window()->windowHandle()->screen() : nullptr);
#endif
  if (screen) {
    const QRect sr = screen->availableGeometry();
    const QRect wr({}, size().boundedTo(sr.size()));
    resize(wr.size());
    move(sr.center() - wr.center());
  }

}

void TranscodeDialog::SaveGeometry() {

  QSettings s;
  s.beginGroup(kSettingsGroup);
  s.setValue("geometry", saveGeometry());
  s.endGroup();

}

void TranscodeDialog::SetWorking(bool working) {

  start_button_->setVisible(!working);
  cancel_button_->setVisible(working);
  close_button_->setVisible(!working);
  ui_->input_group->setEnabled(!working);
  ui_->output_group->setEnabled(!working);
  ui_->progress_group->setVisible(true);

  if (working)
    progress_timer_.start(kProgressInterval, this);
  else
    progress_timer_.stop();

}

void TranscodeDialog::Start() {

  SetWorking(true);

  QAbstractItemModel *file_model = ui_->files->model();
  TranscoderPreset preset = ui_->format->itemData(ui_->format->currentIndex()).value<TranscoderPreset>();

  // Add jobs to the transcoder
  for (int i = 0; i < file_model->rowCount(); ++i) {
    QString filename = file_model->index(i, 0).data(Qt::UserRole).toString();
    QString outfilename = GetOutputFileName(filename, preset);
    transcoder_->AddJob(filename, preset, outfilename);
  }

  // Set up the progressbar
  ui_->progress_bar->setValue(0);
  ui_->progress_bar->setMaximum(file_model->rowCount() * 100);

  // Reset the UI
  queued_ = file_model->rowCount();
  finished_success_ = 0;
  finished_failed_ = 0;
  UpdateStatusText();

  // Start transcoding
  transcoder_->Start();

  // Save the last output format
  QSettings s;
  s.beginGroup(kSettingsGroup);
  s.setValue("last_output_format", preset.codec_mimetype_);
  s.endGroup();

}

void TranscodeDialog::Cancel() {

  transcoder_->Cancel();
  SetWorking(false);

}

void TranscodeDialog::JobComplete(const QString &input, const QString &output, bool success) {

  Q_UNUSED(input);
  Q_UNUSED(output);

  (*(success ? &finished_success_ : &finished_failed_))++;
  queued_--;

  UpdateStatusText();
  UpdateProgress();

}

void TranscodeDialog::UpdateProgress() {

  int progress = (finished_success_ + finished_failed_) * 100;

  QMap<QString, float> current_jobs = transcoder_->GetProgress();
  QList<float> values = current_jobs.values();
  for (const float value : values) {
    progress += qBound(0, static_cast<int>(value * 100), 99);
  }

  ui_->progress_bar->setValue(progress);

}

void TranscodeDialog::UpdateStatusText() {

  QStringList sections;

  if (queued_) {
    sections << "<font color=\"#3467c8\">" + tr("%n remaining", "", queued_) + "</font>";
  }

  if (finished_success_) {
    sections << "<font color=\"#02b600\">" + tr("%n finished", "", finished_success_) + "</font>";
  }

  if (finished_failed_) {
    sections << "<font color=\"#b60000\">" + tr("%n failed", "", finished_failed_) + "</font>";
  }

  ui_->progress_text->setText(sections.join(", "));

}

void TranscodeDialog::AllJobsComplete() {
  SetWorking(false);
}

void TranscodeDialog::Add() {

  QStringList filenames = QFileDialog::getOpenFileNames(
      this, tr("Add files to transcode"), last_add_dir_,
      QString("%1 (%2);;%3").arg(tr("Music"), FileView::kFileFilter, tr(MainWindow::kAllFilesFilterSpec)));

  if (filenames.isEmpty()) return;

  SetFilenames(filenames);

  last_add_dir_ = filenames[0];
  QSettings s;
  s.beginGroup(kSettingsGroup);
  s.setValue("last_add_dir", last_add_dir_);
  s.endGroup();

}

void TranscodeDialog::Import() {

  QString path = QFileDialog::getExistingDirectory(this, tr("Open a directory to import music from"), last_import_dir_, QFileDialog::ShowDirsOnly);

  if (path.isEmpty()) return;

  QStringList filenames;

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
  QStringList audioTypes = QString(FileView::kFileFilter).split(" ", Qt::SkipEmptyParts);
#else
  QStringList audioTypes = QString(FileView::kFileFilter).split(" ", QString::SkipEmptyParts);
#endif

  QDirIterator files(path, audioTypes, QDir::Files | QDir::Readable, QDirIterator::Subdirectories);

  while (files.hasNext()) {
    filenames << files.next();
  }

  SetFilenames(filenames);

  last_import_dir_ = path;
  QSettings s;
  s.beginGroup(kSettingsGroup);
  s.setValue("last_import_dir", last_import_dir_);
  s.endGroup();

}

void TranscodeDialog::SetFilenames(const QStringList &filenames) {

  for (const QString &filename : filenames) {
    QString name = filename.section('/', -1, -1);
    QString path = filename.section('/', 0, -2);

    QTreeWidgetItem *item = new QTreeWidgetItem(ui_->files, QStringList() << name << path);
    item->setData(0, Qt::UserRole, filename);
  }

}

void TranscodeDialog::Remove() { qDeleteAll(ui_->files->selectedItems()); }

void TranscodeDialog::LogLine(const QString &message) {

  QString date(QDateTime::currentDateTime().toString(Qt::TextDate));
  log_ui_->log->appendPlainText(QString("%1: %2").arg(date, message));

}

void TranscodeDialog::timerEvent(QTimerEvent *e) {

  QDialog::timerEvent(e);

  if (e->timerId() == progress_timer_.timerId()) {
    UpdateProgress();
  }

}

void TranscodeDialog::Options() {

  TranscoderPreset preset = ui_->format->itemData(ui_->format->currentIndex()).value<TranscoderPreset>();

  TranscoderOptionsDialog dialog(preset.filetype_, this);
  if (dialog.is_valid()) {
    dialog.exec();
  }

}

// Adds a folder to the destination box.
void TranscodeDialog::AddDestination() {

  int index = ui_->destination->currentIndex();
  QString initial_dir = (!ui_->destination->itemData(index).isNull() ? ui_->destination->itemData(index).toString() : QDir::homePath());
  QString dir = QFileDialog::getExistingDirectory(this, tr("Add folder"), initial_dir);

  if (!dir.isEmpty()) {
    // Keep only a finite number of items in the box.
    while (ui_->destination->count() >= kMaxDestinationItems) {
      ui_->destination->removeItem(1);  // The oldest folder item.
    }

    QIcon icon = IconLoader::Load("folder");
    QVariant data_var = QVariant::fromValue(dir);
    // Do not insert duplicates.
    int duplicate_index = ui_->destination->findData(data_var);
    if (duplicate_index == -1) {
      ui_->destination->addItem(icon, dir, data_var);
      ui_->destination->setCurrentIndex(ui_->destination->count() - 1);
    }
    else {
      ui_->destination->setCurrentIndex(duplicate_index);
    }
  }

}

// Returns the rightmost non-empty part of 'path'.
QString TranscodeDialog::TrimPath(const QString &path) {
  return path.section('/', -1, -1, QString::SectionSkipEmpty);
}

QString TranscodeDialog::GetOutputFileName(const QString &input, const TranscoderPreset &preset) const {

  QString path = ui_->destination->itemData(ui_->destination->currentIndex()).toString();
  if (path.isEmpty()) {
    // Keep the original path.
    return input.section('.', 0, -2) + '.' + preset.extension_;
  }
  else {
    QString file_name = TrimPath(input);
    file_name = file_name.section('.', 0, -2);
    return path + '/' + file_name + '.' + preset.extension_;
  }

}
