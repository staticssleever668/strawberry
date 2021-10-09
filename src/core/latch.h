/* This file is part of Clementine.
   Copyright 2016, John Maguire <john.maguire@gmail.com>

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#ifndef LATCH_H
#define LATCH_H

#include <QObject>
#include <QMutex>

class CountdownLatch : public QObject {
  Q_OBJECT

 public:
  explicit CountdownLatch();
  void Wait();
  void CountDown();

 signals:
  void Done();

 private:
  QMutex mutex_;
  int count_;
};

#endif  // LATCH_H
