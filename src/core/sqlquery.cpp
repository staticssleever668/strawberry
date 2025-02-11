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

#include "config.h"

#include <QSqlQuery>
#include <QMap>
#include <QVariant>
#include <QVariantList>
#include <QString>
#include <QUrl>

#include "sqlquery.h"

void SqlQuery::BindValue(const QString &placeholder, const QVariant &value) {

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  bound_values_.insert(placeholder, value);
#endif

  bindValue(placeholder, value);

}

void SqlQuery::BindStringValue(const QString &placeholder, const QString &value) {

  BindValue(placeholder, value.isNull() ? "" : value);

}

void SqlQuery::BindUrlValue(const QString &placeholder, const QUrl &value) {

  BindValue(placeholder, value.isValid() ? value.toString(QUrl::FullyEncoded) : "");

}

void SqlQuery::BindIntValue(const QString &placeholder, const int value) {

  BindValue(placeholder, value <= 0 ? -1 : value);

}

void SqlQuery::BindLongLongValue(const QString &placeholder, const qint64 value) {

  BindValue(placeholder, value <= 0 ? -1 : value);

}

void SqlQuery::BindFloatValue(const QString &placeholder, const float value) {

  BindValue(placeholder, value <= 0 ? -1 : value);

}

void SqlQuery::BindBoolValue(const QString &placeholder, const bool value) {

  BindValue(placeholder, value ? 1 : 0);

}

void SqlQuery::BindNotNullIntValue(const QString &placeholder, const int value) {

  BindValue(placeholder, value == -1 ? QVariant() : value);

}

void SqlQuery::BindNotNullLongLongValue(const QString &placeholder, const qint64 value) {

  BindValue(placeholder, value == -1 ? QVariant() : value);

}

bool SqlQuery::Exec() {

  bool success = exec();
  last_query_ = executedQuery();

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  for (QMap<QString, QVariant>::const_iterator it = bound_values_.constBegin(); it != bound_values_.constEnd(); ++it) {
    last_query_.replace(it.key(), it.value().toString());
  }
  bound_values_.clear();
#else
  QMapIterator<QString, QVariant> it(boundValues());
  while (it.hasNext()) {
    it.next();
    last_query_.replace(it.key(), it.value().toString());
  }
#endif

  return success;

}

QString SqlQuery::LastQuery() const {

  return last_query_;

}
