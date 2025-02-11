/*
 * Strawberry Music Player
 * Copyright 2020-2022, Jonas Kvinge <jonas@jkvinge.net>
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

#include <memory>

#include <QtGlobal>
#include <QObject>
#include <QWidget>
#include <QByteArray>
#include <QImage>
#include <QPixmap>
#include <QPalette>
#include <QBrush>
#include <QMovie>
#include <QTimeLine>
#include <QPainter>
#include <QSizePolicy>
#include <QMenu>
#include <QContextMenuEvent>
#include <QPaintEvent>

#include "core/imageutils.h"
#include "covermanager/albumcoverchoicecontroller.h"

#include "contextview.h"
#include "contextalbum.h"

const int ContextAlbum::kFadeTimeLineMs = 1000;

ContextAlbum::ContextAlbum(QWidget *parent)
    : QWidget(parent),
      menu_(new QMenu(this)),
      context_view_(nullptr),
      album_cover_choice_controller_(nullptr),
      downloading_covers_(false),
      timeline_fade_(new QTimeLine(kFadeTimeLineMs, this)),
      image_strawberry_(":/pictures/strawberry.png"),
      image_original_(image_strawberry_),
      pixmap_current_opacity_(1.0) {

  setObjectName("context-widget-album");

  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

  cover_loader_options_.desired_height_ = width();
  cover_loader_options_.pad_output_image_ = true;
  cover_loader_options_.scale_output_image_ = true;
  QImage image = ImageUtils::ScaleAndPad(image_strawberry_, cover_loader_options_.scale_output_image_, cover_loader_options_.pad_output_image_, cover_loader_options_.desired_height_);
  if (!image.isNull()) {
    pixmap_current_ = QPixmap::fromImage(image);
  }

  timeline_fade_->setDirection(QTimeLine::Forward);
  QObject::connect(timeline_fade_, &QTimeLine::valueChanged, this, &ContextAlbum::FadeCurrentCover);
  QObject::connect(timeline_fade_, &QTimeLine::finished, this, &ContextAlbum::FadeCurrentCoverFinished);

}

void ContextAlbum::Init(ContextView *context_view, AlbumCoverChoiceController *album_cover_choice_controller) {

  context_view_ = context_view;

  album_cover_choice_controller_ = album_cover_choice_controller;
  QObject::connect(album_cover_choice_controller_, &AlbumCoverChoiceController::AutomaticCoverSearchDone, this, &ContextAlbum::AutomaticCoverSearchDone);

  QList<QAction*> cover_actions = album_cover_choice_controller_->GetAllActions();
  menu_->addActions(cover_actions);
  menu_->addSeparator();
  menu_->addAction(album_cover_choice_controller_->search_cover_auto_action());
  menu_->addSeparator();

}

QSize ContextAlbum::sizeHint() const {

  return QSize(pixmap_current_.width(), pixmap_current_.height());

}

void ContextAlbum::paintEvent(QPaintEvent*) {

  QPainter p(this);
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  DrawPreviousCovers(&p);
  DrawImage(&p, pixmap_current_, pixmap_current_opacity_);
  DrawSpinner(&p);
  p.end();

}

void ContextAlbum::mouseDoubleClickEvent(QMouseEvent *e) {

  // Same behaviour as right-click > Show Fullsize
  if (image_original_ != image_strawberry_ && e->button() == Qt::LeftButton && context_view_->song_playing().is_valid()) {
    album_cover_choice_controller_->ShowCover(context_view_->song_playing(), image_original_);
  }

}

void ContextAlbum::contextMenuEvent(QContextMenuEvent *e) {

  if (menu_ && image_original_ != image_strawberry_) {
    menu_->popup(mapToGlobal(e->pos()));
  }
  else {
    QWidget::contextMenuEvent(e);
  }

}

void ContextAlbum::UpdateWidth(const int new_width) {

  if (new_width != cover_loader_options_.desired_height_) {
    cover_loader_options_.desired_height_ = new_width;
    ScaleCover();
    ScalePreviousCovers();
    updateGeometry();
  }

}

void ContextAlbum::SetImage(QImage image) {

  if (image.isNull()) {
    image = image_strawberry_;
  }

  if (downloading_covers_) {
    downloading_covers_ = false;
    spinner_animation_.reset();
  }

  QImage image_previous = image_original_;
  QPixmap pixmap_previous = pixmap_current_;
  qreal opacity_previous = pixmap_current_opacity_;

  image_original_ = image;
  pixmap_current_opacity_ = 0.0;
  ScaleCover();

  if (!pixmap_previous.isNull()) {
    std::shared_ptr<PreviousCover> previous_cover = std::make_shared<PreviousCover>();
    previous_cover->image = image_previous;
    previous_cover->pixmap =  pixmap_previous;
    previous_cover->opacity = opacity_previous;
    previous_cover->timeline.reset(new QTimeLine(kFadeTimeLineMs), [](QTimeLine *timeline) { timeline->deleteLater(); });
    previous_cover->timeline->setDirection(QTimeLine::Backward);
    previous_cover->timeline->setCurrentTime(timeline_fade_->state() == QTimeLine::Running ? timeline_fade_->currentTime() : kFadeTimeLineMs);
    QObject::connect(previous_cover->timeline.get(), &QTimeLine::valueChanged, this, [this, previous_cover]() { FadePreviousCover(previous_cover); });
    QObject::connect(previous_cover->timeline.get(), &QTimeLine::finished, this, [this, previous_cover]() { FadePreviousCoverFinished(previous_cover); });
    previous_covers_ << previous_cover;
    previous_cover->timeline->start();
  }

  if (timeline_fade_->state() == QTimeLine::Running) {
    timeline_fade_->stop();
  }
  timeline_fade_->start();

}

void ContextAlbum::DrawImage(QPainter *p, const QPixmap &pixmap, const qreal opacity) {

  if (qFuzzyCompare(opacity, static_cast<qreal>(0.0))) return;

  p->setOpacity(opacity);
  p->drawPixmap(0, 0, pixmap.width(), pixmap.height(), pixmap);

}

void ContextAlbum::DrawSpinner(QPainter *p) {

  if (downloading_covers_) {
    p->drawPixmap(50, 50, 16, 16, spinner_animation_->currentPixmap());
  }

}

void ContextAlbum::DrawPreviousCovers(QPainter *p) {

  for (std::shared_ptr<PreviousCover> previous_cover : previous_covers_) {
    DrawImage(p, previous_cover->pixmap, previous_cover->opacity);
  }

}

void ContextAlbum::FadeCurrentCover(const qreal value) {

  if (value <= pixmap_current_opacity_) return;

  pixmap_current_opacity_ = value;
  update();

}

void ContextAlbum::FadeCurrentCoverFinished() {

  if (image_original_ == image_strawberry_) {
    emit FadeStopFinished();
  }

}

void ContextAlbum::FadePreviousCover(std::shared_ptr<PreviousCover> previous_cover) {

  if (previous_cover->timeline->currentValue() >= previous_cover->opacity) return;

  previous_cover->opacity = previous_cover->timeline->currentValue();

}

void ContextAlbum::FadePreviousCoverFinished(std::shared_ptr<PreviousCover> previous_cover) {

  previous_covers_.removeAll(previous_cover);

}

void ContextAlbum::ScaleCover() {

  QImage image = ImageUtils::ScaleAndPad(image_original_, cover_loader_options_.scale_output_image_, cover_loader_options_.pad_output_image_, cover_loader_options_.desired_height_);
  if (image.isNull()) {
    pixmap_current_ = QPixmap();
  }
  else {
    pixmap_current_ = QPixmap::fromImage(image);
  }

}

void ContextAlbum::ScalePreviousCovers() {

  for (std::shared_ptr<PreviousCover> previous_cover : previous_covers_) {
    QImage image = ImageUtils::ScaleAndPad(previous_cover->image, cover_loader_options_.scale_output_image_, cover_loader_options_.pad_output_image_, cover_loader_options_.desired_height_);
    if (image.isNull()) {
      previous_cover->pixmap = QPixmap();
    }
    else {
      previous_cover->pixmap = QPixmap::fromImage(image);
    }
  }

}

void ContextAlbum::SearchCoverInProgress() {

  downloading_covers_ = true;

  // Show a spinner animation
  spinner_animation_ = std::make_unique<QMovie>(":/pictures/spinner.gif", QByteArray(), this);
  QObject::connect(spinner_animation_.get(), &QMovie::updated, this, &ContextAlbum::Update);
  spinner_animation_->start();
  update();

}

void ContextAlbum::AutomaticCoverSearchDone() {

  downloading_covers_ = false;
  spinner_animation_.reset();
  update();

}
