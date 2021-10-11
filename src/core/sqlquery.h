/*
 * Strawberry Music Player
 * Copyright 2021, Jonas Kvinge <jonas@jkvinge.net>
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

#ifndef SQLQUERY_H
#define SQLQUERY_H

#include "config.h"

#include <QMap>
#include <QVariant>
#include <QString>
#include <QSqlDatabase>
#include <QSqlQuery>

class SqlQuery : public QSqlQuery {

 public:
  explicit SqlQuery(const QSqlDatabase& db) : QSqlQuery(db) {}

  void BindValue(const QString &placeholder, const QVariant &value);
  bool Exec();
  QString LastQuery() const;

 private:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  QMap<QString, QVariant> bound_values_;
#endif
  QString last_query_;

};

#endif  // SQLQUERY_H
