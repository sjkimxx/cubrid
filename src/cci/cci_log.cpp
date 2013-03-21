/*
 * Copyright (C) 2008 Search Solution Corporation. All rights reserved by Search Solution.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * - Neither the name of the <ORGANIZATION> nor the names of its contributors
 *   may be used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 */

/*
 * cci_log.cpp -
 */

#include <errno.h>
#include <stdarg.h>
#if defined(WINDOWS)
#include <winsock2.h>
#include <windows.h>
#include <time.h>
#include <direct.h>
#include <io.h>
#else
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <list>
#include <sstream>

#include "cci_common.h"
#include "cci_mutex.h"
#include "cci_log.h"

static const int LOG_BUFFER_SIZE = 1024 * 20;
static const int LOG_ER_OPEN = -1;
static const long int LOG_FLUSH_SIZE = 1024 * 1024; /* byte */
static const long int LOG_FLUSH_USEC = 1 * 1000000; /* usec */
static const long int LOG_CHECK_FILE_INTERVAL_USEC = 10 * 1000000; /* usec */
static const char *cci_log_level_string[] =
{ "OFF", "ERROR", "WARN", "INFO", "DEBUG" };

struct _LoggerContext;
class _Logger;

class _LogAppender
{
public:
  _LogAppender(const _LoggerContext &context);
  virtual ~_LogAppender() {}

  virtual void open() = 0;
  virtual void close() = 0;
  virtual void write(const char *msg) = 0;
  virtual void flush() = 0;

protected:
  const _LoggerContext &context;
};

class _LogAppenderBase : public _LogAppender
{
public:
  _LogAppenderBase(const _LoggerContext &context);
  virtual ~_LogAppenderBase();

  virtual void open();
  virtual void close();
  virtual void write(const char *msg);
  virtual void flush();

protected:
  virtual void role() = 0;
  virtual bool isRole() = 0;

  std::string rename(const char *newPath, const char *postfix);
  int getLogSizeKBytes();

private:
  void makeLogDir();
  void checkFileIsOpen();

private:
  std::ofstream out;
  long int nextCheckTime;
};

class _MaxSizeLogAppender : public _LogAppenderBase
{
public:
  _MaxSizeLogAppender(const _LoggerContext &context, int maxFileSizeKBytes,
      int maxBackupCount);
  virtual ~_MaxSizeLogAppender() {}

protected:
  virtual void role();
  virtual bool isRole();

private:
  std::string get_curr_date_time();

private:
  int maxFileSizeKBytes;
  int maxBackupCount;
  int currBackupCount;
  std::list<std::string> backupList;
};

class _DailyLogAppender : public _LogAppenderBase
{
public:
  _DailyLogAppender(const _LoggerContext &context);
  virtual ~_DailyLogAppender() {}

protected:
  virtual void role();
  virtual bool isRole();

private:
  std::string getCurrDate();
};

struct _LoggerContext
{
  std::string path;
  struct timeval now;
};

class _Logger
{
public:
  _Logger(const char *path);
  virtual ~_Logger();

  void open();
  void setLogLevel(CCI_LOG_LEVEL level);
  void setUseDefaultPrefix(bool useDefaultPrefix);
  void setUseDefaultNewLine(bool useDefaultNewLine);
  void setForceFlush(bool isForceFlush);
  void log(CCI_LOG_LEVEL level, const char *msg);
  void changeMaxFileSizeAppender(int maxFileSizeKBytes, int maxBackupCount);

public:
  const char *getPath() const;
  bool isWritable(CCI_LOG_LEVEL level) const;

private:
  void write(const char *msg);
  void logPrefix(CCI_LOG_LEVEL level);

private:
  cci::_Mutex critical;
  _LoggerContext context;
  _LogAppender *logAppender;
  CCI_LOG_LEVEL level;
  bool useDefaultPrefix;
  bool useDefaultNewLine;
  bool isForceFlush;
  int unflushedBytes;
  unsigned long nextFlushTime;
};

_LogAppender::_LogAppender(const _LoggerContext &context) :
  context(context)
{
}

_LogAppenderBase::_LogAppenderBase(const _LoggerContext &context) :
  _LogAppender(context), nextCheckTime(0)
{
  open();
}

_LogAppenderBase::~_LogAppenderBase()
{
  close();
}

void _LogAppenderBase::open()
{
  if (out.is_open())
    {
      return;
    }

  out.open(context.path.c_str(), std::fstream::out | std::fstream::app);
  if (out.fail())
    {
      makeLogDir();

      out.open(context.path.c_str(), std::fstream::out);
      if (out.fail())
        {
          throw LOG_ER_OPEN;
        }
    }
}

void _LogAppenderBase::close()
{
  if (!out.is_open())
    {
      return;
    }

  out.close();
}

void _LogAppenderBase::write(const char *msg)
{
  if (!out.is_open())
    {
      return;
    }

  checkFileIsOpen();

  if (this->isRole())
    {
      this->role();
    }

  out << msg;
}

void _LogAppenderBase::flush()
{
  if (!out.is_open())
    {
      return;
    }

  out.flush();
}

std::string _LogAppenderBase::rename(const char *newPath, const char *postfix)
{
  std::stringstream newPathStream;
  newPathStream << newPath;

  close();

  if (access(newPath, F_OK) == 0 && postfix != NULL)
    {
      newPathStream << postfix;
    }

  int e = std::rename(context.path.c_str(), newPathStream.str().c_str());
  if (e != 0)
    {
      perror("rename");
    }

  try
    {
      open();
    }
  catch (...)
    {
    }

  return newPathStream.str();
}

int _LogAppenderBase::getLogSizeKBytes()
{
  if (!out.is_open())
    {
      return 0;
    }
  else
    {
      return out.tellp() / 1024;
    }
}

void _LogAppenderBase::makeLogDir()
{
  const char *sep = "/\\";

  char dir[FILENAME_MAX];
  char *p = dir;
  const char *q = context.path.c_str();

  while (*q)
    {
      *p++ = *q;
      *p = '\0';
      if (*q == sep[0] || *q == sep[1])
        {
          mkdir(dir, 0755);
        }
      q++;
    }
}

void _LogAppenderBase::checkFileIsOpen()
{
  long int currentTime = context.now.tv_sec * 1000000 + context.now.tv_usec;

  if (nextCheckTime == 0 || currentTime >= nextCheckTime)
    {
      if (access(context.path.c_str(), F_OK) != 0)
        {
          if (out.is_open())
            {
              out.close();
            }
          open();
        }

      nextCheckTime = currentTime + LOG_CHECK_FILE_INTERVAL_USEC;
    }
}

_MaxSizeLogAppender::_MaxSizeLogAppender(const _LoggerContext &context,
    int maxFileSizeKBytes, int maxBackupCount) :
  _LogAppenderBase(context), maxFileSizeKBytes(maxFileSizeKBytes),
  maxBackupCount(maxBackupCount), currBackupCount(0)
{
}

void _MaxSizeLogAppender::role()
{
  std::stringstream newPath_stream;
  newPath_stream << context.path << "." << get_curr_date_time();

  std::stringstream postfix_stream;
  postfix_stream << "(" << currBackupCount++ << ")";

  std::string newPath = rename(newPath_stream.str().c_str(),
      postfix_stream.str().c_str());
  backupList.push_back(newPath);

  if (backupList.size() > (size_t) maxBackupCount)
    {
      std::string remove_path = backupList.front();
      backupList.pop_front();

      int e = remove(remove_path.c_str());
      if (e != 0)
        {
          perror("remove");
        }
    }
}

bool _MaxSizeLogAppender::isRole()
{
  return getLogSizeKBytes() > maxFileSizeKBytes;
}

std::string _MaxSizeLogAppender::get_curr_date_time()
{
  struct tm cal;
  char buf[16];
  time_t now = time(NULL);

  localtime_r((const time_t *) &context.now.tv_sec, &cal);
  cal.tm_year += 1900;
  cal.tm_mon += 1;
  snprintf(buf, 16, "%d%02d%02d%02d%02d%02d", cal.tm_year, cal.tm_mon,
      cal.tm_mday, cal.tm_hour, cal.tm_min, cal.tm_sec);

  return buf;
}

_DailyLogAppender::_DailyLogAppender(const _LoggerContext &context) :
  _LogAppenderBase(context)
{
}

void _DailyLogAppender::role()
{
  std::stringstream newPathStream;
  newPathStream << context.path << "." << getCurrDate();

  rename(newPathStream.str().c_str(), NULL);
}

bool _DailyLogAppender::isRole()
{
  int roleDay = context.now.tv_sec / 86400;
  int nowDay = time(NULL) / 86400;
  return roleDay < nowDay;
}

std::string _DailyLogAppender::getCurrDate()
{
  struct tm cal;
  char buf[16];
  time_t now = time(NULL);

  localtime_r((const time_t *) &context.now.tv_sec, &cal);
  cal.tm_year += 1900;
  cal.tm_mon += 1;
  snprintf(buf, 16, "%d-%02d-%02d", cal.tm_year, cal.tm_mon, cal.tm_mday);

  return buf;
}

_Logger::_Logger(const char *path) :
  logAppender(NULL), level(CCI_LOG_LEVEL_INFO), useDefaultPrefix(true),
  useDefaultNewLine(true), isForceFlush(true), unflushedBytes(0),
  nextFlushTime(0)
{
  context.path = path;
  gettimeofday(&context.now, NULL);
  nextFlushTime = context.now.tv_usec + LOG_FLUSH_USEC;

  logAppender = new _DailyLogAppender(context);

  open();
}

_Logger::~_Logger()
{
  if (logAppender != NULL)
    {
      delete logAppender;
    }
}

void _Logger::open()
{
  cci::_MutexAutolock lock(&critical);

  logAppender->open();
}

void _Logger::setLogLevel(CCI_LOG_LEVEL level)
{
  this->level = level;
}

void _Logger::setUseDefaultPrefix(bool useDefaultPrefix)
{
  this->useDefaultPrefix = useDefaultPrefix;
}

void _Logger::setUseDefaultNewLine(bool useDefaultNewLine)
{
  this->useDefaultNewLine = useDefaultNewLine;
}

void _Logger::setForceFlush(bool isForceFlush)
{
  this->isForceFlush = isForceFlush;
}

void _Logger::log(CCI_LOG_LEVEL level, const char *msg)
{
  cci::_MutexAutolock lock(&critical);

  gettimeofday(&context.now, NULL);

  if (useDefaultPrefix)
    {
      logPrefix(level);
    }

  write(msg);

  if (useDefaultNewLine)
    {
      write("\n");
    }
}

void _Logger::changeMaxFileSizeAppender(int maxFileSizeKBytes, int maxBackupCount)
{
  cci::_MutexAutolock lock(&critical);

  if (this->logAppender != NULL)
    {
      delete this->logAppender;
    }

  this->logAppender = new _MaxSizeLogAppender(context, maxFileSizeKBytes,
      maxBackupCount);
}

const char *_Logger::getPath() const
{
  return context.path.c_str();
}

bool _Logger::isWritable(CCI_LOG_LEVEL level) const
{
  return this->level >= level;
}

void _Logger::write(const char *msg)
{
  logAppender->write(msg);

  unflushedBytes += strlen(msg);

  if (isForceFlush || unflushedBytes >= LOG_FLUSH_SIZE
      || nextFlushTime >= (unsigned long) context.now.tv_usec)
    {
      logAppender->flush();
      unflushedBytes = 0;
      nextFlushTime = context.now.tv_usec + LOG_FLUSH_USEC;
    }
}

void _Logger::logPrefix(CCI_LOG_LEVEL level)
{
  struct tm cal;
  time_t t;

  t = context.now.tv_sec;

  localtime_r((const time_t *) &t, &cal);
  cal.tm_year += 1900;
  cal.tm_mon += 1;

  char buf[128];
  unsigned long tid = gettid();
  snprintf(buf, 128, "%d-%02d-%02d %02d:%02d:%02d.%03d [TID:%lu] [%5s]",
      cal.tm_year, cal.tm_mon, cal.tm_mday, cal.tm_hour, cal.tm_min,
      cal.tm_sec, (int)(context.now.tv_usec / 1000), tid,
      cci_log_level_string[level]);

  write(buf);
}

typedef std::map<std::string, _Logger *> _logger_map;

class _LoggerManager
{
public:
  _LoggerManager() {}
  virtual ~_LoggerManager() {}

  _Logger *getLogger(const char *path);
  void removeLogger(const char *path);
  void clearLogger();

private:
  cci::_Mutex critical;
  _logger_map map;
};

_Logger *_LoggerManager::getLogger(const char *path)
{
  cci::_MutexAutolock lock(&critical);

  _logger_map::iterator it = map.find(path);
  if (it == map.end())
    {
      try
        {
          _Logger *logger = new _Logger(path);
          map[path] = logger;
          return logger;
        }
      catch (...)
        {
          return NULL;
        }
    }

  return it->second;
}

void _LoggerManager::removeLogger(const char *path)
{
  cci::_MutexAutolock lock(&critical);

  _logger_map::iterator it = map.find(path);
  if (it != map.end())
    {
      delete it->second;
      map.erase(it);
    }
}

void _LoggerManager::clearLogger()
{
  cci::_MutexAutolock lock(&critical);

  _logger_map::iterator it = map.begin();
  for (; it != map.end(); it++)
    {
      delete it->second;
    }

  map.clear();
}

static _LoggerManager loggerManager;

Logger cci_log_add(const char *path)
{
  return loggerManager.getLogger(path);
}

Logger cci_log_get(const char *path)
{
  return loggerManager.getLogger(path);
}

void cci_log_finalize(void)
{
  loggerManager.clearLogger();
}

void cci_log_writef(CCI_LOG_LEVEL level, Logger logger, const char *format, ...)
{
  _Logger *l = (_Logger *) logger;

  if (l == NULL)
    {
      return;
    }

  char buf[LOG_BUFFER_SIZE];
  va_list vl;

  va_start(vl, format);
  vsnprintf(buf, LOG_BUFFER_SIZE, format, vl);
  va_end(vl);

  l->log(level, buf);
}

void cci_log_write(CCI_LOG_LEVEL level, Logger logger, const char *log)
{
  _Logger *l = (_Logger *) logger;

  if (l == NULL)
    {
      return;
    }

  l->log(level, (char *) log);
}

void cci_log_remove(const char *path)
{
  loggerManager.removeLogger(path);
}

void cci_log_set_level(Logger logger, CCI_LOG_LEVEL level)
{
  _Logger *l = (_Logger *) logger;

  if (l == NULL)
    {
      return;
    }

  l->setLogLevel(level);
}

bool cci_log_is_writable(Logger logger, CCI_LOG_LEVEL level)
{
  _Logger *l = (_Logger *) logger;

  if (l == NULL)
    {
      return false;
    }

  return l->isWritable(level);
}

void cci_log_set_force_flush(Logger logger, bool force_flush)
{
  _Logger *l = (_Logger *) logger;

  if (l == NULL)
    {
      return;
    }

  l->setForceFlush(force_flush);
}

void cci_log_use_default_newline(Logger logger, bool use_default_newline)
{
  _Logger *l = (_Logger *) logger;

  if (l == NULL)
    {
      return;
    }

  l->setUseDefaultNewLine(use_default_newline);
}

void cci_log_use_default_prefix(Logger logger, bool use_default_prefix)
{
  _Logger *l = (_Logger *) logger;

  if (l == NULL)
    {
      return;
    }

  l->setUseDefaultPrefix(use_default_prefix);
}

void cci_log_change_max_file_size_appender(Logger logger,
    int maxFileSizeKBytes, int maxBackupCount)
{
  _Logger *l = (_Logger *) logger;

  if (l == NULL)
    {
      return;
    }

  try
    {
      l->changeMaxFileSizeAppender(maxFileSizeKBytes, maxBackupCount);
    }
  catch (...)
    {
    }
}
