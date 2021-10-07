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

#include <optional>

#include <QtGlobal>
#include <QObject>
#include <QApplication>
#include <QThread>
#include <QMutex>
#include <QSet>
#include <QMap>
#include <QVector>
#include <QVariant>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QFileInfo>
#include <QDateTime>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>

#include "core/logging.h"
#include "core/database.h"
#include "core/scopedtransaction.h"
#include "core/song.h"
#include "smartplaylists/smartplaylistsearch.h"

#include "directory.h"
#include "sqlrow.h"
#include "collectionbackend.h"
#include "collectionquery.h"
#include "collectiontask.h"

CollectionBackend::CollectionBackend(QObject *parent)
    : CollectionBackendInterface(parent),
      db_(nullptr),
      task_manager_(nullptr),
      source_(Song::Source_Unknown),
      original_thread_(nullptr) {

  original_thread_ = thread();

}

void CollectionBackend::Init(Database *db, TaskManager *task_manager, const Song::Source source, const QString &songs_table, const QString &dirs_table, const QString &subdirs_table) {
  db_ = db;
  task_manager_ = task_manager;
  source_ = source;
  songs_table_ = songs_table;
  dirs_table_ = dirs_table;
  subdirs_table_ = subdirs_table;
}

void CollectionBackend::Close() {

  if (db_) {
    QMutexLocker l(db_->Mutex());
    db_->Close();
  }

}

void CollectionBackend::ExitAsync() {
  QMetaObject::invokeMethod(this, "Exit", Qt::QueuedConnection);
}

void CollectionBackend::Exit() {

  Q_ASSERT(QThread::currentThread() == thread());

  moveToThread(original_thread_);
  emit ExitFinished();

}

void CollectionBackend::ReportErrors(const CollectionQuery &query) {

  const QSqlError sql_error = query.lastError();
  if (sql_error.isValid()) {
    qLog(Error) << "Unable to execute collection SQL query: " << sql_error;
    qLog(Error) << "Faulty SQL query: " << query.lastQuery();
    qLog(Error) << "Bound SQL values: " << query.boundValues();
    QString error;
    error += "Unable to execute collection SQL query: " + sql_error.text() + "<br />";
    error += "Faulty SQL query: " + query.lastQuery();
    emit Error(error);
  }

}

void CollectionBackend::GetAllSongsAsync(const int id) {
  metaObject()->invokeMethod(this, "GetAllSongs", Qt::QueuedConnection, Q_ARG(int, id));
}

void CollectionBackend::GetAllSongs(const int id) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  QSqlQuery q(db);
  q.setForwardOnly(true);
  q.prepare(QString("SELECT ROWID, " + Song::kColumnSpec + " FROM %1").arg(songs_table_));
  q.exec();
  if (db_->CheckErrors(q)) emit GotSongs(SongList(), id);

  SongList songs;
  while (q.next()) {
    Song song(source_);
    song.InitFromQuery(q, true);
    songs << song;
  }

  emit GotSongs(songs, id);

}

void CollectionBackend::LoadDirectoriesAsync() {
  QMetaObject::invokeMethod(this, "LoadDirectories", Qt::QueuedConnection);
}

void CollectionBackend::UpdateTotalSongCountAsync() {
  QMetaObject::invokeMethod(this, "UpdateTotalSongCount", Qt::QueuedConnection);
}

void CollectionBackend::UpdateTotalArtistCountAsync() {
  QMetaObject::invokeMethod(this, "UpdateTotalArtistCount", Qt::QueuedConnection);
}

void CollectionBackend::UpdateTotalAlbumCountAsync() {
  QMetaObject::invokeMethod(this, "UpdateTotalAlbumCount", Qt::QueuedConnection);
}

void CollectionBackend::IncrementPlayCountAsync(const int id) {
  QMetaObject::invokeMethod(this, "IncrementPlayCount", Qt::QueuedConnection, Q_ARG(int, id));
}

void CollectionBackend::IncrementSkipCountAsync(const int id, const float progress) {
  QMetaObject::invokeMethod(this, "IncrementSkipCount", Qt::QueuedConnection, Q_ARG(int, id), Q_ARG(float, progress));
}

void CollectionBackend::ResetStatisticsAsync(const int id) {
  QMetaObject::invokeMethod(this, "ResetStatistics", Qt::QueuedConnection, Q_ARG(int, id));
}

void CollectionBackend::LoadDirectories() {

  DirectoryList dirs = GetAllDirectories();

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  for (const Directory &dir : dirs) {
    emit DirectoryDiscovered(dir, SubdirsInDirectory(dir.id, db));
  }

}

void CollectionBackend::ChangeDirPath(const int id, const QString &old_path, const QString &new_path) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());
  ScopedTransaction t(&db);

  // Do the dirs table
  {
    SqlQuery q(db);
    q.prepare(QString("UPDATE %1 SET path=:path WHERE ROWID=:id").arg(dirs_table_));
    q.BindValue(":path", new_path);
    q.BindValue(":id", id);
    if (!q.Exec()) {
      db_->ReportErrors(q);
      return;
    }
  }

  const QByteArray old_url = QUrl::fromLocalFile(old_path).toEncoded();
  const QByteArray new_url = QUrl::fromLocalFile(new_path).toEncoded();

  const int path_len = old_url.length();

  // Do the subdirs table
  {
    SqlQuery q(db);
    q.prepare(QString("UPDATE %1 SET path=:path || substr(path, %2) WHERE directory=:id").arg(subdirs_table_).arg(path_len));
    q.BindValue(":path", new_url);
    q.BindValue(":id", id);
    if (!q.Exec()) {
      db_->ReportErrors(q);
      return;
    }
  }

  // Do the songs table
  {
    SqlQuery q(db);
    q.prepare(QString("UPDATE %1 SET url=:path || substr(url, %2) WHERE directory=:id").arg(songs_table_).arg(path_len));
    q.BindValue(":path", new_url);
    q.BindValue(":id", id);
    if (!q.Exec()) {
      db_->ReportErrors(q);
      return;
    }
  }

  t.Commit();

}

DirectoryList CollectionBackend::GetAllDirectories() {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  DirectoryList ret;

  SqlQuery q(db);
  q.prepare(QString("SELECT ROWID, path FROM %1").arg(dirs_table_));
  if (!q.Exec()) {
    db_->ReportErrors(q);
    return ret;
  }

  while (q.next()) {
    Directory dir;
    dir.id = q.value(0).toInt();
    dir.path = q.value(1).toString();

    ret << dir;
  }
  return ret;

}

SubdirectoryList CollectionBackend::SubdirsInDirectory(const int id) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db = db_->Connect();
  return SubdirsInDirectory(id, db);

}

SubdirectoryList CollectionBackend::SubdirsInDirectory(const int id, QSqlDatabase &db) {

  SqlQuery q(db);
  q.prepare(QString("SELECT path, mtime FROM %1 WHERE directory_id = :dir").arg(subdirs_table_));
  q.BindValue(":dir", id);
  if (!q.Exec()) {
    db_->ReportErrors(q);
    return SubdirectoryList();
  }

  SubdirectoryList subdirs;
  while (q.next()) {
    Subdirectory subdir;
    subdir.directory_id = id;
    subdir.path = q.value(0).toString();
    subdir.mtime = q.value(1).toLongLong();
    subdirs << subdir;
  }

  return subdirs;

}

void CollectionBackend::UpdateTotalSongCount() {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  SqlQuery q(db);
  q.prepare(QString("SELECT COUNT(*) FROM %1 WHERE unavailable = 0").arg(songs_table_));
  if (!q.Exec()) {
    db_->ReportErrors(q);
    return;
  }
  if (!q.next()) {
    db_->ReportErrors(q);
    return;
  }

  emit TotalSongCountUpdated(q.value(0).toInt());

}

void CollectionBackend::UpdateTotalArtistCount() {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  SqlQuery q(db);
  q.prepare(QString("SELECT COUNT(DISTINCT artist) FROM %1 WHERE unavailable = 0").arg(songs_table_));
  if (!q.Exec()) {
    db_->ReportErrors(q);
    return;
  }
  if (!q.next()) {
    db_->ReportErrors(q);
    return;
  }

  emit TotalArtistCountUpdated(q.value(0).toInt());

}

void CollectionBackend::UpdateTotalAlbumCount() {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  SqlQuery q(db);
  q.prepare(QString("SELECT COUNT(*) FROM (SELECT DISTINCT effective_albumartist, album FROM %1 WHERE unavailable = 0)").arg(songs_table_));
  if (!q.Exec()) {
    db_->ReportErrors(q);
    return;
  }
  if (!q.next()) {
    db_->ReportErrors(q);
    return;
  }

  emit TotalAlbumCountUpdated(q.value(0).toInt());

}

void CollectionBackend::AddDirectory(const QString &path) {

  QString canonical_path = QFileInfo(path).canonicalFilePath();
  QString db_path = canonical_path;

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  SqlQuery q(db);
  q.prepare(QString("INSERT INTO %1 (path, subdirs) VALUES (:path, 1)").arg(dirs_table_));
  q.BindValue(":path", db_path);
  if (!q.Exec()) {
   db_->ReportErrors(q);
   return;
  }

  Directory dir;
  dir.path = canonical_path;
  dir.id = q.lastInsertId().toInt();

  emit DirectoryDiscovered(dir, SubdirectoryList());

}

void CollectionBackend::RemoveDirectory(const Directory &dir) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  // Remove songs first
  DeleteSongs(FindSongsInDirectory(dir.id));

  ScopedTransaction transaction(&db);

  // Delete the subdirs that were in this directory
  {
    SqlQuery q(db);
    q.prepare(QString("DELETE FROM %1 WHERE directory_id = :id").arg(subdirs_table_));
    q.BindValue(":id", dir.id);
    if (!q.Exec()) {
      db_->ReportErrors(q);
      return;
    }
  }

  // Now remove the directory itself
  {
    SqlQuery q(db);
    q.prepare(QString("DELETE FROM %1 WHERE ROWID = :id").arg(dirs_table_));
    q.BindValue(":id", dir.id);
    if (!q.Exec()) {
      db_->ReportErrors(q);
      return;
    }
  }

  emit DirectoryDeleted(dir);

  transaction.Commit();

}

SongList CollectionBackend::FindSongsInDirectory(const int id) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  SqlQuery q(db);
  q.prepare(QString("SELECT ROWID, " + Song::kColumnSpec + " FROM %1 WHERE directory_id = :directory_id").arg(songs_table_));
  q.BindValue(":directory_id", id);
  if (!q.Exec()) {
    db_->ReportErrors(q);
    return SongList();
  }

  SongList ret;
  while (q.next()) {
    Song song(source_);
    song.InitFromQuery(q, true);
    ret << song;
  }
  return ret;

}

SongList CollectionBackend::SongsWithMissingFingerprint(const int id) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  SqlQuery q(db);
  q.prepare(QString("SELECT ROWID, " + Song::kColumnSpec + " FROM %1 WHERE directory_id = :directory_id AND unavailable = 0 AND (fingerprint IS NULL OR fingerprint = '')").arg(songs_table_));
  q.BindValue(":directory_id", id);
  if (!q.Exec()) {
    db_->ReportErrors(q);
    return SongList();
  }

  SongList ret;
  while (q.next()) {
    Song song(source_);
    song.InitFromQuery(q, true);
    ret << song;
  }
  return ret;

}

void CollectionBackend::SongPathChanged(const Song &song, const QFileInfo &new_file, const std::optional<int> new_collection_directory_id) {

  // Take a song and update its path
  Song updated_song = song;
  updated_song.set_source(source_);
  updated_song.set_url(QUrl::fromLocalFile(new_file.absoluteFilePath()));
  updated_song.set_basefilename(new_file.fileName());
  updated_song.InitArtManual();
  if (updated_song.is_collection_song() && new_collection_directory_id) {
    updated_song.set_directory_id(new_collection_directory_id.value());
  }

  AddOrUpdateSongs(SongList() << updated_song);

}

void CollectionBackend::AddOrUpdateSubdirs(const SubdirectoryList &subdirs) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  ScopedTransaction transaction(&db);
  for (const Subdirectory &subdir : subdirs) {
    if (subdir.mtime == 0) {
      // Delete the subdirectory
      SqlQuery q(db);
      q.prepare(QString("DELETE FROM %1 WHERE directory_id = :id AND path = :path").arg(subdirs_table_));
      q.BindValue(":id", subdir.directory_id);
      q.BindValue(":path", subdir.path);
      if (!q.Exec()) {
        db_->ReportErrors(q);
        return;
      }
    }
    else {
      // See if this subdirectory already exists in the database
      bool exists = false;
      {
        SqlQuery q(db);
        q.prepare(QString("SELECT ROWID FROM %1 WHERE directory_id = :id AND path = :path").arg(subdirs_table_));
        q.BindValue(":id", subdir.directory_id);
        q.BindValue(":path", subdir.path);
        if (!q.Exec()) {
          db_->ReportErrors(q);
          return;
        }
        exists = q.next();
      }

      if (exists) {
        SqlQuery q(db);
        q.prepare(QString("UPDATE %1 SET mtime = :mtime WHERE directory_id = :id AND path = :path").arg(subdirs_table_));
        q.BindValue(":mtime", subdir.mtime);
        q.BindValue(":id", subdir.directory_id);
        q.BindValue(":path", subdir.path);
        if (!q.Exec()) {
          db_->ReportErrors(q);
          return;
        }
      }
      else {
        SqlQuery q(db);
        q.prepare(QString("INSERT INTO %1 (directory_id, path, mtime) VALUES (:id, :path, :mtime)").arg(subdirs_table_));
        q.BindValue(":id", subdir.directory_id);
        q.BindValue(":path", subdir.path);
        q.BindValue(":mtime", subdir.mtime);
        if (!q.Exec()) {
          db_->ReportErrors(q);
          return;
        }
      }
    }
  }

  transaction.Commit();

}

void CollectionBackend::AddOrUpdateSongsAsync(const SongList &songs) {
  QMetaObject::invokeMethod(this, "AddOrUpdateSongs", Qt::QueuedConnection, Q_ARG(SongList, songs));
}

void CollectionBackend::AddOrUpdateSongs(const SongList &songs) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  ScopedTransaction transaction(&db);

  SongList added_songs;
  SongList deleted_songs;

  for (const Song &song : songs) {

    // Do a sanity check first - make sure the song's directory still exists
    // This is to fix a possible race condition when a directory is removed while CollectionWatcher is scanning it.
    if (!dirs_table_.isEmpty()) {
      SqlQuery check_dir(db);
      check_dir.prepare(QString("SELECT ROWID FROM %1 WHERE ROWID = :id").arg(dirs_table_));
      check_dir.BindValue(":id", song.directory_id());
      if (!check_dir.Exec()) {
        db_->ReportErrors(check_dir);
        return;
      }

      if (!check_dir.next()) continue;

    }

    if (song.id() != -1) {  // This song exists in the DB.

      // Get the previous song data first
      Song old_song(GetSongById(song.id()));
      if (!old_song.is_valid()) continue;

      // Update
      {
        SqlQuery q(db);
        q.prepare(QString("UPDATE %1 SET " + Song::kUpdateSpec + " WHERE ROWID = :id").arg(songs_table_));
        song.BindToQuery(&q);
        q.BindValue(":id", song.id());
        if (!q.Exec()) {
          db_->ReportErrors(q);
          return;
        }
      }

      {
        SqlQuery q(db);
        q.prepare(QString("UPDATE %1 SET " + Song::kFtsUpdateSpec + " WHERE ROWID = :id").arg(fts_table_));
        song.BindToFtsQuery(&q);
        q.BindValue(":id", song.id());
        if (!q.Exec()) {
          db_->ReportErrors(q);
          return;
        }
      }

      deleted_songs << old_song;
      added_songs << song;

      continue;

    }
    else if (!song.song_id().isEmpty()) {  // Song has a unique id, check if the song exists.

      // Get the previous song data first
      Song old_song(GetSongBySongId(song.song_id()));

      if (old_song.is_valid() && old_song.id() != -1) {

        Song new_song = song;
        new_song.set_id(old_song.id());

        // Update
        {
          SqlQuery q(db);
          q.prepare(QString("UPDATE %1 SET " + Song::kUpdateSpec + " WHERE ROWID = :id").arg(songs_table_));
          new_song.BindToQuery(&q);
          q.BindValue(":id", new_song.id());
          if (!q.Exec()) {
            db_->ReportErrors(q);
            return;
          }
        }

        deleted_songs << old_song;
        added_songs << new_song;

        continue;

      }

    }

    // Create new song

    int id = -1;
    { // Insert the row and create a new ID
      SqlQuery q(db);
      q.prepare(QString("INSERT INTO %1 (" + Song::kColumnSpec + ") VALUES (" + Song::kBindSpec + ")").arg(songs_table_));
      song.BindToQuery(&q);
      if (!q.Exec()) {
        db_->ReportErrors(q);
        return;
      }
      // Get the new ID
      id = q.lastInsertId().toInt();
    }

    if (id == -1) return;

    Song song_copy(song);
    song_copy.set_id(id);
    added_songs << song_copy;

  }

  transaction.Commit();

  if (!deleted_songs.isEmpty()) emit SongsDeleted(deleted_songs);
  if (!added_songs.isEmpty()) emit SongsDiscovered(added_songs);

  UpdateTotalSongCountAsync();
  UpdateTotalArtistCountAsync();
  UpdateTotalAlbumCountAsync();

}

void CollectionBackend::UpdateSongsBySongIDAsync(const SongMap &new_songs) {
  QMetaObject::invokeMethod(this, "UpdateSongsBySongID", Qt::QueuedConnection, Q_ARG(SongMap, new_songs));
}

void CollectionBackend::UpdateSongsBySongID(const SongMap &new_songs) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  CollectionTask task(task_manager_, tr("Updating %1 database.").arg(Song::TextForSource(source_)));
  ScopedTransaction transaction(&db);

  SongList added_songs;
  SongList deleted_songs;

  SongMap old_songs;
  {
    CollectionQuery query(db, songs_table_, fts_table_);
    if (!ExecCollectionQuery(&query, old_songs)) {
      ReportErrors(query);
      return;
    }
  }

  // Add or update songs.
  for (const Song &new_song : new_songs) {
    if (old_songs.contains(new_song.song_id())) {

      Song old_song = old_songs[new_song.song_id()];

      if (!new_song.IsMetadataEqual(old_song)) {  // Update existing song.

        {
          SqlQuery q(db);
          q.prepare(QString("UPDATE %1 SET " + Song::kUpdateSpec + " WHERE ROWID = :id").arg(songs_table_));
          new_song.BindToQuery(&q);
          q.BindValue(":id", old_song.id());
          if (!q.Exec()) {
            db_->ReportErrors(q);
            return;
          }
        }
        {
          SqlQuery q(db);
          q.prepare(QString("UPDATE %1 SET " + Song::kFtsUpdateSpec + " WHERE ROWID = :id").arg(fts_table_));
          new_song.BindToFtsQuery(&q);
          q.BindValue(":id", old_song.id());
          if (!q.Exec()) {
            db_->ReportErrors(q);
            return;
          }
        }

        deleted_songs << old_song;
        Song new_song_copy(new_song);
        new_song_copy.set_id(old_song.id());
        added_songs << new_song_copy;

      }

    }
    else {  // Add new song
      int id = -1;
      {
        SqlQuery q(db);
        q.prepare(QString("INSERT INTO %1 (" + Song::kColumnSpec + ") VALUES (" + Song::kBindSpec + ")").arg(songs_table_));
        new_song.BindToQuery(&q);
        if (!q.Exec()) {
          db_->ReportErrors(q);
          return;
        }
        // Get the new ID
        id = q.lastInsertId().toInt();
      }

      if (id == -1) return;

      {  // Add to the FTS index
        SqlQuery q(db);
        q.prepare(QString("INSERT INTO %1 (ROWID, " + Song::kFtsColumnSpec + ") VALUES (:id, " + Song::kFtsBindSpec + ")").arg(fts_table_));
        q.BindValue(":id", id);
        new_song.BindToFtsQuery(&q);
        if (!q.Exec()) {
          db_->ReportErrors(q);
          return;
        }
      }

      Song new_song_copy(new_song);
      new_song_copy.set_id(id);
      added_songs << new_song_copy;
    }
  }

  // Delete songs
  for (const Song &old_song : old_songs) {
    if (!new_songs.contains(old_song.song_id())) {
      {
        SqlQuery q(db);
        q.prepare(QString("DELETE FROM %1 WHERE ROWID = :id").arg(songs_table_));
        q.BindValue(":id", old_song.id());
        if (!q.Exec()) {
          db_->ReportErrors(q);
          return;
        }
      }
      {
        SqlQuery q(db);
        q.prepare(QString("DELETE FROM %1 WHERE ROWID = :id").arg(fts_table_));
        q.BindValue(":id", old_song.id());
        if (!q.Exec()) {
          db_->ReportErrors(q);
          return;
        }
      }
      deleted_songs << old_song;
    }
  }

  transaction.Commit();

  if (!deleted_songs.isEmpty()) emit SongsDeleted(deleted_songs);
  if (!added_songs.isEmpty()) emit SongsDiscovered(added_songs);

  UpdateTotalSongCountAsync();
  UpdateTotalArtistCountAsync();
  UpdateTotalAlbumCountAsync();

}

void CollectionBackend::UpdateMTimesOnly(const SongList &songs) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  SqlQuery q(db);
  q.prepare(QString("UPDATE %1 SET mtime = :mtime WHERE ROWID = :id").arg(songs_table_));

  ScopedTransaction transaction(&db);
  for (const Song &song : songs) {
    q.BindValue(":mtime", song.mtime());
    q.BindValue(":id", song.id());
    if (!q.Exec()) {
      db_->ReportErrors(q);
      return;
    }
  }
  transaction.Commit();

}

void CollectionBackend::DeleteSongs(const SongList &songs) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  SqlQuery remove(db);
  remove.prepare(QString("DELETE FROM %1 WHERE ROWID = :id").arg(songs_table_));

  ScopedTransaction transaction(&db);
  for (const Song &song : songs) {
    remove.BindValue(":id", song.id());
    if (!remove.Exec()) {
      db_->ReportErrors(remove);
      return;
    }

  ScopedTransaction transaction(&db);
  for (const Song &song : songs) {
    remove.bindValue(":id", song.id());
    remove.exec();
    db_->CheckErrors(remove);
  }
  transaction.Commit();

  emit SongsDeleted(songs);

  UpdateTotalSongCountAsync();
  UpdateTotalArtistCountAsync();
  UpdateTotalAlbumCountAsync();

}

void CollectionBackend::MarkSongsUnavailable(const SongList &songs, const bool unavailable) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  SqlQuery remove(db);
  remove.prepare(QString("UPDATE %1 SET unavailable = %2 WHERE ROWID = :id").arg(songs_table_).arg(int(unavailable)));

  ScopedTransaction transaction(&db);
  for (const Song &song : songs) {
    remove.BindValue(":id", song.id());
    if (!remove.Exec()) {
      db_->ReportErrors(remove);
      return;
    }
  }
  transaction.Commit();

  if (unavailable) {
    emit SongsDeleted(songs);
  }
  else {
    emit SongsDiscovered(songs);
  }

  UpdateTotalSongCountAsync();
  UpdateTotalArtistCountAsync();
  UpdateTotalAlbumCountAsync();

}

QStringList CollectionBackend::GetAll(const QString &column, const QueryOptions &opt) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  CollectionQuery query(db, songs_table_, opt);
  query.SetColumnSpec("DISTINCT " + column);
  query.AddCompilationRequirement(false);

  if (!query.Exec()) {
    ReportErrors(query);
    return QStringList();
  }

  QStringList ret;
  while (query.Next()) {
    ret << query.Value(0).toString();
  }
  return ret;

}

QStringList CollectionBackend::GetAllArtists(const QueryOptions &opt) {

  return GetAll("artist", opt);
}

QStringList CollectionBackend::GetAllArtistsWithAlbums(const QueryOptions &opt) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  // Albums with 'albumartist' field set:
  CollectionQuery query(db, songs_table_, opt);
  query.SetColumnSpec("DISTINCT albumartist");
  query.AddCompilationRequirement(false);
  query.AddWhere("album", "", "!=");

  // Albums with no 'albumartist' (extract 'artist'):
  CollectionQuery query2(db, songs_table_, opt);
  query2.SetColumnSpec("DISTINCT artist");
  query2.AddCompilationRequirement(false);
  query2.AddWhere("album", "", "!=");
  query2.AddWhere("albumartist", "", "=");

  if (!query.Exec()) {
    ReportErrors(query);
    return QStringList();
  }
  if (!query2.Exec()) {
    ReportErrors(query2);
    return QStringList();
  }

  QSet<QString> artists;
  while (query.Next()) {
    artists << query.Value(0).toString();
  }

  while (query2.Next()) {
    artists << query2.Value(0).toString();
  }

  return QStringList(artists.values());

}

CollectionBackend::AlbumList CollectionBackend::GetAllAlbums(const QueryOptions &opt) {
  return GetAlbums(QString(), false, opt);
}

CollectionBackend::AlbumList CollectionBackend::GetAlbumsByArtist(const QString &artist, const QueryOptions &opt) {
  return GetAlbums(artist, false, opt);
}

SongList CollectionBackend::GetArtistSongs(const QString &effective_albumartist, const QueryOptions &opt) {

  QSqlDatabase db(db_->Connect());
  QMutexLocker l(db_->Mutex());

  CollectionQuery query(db, songs_table_, opt);
  query.AddCompilationRequirement(false);
  query.AddWhere("effective_albumartist", effective_albumartist);

  SongList songs;
  if (!ExecCollectionQuery(&query, songs)) {
    ReportErrors(query);
  }

  return songs;

}

SongList CollectionBackend::GetAlbumSongs(const QString &effective_albumartist, const QString &album, const QueryOptions &opt) {

  QSqlDatabase db(db_->Connect());
  QMutexLocker l(db_->Mutex());

  CollectionQuery query(db, songs_table_, opt);
  query.AddCompilationRequirement(false);
  query.AddWhere("effective_albumartist", effective_albumartist);
  query.AddWhere("album", album);

  SongList songs;
  if (!ExecCollectionQuery(&query, songs)) {
    ReportErrors(query);
  }

  return songs;

}

SongList CollectionBackend::GetSongsByAlbum(const QString &album, const QueryOptions &opt) {

  QSqlDatabase db(db_->Connect());
  QMutexLocker l(db_->Mutex());

  CollectionQuery query(db, songs_table_, opt);
  query.AddCompilationRequirement(false);
  query.AddWhere("album", album);

  SongList songs;
  if (!ExecCollectionQuery(&query, songs)) {
    ReportErrors(query);
  }

  return songs;

}

bool CollectionBackend::ExecCollectionQuery(CollectionQuery *query, SongList &songs) {

  query->SetColumnSpec("%songs_table.ROWID, " + Song::kColumnSpec);

  if (!query->Exec()) return false;

  while (query->Next()) {
    Song song(source_);
    song.InitFromQuery(*query, true);
    songs << song;
  }
  return true;

}

bool CollectionBackend::ExecCollectionQuery(CollectionQuery *query, SongMap &songs) {

  query->SetColumnSpec("%songs_table.ROWID, " + Song::kColumnSpec);

  if (!query->Exec()) return false;

  while (query->Next()) {
    Song song(source_);
    song.InitFromQuery(*query, true);
    songs.insert(song.song_id(), song);
  }
  return true;

}

Song CollectionBackend::GetSongById(const int id) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());
  return GetSongById(id, db);

}

SongList CollectionBackend::GetSongsById(const QList<int> &ids) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  QStringList str_ids;
  str_ids.reserve(ids.count());
  for (const int id : ids) {
    str_ids << QString::number(id);
  }

  return GetSongsById(str_ids, db);

}

SongList CollectionBackend::GetSongsById(const QStringList &ids) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  return GetSongsById(ids, db);

}

SongList CollectionBackend::GetSongsByForeignId(const QStringList &ids, const QString &table, const QString &column) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  QString in = ids.join(",");

  SqlQuery q(db);
  q.prepare(QString("SELECT %2.ROWID, " + Song::kColumnSpec + ", %2.%3 FROM %2, %1 WHERE %2.%3 IN (%4) AND %1.ROWID = %2.ROWID AND unavailable = 0").arg(songs_table_, table, column, in));
  if (!q.Exec()) {
    db_->ReportErrors(q);
    return SongList();
  }

  QVector<Song> ret(ids.count());
  while (q.next()) {
    const QString foreign_id = q.value(static_cast<int>(Song::kColumns.count()) + 1).toString();
    const int index = ids.indexOf(foreign_id);
    if (index == -1) continue;

    ret[index].InitFromQuery(q, true);
  }
  return ret.toList();

}

Song CollectionBackend::GetSongById(const int id, QSqlDatabase &db) {

  SongList list = GetSongsById(QStringList() << QString::number(id), db);
  if (list.isEmpty()) return Song();
  return list.first();

}

SongList CollectionBackend::GetSongsById(const QStringList &ids, QSqlDatabase &db) {

  QString in = ids.join(",");

  SqlQuery q(db);
  q.prepare(QString("SELECT ROWID, " + Song::kColumnSpec + " FROM %1 WHERE ROWID IN (%2)").arg(songs_table_, in));
  if (!q.Exec()) {
    db_->ReportErrors(q);
    return SongList();
  }

  SongList ret;
  while (q.next()) {
    Song song(source_);
    song.InitFromQuery(q, true);
    ret << song;
  }
  return ret;

}

Song CollectionBackend::GetSongByUrl(const QUrl &url, const qint64 beginning) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  SqlQuery q(db);
  q.prepare(QString("SELECT ROWID, " + Song::kColumnSpec + " FROM %1 WHERE (url = :url1 OR url = :url2 OR url = :url3 OR url = :url4) AND beginning = :beginning AND unavailable = 0").arg(songs_table_));

  q.BindValue(":url1", url);
  q.BindValue(":url2", url.toString());
  q.BindValue(":url3", url.toString(QUrl::FullyEncoded));
  q.BindValue(":url4", url.toEncoded());
  q.BindValue(":beginning", beginning);

  if (!q.Exec()) {
    db_->ReportErrors(q);
    return Song();
  }

  if (!q.next()) {
    return Song();
  }

  Song song(source_);
  song.InitFromQuery(q, true);

  return song;

}

SongList CollectionBackend::GetSongsByUrl(const QUrl &url, const bool unavailable) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  SqlQuery q(db);
  q.prepare(QString("SELECT ROWID, " + Song::kColumnSpec + " FROM %1 WHERE (url = :url1 OR url = :url2 OR url = :url3 OR url = :url4) AND unavailable = :unavailable").arg(songs_table_));

  q.BindValue(":url1", url);
  q.BindValue(":url2", url.toString());
  q.BindValue(":url3", url.toString(QUrl::FullyEncoded));
  q.BindValue(":url4", url.toEncoded());
  q.BindValue(":unavailable", (unavailable ? 1 : 0));

  SongList songs;
  if (q.Exec()) {
    while (q.next()) {
      Song song(source_);
      song.InitFromQuery(q, true);
      songs << song;
    }
  }
  else {
    db_->ReportErrors(q);
  }

  return songs;

}


Song CollectionBackend::GetSongBySongId(const QString &song_id) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());
  return GetSongBySongId(song_id, db);

}

SongList CollectionBackend::GetSongsBySongId(const QStringList &song_ids) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  return GetSongsBySongId(song_ids, db);

}

Song CollectionBackend::GetSongBySongId(const QString &song_id, QSqlDatabase &db) {

  SongList list = GetSongsBySongId(QStringList() << song_id, db);
  if (list.isEmpty()) return Song();
  return list.first();

}

SongList CollectionBackend::GetSongsBySongId(const QStringList &song_ids, QSqlDatabase &db) {

  QStringList song_ids2;
  song_ids2.reserve(song_ids.count());
  for (const QString &song_id : song_ids) {
    song_ids2 << "'" + song_id + "'";
  }
  QString in = song_ids2.join(",");

  SqlQuery q(db);
  q.prepare(QString("SELECT ROWID, " + Song::kColumnSpec + " FROM %1 WHERE SONG_ID IN (%2)").arg(songs_table_, in));
  if (!q.Exec()) {
    db_->ReportErrors(q);
    return SongList();
  }

  SongList ret;
  while (q.next()) {
    Song song(source_);
    song.InitFromQuery(q, true);
    ret << song;
  }

  return ret;

}

SongList CollectionBackend::GetSongsByFingerprint(const QString &fingerprint) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  SqlQuery q(db);
  q.prepare(QString("SELECT ROWID, " + Song::kColumnSpec + " FROM %1 WHERE fingerprint = :fingerprint").arg(songs_table_));
  q.BindValue(":fingerprint", fingerprint);
  if (!q.Exec()) {
    db_->ReportErrors(q);
    return SongList();
  }

  SongList songs;
  while (q.next()) {
    Song song(source_);
    song.InitFromQuery(q, true);
    songs << song;
  }

  return songs;

}


CollectionBackend::AlbumList CollectionBackend::GetCompilationAlbums(const QueryOptions &opt) {
  return GetAlbums(QString(), true, opt);
}

SongList CollectionBackend::GetCompilationSongs(const QString &album, const QueryOptions &opt) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  CollectionQuery query(db, songs_table_, opt);
  query.SetColumnSpec("%songs_table.ROWID, " + Song::kColumnSpec);
  query.AddCompilationRequirement(true);
  query.AddWhere("album", album);

  if (!query.Exec()) {
    ReportErrors(query);
    return SongList();
  }

  SongList ret;
  while (query.Next()) {
    Song song(source_);
    song.InitFromQuery(query, true);
    ret << song;
  }
  return ret;

}

Song::Source CollectionBackend::Source() const {
  return source_;
}

void CollectionBackend::CompilationsNeedUpdating() {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  // Look for albums that have songs by more than one 'effective album artist' in the same directory

  SqlQuery q(db);
  q.prepare(QString("SELECT effective_albumartist, album, url, compilation_detected FROM %1 WHERE unavailable = 0 ORDER BY album").arg(songs_table_));
  if (!q.Exec()) {
    db_->ReportErrors(q);
    return;
  }

  QMap<QString, CompilationInfo> compilation_info;
  while (q.next()) {
    QString artist = q.value(0).toString();
    QString album = q.value(1).toString();
    QUrl url = QUrl::fromEncoded(q.value(2).toString().toUtf8());
    bool compilation_detected = q.value(3).toBool();

    // Ignore songs that don't have an album field set
    if (album.isEmpty()) continue;

    // Find the directory the song is in
    QString directory = url.toString(QUrl::PreferLocalFile|QUrl::RemoveFilename);

    CompilationInfo &info = compilation_info[directory + album];
    info.urls << url;
    if (!info.artists.contains(artist)) {
      info.artists << artist;
    }
    if (compilation_detected) info.has_compilation_detected++;
    else info.has_not_compilation_detected++;
  }

  // Now mark the songs that we think are in compilations
  SongList deleted_songs;
  SongList added_songs;

  ScopedTransaction transaction(&db);

  QMap<QString, CompilationInfo>::const_iterator it = compilation_info.constBegin();
  for (; it != compilation_info.constEnd(); ++it) {
    const CompilationInfo &info = it.value();

    // If there were more than one 'effective album artist' for this album directory, then it's a compilation.

    for (const QUrl &url : info.urls) {
      if (info.artists.count() > 1) {  // This directory+album is a compilation.
        if (info.has_not_compilation_detected > 0) {  // Run updates if any of the songs is not marked as compilations.
          UpdateCompilations(db, deleted_songs, added_songs, url, true);
        }
      }
      else {
        if (info.has_compilation_detected > 0) {
          UpdateCompilations(db, deleted_songs, added_songs, url, false);
        }
      }
    }
  }

  transaction.Commit();

  if (!deleted_songs.isEmpty()) {
    emit SongsDeleted(deleted_songs);
    emit SongsDiscovered(added_songs);
  }

}

bool CollectionBackend::UpdateCompilations(const QSqlDatabase &db, SongList &deleted_songs, SongList &added_songs, const QUrl &url, const bool compilation_detected) {

  {  // Get song, so we can tell the model its updated
    SqlQuery q(db);
    q.prepare(QString("SELECT ROWID, " + Song::kColumnSpec + " FROM %1 WHERE (url = :url1 OR url = :url2 OR url = :url3 OR url = :url4) AND unavailable = 0").arg(songs_table_));
    q.BindValue(":url1", url);
    q.BindValue(":url2", url.toString());
    q.BindValue(":url3", url.toString(QUrl::FullyEncoded));
    q.BindValue(":url4", url.toEncoded());
    if (q.Exec()) {
      while (q.next()) {
        Song song(source_);
        song.InitFromQuery(q, true);
        deleted_songs << song;
        song.set_compilation_detected(compilation_detected);
        added_songs << song;
      }
    }
    else {
      db_->ReportErrors(q);
      return false;
    }
  }

  // Update the song
  SqlQuery q(db);
  q.prepare(QString("UPDATE %1 SET compilation_detected = :compilation_detected, compilation_effective = ((compilation OR :compilation_detected OR compilation_on) AND NOT compilation_off) + 0 WHERE (url = :url1 OR url = :url2 OR url = :url3 OR url = :url4) AND unavailable = 0").arg(songs_table_));
  q.BindValue(":compilation_detected", int(compilation_detected));
  q.BindValue(":url1", url);
  q.BindValue(":url2", url.toString());
  q.BindValue(":url3", url.toString(QUrl::FullyEncoded));
  q.BindValue(":url4", url.toEncoded());
  if (!q.Exec()) {
    db_->ReportErrors(q);
    return false;
  }

  return true;

}

CollectionBackend::AlbumList CollectionBackend::GetAlbums(const QString &artist, const bool compilation_required, const QueryOptions &opt) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  CollectionQuery query(db, songs_table_, opt);
  query.SetColumnSpec("url, effective_albumartist, album, compilation_effective, art_automatic, art_manual, filetype, cue_path");
  query.SetOrderBy("effective_albumartist, album, url");

  if (compilation_required) {
    query.AddCompilationRequirement(true);
  }
  else if (!artist.isEmpty()) {
    query.AddCompilationRequirement(false);
    query.AddWhere("effective_albumartist", artist);
  }

  if (!query.Exec()) {
    ReportErrors(query);
    return AlbumList();
  }

  QMap<QString, Album> albums;
  while (query.Next()) {
    bool is_compilation = query.Value(3).toBool();

    Album info;
    QUrl url = QUrl::fromEncoded(query.Value(0).toByteArray());
    if (!is_compilation) {
      info.album_artist = query.Value(1).toString();
    }
    info.album = query.Value(2).toString();

    QString art_automatic = query.Value(4).toString();
    if (art_automatic.contains(QRegularExpression("..+:.*"))) {
      info.art_automatic = QUrl::fromEncoded(art_automatic.toUtf8());
    }
    else {
      info.art_automatic = QUrl::fromLocalFile(art_automatic);
    }

    QString art_manual = query.Value(5).toString();
    if (art_manual.contains(QRegularExpression("..+:.*"))) {
      info.art_manual = QUrl::fromEncoded(art_manual.toUtf8());
    }
    else {
      info.art_manual = QUrl::fromLocalFile(art_manual);
    }

    info.filetype = Song::FileType(query.Value(6).toInt());
    QString filetype = Song::TextForFiletype(info.filetype);
    info.cue_path = query.Value(7).toString();

    QString key;
    if (!info.album_artist.isEmpty()) {
      key.append(info.album_artist);
    }
    if (!info.album.isEmpty()) {
      if (!key.isEmpty()) key.append("-");
      key.append(info.album);
    }
    if (!filetype.isEmpty()) {
      key.append(filetype);
    }

    if (key.isEmpty()) continue;

    if (albums.contains(key)) {
      albums[key].urls.append(url);
    }
    else {
      info.urls << url;
      albums.insert(key, info);
    }

  }

  return albums.values();

}

CollectionBackend::Album CollectionBackend::GetAlbumArt(const QString &effective_albumartist, const QString &album) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  Album ret;
  ret.album = album;
  ret.album_artist = effective_albumartist;

  CollectionQuery query(db, songs_table_, QueryOptions());
  query.SetColumnSpec("art_automatic, art_manual, url");
  if (!effective_albumartist.isEmpty()) {
    query.AddWhere("effective_albumartist", effective_albumartist);
  }
  query.AddWhere("album", album);

  if (!query.Exec()) {
    ReportErrors(query);
    return ret;
  }

  if (query.Next()) {
    ret.art_automatic = QUrl::fromEncoded(query.Value(0).toByteArray());
    ret.art_manual = QUrl::fromEncoded(query.Value(1).toByteArray());
    ret.urls << QUrl::fromEncoded(query.Value(2).toByteArray());
  }

  return ret;

}

void CollectionBackend::UpdateManualAlbumArtAsync(const QString &effective_albumartist, const QString &album, const QUrl &cover_url, const bool clear_art_automatic) {

  QMetaObject::invokeMethod(this, "UpdateManualAlbumArt", Qt::QueuedConnection, Q_ARG(QString, effective_albumartist), Q_ARG(QString, album), Q_ARG(QUrl, cover_url), Q_ARG(bool, clear_art_automatic));

}

void CollectionBackend::UpdateManualAlbumArt(const QString &effective_albumartist, const QString &album, const QUrl &cover_url, const bool clear_art_automatic) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  // Get the songs before they're updated
  CollectionQuery query(db, songs_table_);
  query.SetColumnSpec("ROWID, " + Song::kColumnSpec);
  query.AddWhere("effective_albumartist", effective_albumartist);
  query.AddWhere("album", album);

  if (!query.Exec()) {
    ReportErrors(query);
    return;
  }

  SongList deleted_songs;
  while (query.Next()) {
    Song song(source_);
    song.InitFromQuery(query, true);
    deleted_songs << song;
  }

  // Update the songs
  QString sql(QString("UPDATE %1 SET art_manual = :cover").arg(songs_table_));
  if (clear_art_automatic) {
    sql += ", art_automatic = ''";
  }
  sql += " WHERE effective_albumartist = :effective_albumartist AND album = :album AND unavailable = 0";

  SqlQuery q(db);
  q.prepare(sql);
  q.BindValue(":cover", cover_url.isValid() ? cover_url.toString(QUrl::FullyEncoded) : "");
  q.BindValue(":effective_albumartist", effective_albumartist);
  q.BindValue(":album", album);

  if (!q.Exec()) {
    db_->ReportErrors(q);
    return;
  }

  // Now get the updated songs
  if (!query.Exec()) {
    ReportErrors(query);
    return;
  }

  SongList added_songs;
  while (query.Next()) {
    Song song(source_);
    song.InitFromQuery(query, true);
    added_songs << song;
  }

  if (!added_songs.isEmpty() || !deleted_songs.isEmpty()) {
    emit SongsDeleted(deleted_songs);
    emit SongsDiscovered(added_songs);
  }

}

void CollectionBackend::UpdateAutomaticAlbumArtAsync(const QString &effective_albumartist, const QString &album, const QUrl &cover_url) {

  QMetaObject::invokeMethod(this, "UpdateAutomaticAlbumArt", Qt::QueuedConnection, Q_ARG(QString, effective_albumartist), Q_ARG(QString, album), Q_ARG(QUrl, cover_url));

}

void CollectionBackend::UpdateAutomaticAlbumArt(const QString &effective_albumartist, const QString &album, const QUrl &cover_url) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  // Get the songs before they're updated
  CollectionQuery query(db, songs_table_);
  query.SetColumnSpec("ROWID, " + Song::kColumnSpec);
  query.AddWhere("effective_albumartist", effective_albumartist);
  query.AddWhere("album", album);

  if (!query.Exec()) {
    ReportErrors(query);
    return;
  }

  SongList deleted_songs;
  while (query.Next()) {
    Song song(source_);
    song.InitFromQuery(query, true);
    deleted_songs << song;
  }

  // Update the songs
  QString sql(QString("UPDATE %1 SET art_automatic = :cover WHERE effective_albumartist = :effective_albumartist AND album = :album AND unavailable = 0").arg(songs_table_));

  SqlQuery q(db);
  q.prepare(sql);
  q.BindValue(":cover", cover_url.isValid() ? cover_url.toString(QUrl::FullyEncoded) : "");
  q.BindValue(":effective_albumartist", effective_albumartist);
  q.BindValue(":album", album);

  if (!q.Exec()) {
    db_->ReportErrors(q);
    return;
  }

  // Now get the updated songs
  if (!query.Exec()) {
    ReportErrors(query);
    return;
  }

  SongList added_songs;
  while (query.Next()) {
    Song song(source_);
    song.InitFromQuery(query, true);
    added_songs << song;
  }

  if (!added_songs.isEmpty() || !deleted_songs.isEmpty()) {
    emit SongsDeleted(deleted_songs);
    emit SongsDiscovered(added_songs);
  }

}

void CollectionBackend::ForceCompilation(const QString &album, const QList<QString> &artists, const bool on) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());
  SongList deleted_songs, added_songs;

  for (const QString &artist : artists) {
    // Get the songs before they're updated
    CollectionQuery query(db, songs_table_);
    query.SetColumnSpec("ROWID, " + Song::kColumnSpec);
    query.AddWhere("album", album);
    if (!artist.isEmpty()) query.AddWhere("artist", artist);

    if (!query.Exec()) {
      ReportErrors(query);
      return;
    }

    while (query.Next()) {
      Song song(source_);
      song.InitFromQuery(query, true);
      deleted_songs << song;
    }

    // Update the songs
    QString sql(QString("UPDATE %1 SET compilation_on = :compilation_on, compilation_off = :compilation_off, compilation_effective = ((compilation OR compilation_detected OR :compilation_on) AND NOT :compilation_off) + 0 WHERE album = :album AND unavailable = 0").arg(songs_table_));
    if (!artist.isEmpty()) sql += " AND artist = :artist";

    SqlQuery q(db);
    q.prepare(sql);
    q.BindValue(":compilation_on", on ? 1 : 0);
    q.BindValue(":compilation_off", on ? 0 : 1);
    q.BindValue(":album", album);
    if (!artist.isEmpty()) q.BindValue(":artist", artist);

    if (!q.Exec()) {
      db_->ReportErrors(q);
      return;
    }

    // Now get the updated songs
    if (!query.Exec()) {
      ReportErrors(query);
      return;
    }

    while (query.Next()) {
      Song song(source_);
      song.InitFromQuery(query, true);
      added_songs << song;
    }
  }

  if (!added_songs.isEmpty() || !deleted_songs.isEmpty()) {
    emit SongsDeleted(deleted_songs);
    emit SongsDiscovered(added_songs);
  }

}

void CollectionBackend::IncrementPlayCount(const int id) {

  if (id == -1) return;

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  SqlQuery q(db);
  q.prepare(QString("UPDATE %1 SET playcount = playcount + 1, lastplayed = :now WHERE ROWID = :id").arg(songs_table_));
  q.BindValue(":now", QDateTime::currentDateTime().toSecsSinceEpoch());
  q.BindValue(":id", id);
  if (!q.Exec()) {
    db_->ReportErrors(q);
    return;
  }

  Song new_song = GetSongById(id, db);
  emit SongsStatisticsChanged(SongList() << new_song);

}

void CollectionBackend::IncrementSkipCount(const int id, const float progress) {

  Q_UNUSED(progress);

  if (id == -1) return;

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  SqlQuery q(db);
  q.prepare(QString("UPDATE %1 SET skipcount = skipcount + 1 WHERE ROWID = :id").arg(songs_table_));
  q.BindValue(":id", id);
  if (!q.Exec()) {
    db_->ReportErrors(q);
    return;
  }

  Song new_song = GetSongById(id, db);
  emit SongsStatisticsChanged(SongList() << new_song);

}

void CollectionBackend::ResetStatistics(const int id) {

  if (id == -1) return;

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  SqlQuery q(db);
  q.prepare(QString("UPDATE %1 SET playcount = 0, skipcount = 0, lastplayed = -1 WHERE ROWID = :id").arg(songs_table_));
  q.BindValue(":id", id);
  if (!q.Exec()) {
    db_->ReportErrors(q);
    return;
  }

  Song new_song = GetSongById(id, db);
  emit SongsStatisticsChanged(SongList() << new_song);

}

void CollectionBackend::DeleteAll() {

  {
    QMutexLocker l(db_->Mutex());
    QSqlDatabase db(db_->Connect());
    ScopedTransaction t(&db);

    {
      SqlQuery q(db);
      q.prepare("DELETE FROM " + songs_table_);
      if (!q.Exec()) {
        db_->ReportErrors(q);
        return;
      }
    }

    t.Commit();
  }

  emit DatabaseReset();

}

SongList CollectionBackend::SmartPlaylistsFindSongs(const SmartPlaylistSearch &search) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  // Build the query
  QString sql = search.ToSql(songs_table());

  // Run the query
  SongList ret;
  SqlQuery query(db);
  query.prepare(sql);
  if (!query.Exec()) {
    db_->ReportErrors(query);
    return ret;
  }

  // Read the results
  while (query.next()) {
    Song song;
    song.InitFromQuery(query, true);
    ret << song;
  }
  return ret;

}

SongList CollectionBackend::SmartPlaylistsGetAllSongs() {

  // Get all the songs!
  return SmartPlaylistsFindSongs(SmartPlaylistSearch(SmartPlaylistSearch::Type_All, SmartPlaylistSearch::TermList(), SmartPlaylistSearch::Sort_FieldAsc, SmartPlaylistSearchTerm::Field_Artist, -1));

}

SongList CollectionBackend::GetSongsBy(const QString &artist, const QString &album, const QString &title) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  SongList songs;
  SqlQuery q(db);
  if (album.isEmpty()) {
    q.prepare(QString("SELECT ROWID, " + Song::kColumnSpec + " FROM %1 WHERE artist = :artist COLLATE NOCASE AND title = :title COLLATE NOCASE").arg(songs_table_));
  }
  else {
    q.prepare(QString("SELECT ROWID, " + Song::kColumnSpec + " FROM %1 WHERE artist = :artist COLLATE NOCASE AND album = :album COLLATE NOCASE AND title = :title COLLATE NOCASE").arg(songs_table_));
  }
  q.BindValue(":artist", artist);
  if (!album.isEmpty()) q.BindValue(":album", album);
  q.BindValue(":title", title);
  if (!q.Exec()) {
    db_->ReportErrors(q);
    return SongList();
  }
  while (q.next()) {
    Song song(source_);
    song.InitFromQuery(q, true);
    songs << song;
  }

  return songs;

}

void CollectionBackend::UpdateLastPlayed(const QString &artist, const QString &album, const QString &title, const qint64 lastplayed) {

  SongList songs = GetSongsBy(artist, album, title);
  if (songs.isEmpty()) {
    qLog(Debug) << "Could not find a matching song in the database for" << artist << album << title;
    return;
  }

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  for (const Song &song : songs) {
    if (song.lastplayed() >= lastplayed) {
      continue;
    }
    SqlQuery q(db);
    q.prepare(QString("UPDATE %1 SET lastplayed = :lastplayed WHERE ROWID = :id").arg(songs_table_));
    q.BindValue(":lastplayed", lastplayed);
    q.BindValue(":id", song.id());
    if (!q.Exec()) {
      db_->ReportErrors(q);
      continue;
    }
  }

  emit SongsStatisticsChanged(SongList() << songs);

}

void CollectionBackend::UpdatePlayCount(const QString &artist, const QString &title, const int playcount) {

  SongList songs = GetSongsBy(artist, QString(), title);
  if (songs.isEmpty()) {
    qLog(Debug) << "Could not find a matching song in the database for" << artist << title;
    return;
  }

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  for (const Song &song : songs) {
    SqlQuery q(db);
    q.prepare(QString("UPDATE %1 SET playcount = :playcount WHERE ROWID = :id").arg(songs_table_));
    q.BindValue(":playcount", playcount);
    q.BindValue(":id", song.id());
    if (!q.Exec()) {
      db_->ReportErrors(q);
      return;
    }
  }

  emit SongsStatisticsChanged(SongList() << songs);

}

void CollectionBackend::UpdateSongRating(const int id, const double rating) {

  if (id == -1) return;

  QList<int> id_list;
  id_list << id;
  UpdateSongsRating(id_list, rating);

}

void CollectionBackend::UpdateSongsRating(const QList<int> &id_list, const double rating) {

  if (id_list.isEmpty()) return;

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  QStringList id_str_list;
  id_str_list.reserve(id_list.count());
  for (int i : id_list) {
    id_str_list << QString::number(i);
  }
  QString ids = id_str_list.join(",");
  SqlQuery q(db);
  q.prepare(QString("UPDATE %1 SET rating = :rating WHERE ROWID IN (%2)").arg(songs_table_, ids));
  q.BindValue(":rating", rating);
  if (!q.Exec()) {
    db_->ReportErrors(q);
    return;
  }

  SongList new_song_list = GetSongsById(id_str_list, db);

  emit SongsRatingChanged(new_song_list);

}

void CollectionBackend::UpdateSongRatingAsync(const int id, const double rating) {
  QMetaObject::invokeMethod(this, "UpdateSongRating", Qt::QueuedConnection, Q_ARG(int, id), Q_ARG(double, rating));
}

void CollectionBackend::UpdateSongsRatingAsync(const QList<int> &ids, const double rating) {
  QMetaObject::invokeMethod(this, "UpdateSongsRating", Qt::QueuedConnection, Q_ARG(QList<int>, ids), Q_ARG(double, rating));
}

void CollectionBackend::UpdateLastSeen(const int directory_id, const int expire_unavailable_songs_days) {

  {
    QMutexLocker l(db_->Mutex());
    QSqlDatabase db(db_->Connect());

    SqlQuery q(db);
    q.prepare(QString("UPDATE %1 SET lastseen = :lastseen WHERE directory_id = :directory_id AND unavailable = 0").arg(songs_table_));
    q.BindValue(":lastseen", QDateTime::currentDateTime().toSecsSinceEpoch());
    q.BindValue(":directory_id", directory_id);
    if (!q.Exec()) {
      db_->ReportErrors(q);
      return;
    }
  }

  if (expire_unavailable_songs_days > 0) ExpireSongs(directory_id, expire_unavailable_songs_days);

}

void CollectionBackend::ExpireSongs(const int directory_id, const int expire_unavailable_songs_days) {

  SongList songs;
  {
    QMutexLocker l(db_->Mutex());
    QSqlDatabase db(db_->Connect());
    SqlQuery q(db);
    q.prepare(QString("SELECT ROWID, " + Song::kColumnSpec + " FROM %1 WHERE directory_id = :directory_id AND unavailable = 1 AND lastseen > 0 AND lastseen < :time").arg(songs_table_));
    q.BindValue(":directory_id", directory_id);
    q.BindValue(":time", QDateTime::currentDateTime().toSecsSinceEpoch() - (expire_unavailable_songs_days * 86400));
    if (!q.Exec()) {
      db_->ReportErrors(q);
      return;
    }
    while (q.next()) {
      Song song(source_);
      song.InitFromQuery(q, true);
      songs << song;
    }
  }

  if (!songs.isEmpty()) DeleteSongs(songs);

}

