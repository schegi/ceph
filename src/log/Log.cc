// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "Log.h"

#include <errno.h>
#include <syslog.h>

#include "common/errno.h"
#include "common/safe_io.h"
#include "common/Clock.h"
#include "common/Graylog.h"
#include "common/valgrind.h"

#include "include/assert.h"
#include "include/compat.h"
#include "include/on_exit.h"

#include "Entry.h"
#include "LogClock.h"
#include "SubsystemMap.h"

#define DEFAULT_MAX_NEW    100
#define DEFAULT_MAX_RECENT 10000

#define PREALLOC 1000000
#define MAX_LOG_BUF 65536

namespace ceph {
namespace logging {

static OnExitManager exit_callbacks;

static void log_on_exit(void *p)
{
  Log *l = *(Log **)p;
  if (l)
    l->flush();
  delete (Log **)p;// Delete allocated pointer (not Log object, the pointer only!)
}

Log::Log(const SubsystemMap *s)
  : m_indirect_this(NULL),
    m_subs(s),
    m_queue_mutex_holder(0),
    m_flush_mutex_holder(0),
    m_new(), m_recent(),
    m_fd(-1),
    m_uid(0),
    m_gid(0),
    m_fd_last_error(0),
    m_syslog_log(-2), m_syslog_crash(-2),
    m_stderr_log(1), m_stderr_crash(-1),
    m_graylog_log(-3), m_graylog_crash(-3),
    m_log_buf(nullptr), m_log_buf_pos(0),
    m_stop(false),
    m_max_new(DEFAULT_MAX_NEW),
    m_max_recent(DEFAULT_MAX_RECENT),
    m_inject_segv(false)
{
  int ret;

  ret = pthread_mutex_init(&m_flush_mutex, NULL);
  ceph_assert(ret == 0);

  ret = pthread_mutex_init(&m_queue_mutex, NULL);
  ceph_assert(ret == 0);

  ret = pthread_cond_init(&m_cond_loggers, NULL);
  ceph_assert(ret == 0);

  ret = pthread_cond_init(&m_cond_flusher, NULL);
  ceph_assert(ret == 0);

  m_log_buf = (char*)malloc(MAX_LOG_BUF);

  // kludge for prealloc testing
  if (false)
    for (int i=0; i < PREALLOC; i++)
      m_recent.enqueue(new Entry);
}

Log::~Log()
{
  if (m_indirect_this) {
    *m_indirect_this = NULL;
  }

  ceph_assert(!is_started());
  if (m_fd >= 0)
    VOID_TEMP_FAILURE_RETRY(::close(m_fd));
  free(m_log_buf);

  pthread_mutex_destroy(&m_queue_mutex);
  pthread_mutex_destroy(&m_flush_mutex);
  pthread_cond_destroy(&m_cond_loggers);
  pthread_cond_destroy(&m_cond_flusher);
}


///
void Log::set_coarse_timestamps(bool coarse) {
  if (coarse)
    clock.coarsen();
  else
    clock.refine();
}

void Log::set_flush_on_exit()
{
  // Make sure we flush on shutdown.  We do this by deliberately
  // leaking an indirect pointer to ourselves (on_exit() can't
  // unregister a callback).  This is not racy only becuase we
  // assume that exit() won't race with ~Log().
  if (m_indirect_this == NULL) {
    m_indirect_this = new (Log*)(this);
    exit_callbacks.add_callback(log_on_exit, m_indirect_this);
  }
}

void Log::set_max_new(int n)
{
  m_max_new = n;
}

void Log::set_max_recent(int n)
{
  pthread_mutex_lock(&m_flush_mutex);
  m_flush_mutex_holder = pthread_self();
  m_max_recent = n;
  m_flush_mutex_holder = 0;
  pthread_mutex_unlock(&m_flush_mutex);
}

void Log::set_log_file(string fn)
{
  m_log_file = fn;
}

void Log::set_log_stderr_prefix(const std::string& p)
{
  m_log_stderr_prefix = p;
}

void Log::reopen_log_file()
{
  pthread_mutex_lock(&m_flush_mutex);
  m_flush_mutex_holder = pthread_self();
  if (m_fd >= 0)
    VOID_TEMP_FAILURE_RETRY(::close(m_fd));
  if (m_log_file.length()) {
    m_fd = ::open(m_log_file.c_str(), O_CREAT|O_WRONLY|O_APPEND, 0644);
    if (m_fd >= 0 && (m_uid || m_gid)) {
      int r = ::fchown(m_fd, m_uid, m_gid);
      if (r < 0) {
	r = -errno;
	cerr << "failed to chown " << m_log_file << ": " << cpp_strerror(r)
	     << std::endl;
      }
    }
  } else {
    m_fd = -1;
  }
  m_flush_mutex_holder = 0;
  pthread_mutex_unlock(&m_flush_mutex);
}

void Log::chown_log_file(uid_t uid, gid_t gid)
{
  pthread_mutex_lock(&m_flush_mutex);
  if (m_fd >= 0) {
    int r = ::fchown(m_fd, uid, gid);
    if (r < 0) {
      r = -errno;
      cerr << "failed to chown " << m_log_file << ": " << cpp_strerror(r)
	   << std::endl;
    }
  }
  pthread_mutex_unlock(&m_flush_mutex);
}

void Log::set_syslog_level(int log, int crash)
{
  pthread_mutex_lock(&m_flush_mutex);
  m_syslog_log = log;
  m_syslog_crash = crash;
  pthread_mutex_unlock(&m_flush_mutex);
}

void Log::set_stderr_level(int log, int crash)
{
  pthread_mutex_lock(&m_flush_mutex);
  m_stderr_log = log;
  m_stderr_crash = crash;
  pthread_mutex_unlock(&m_flush_mutex);
}

void Log::set_graylog_level(int log, int crash)
{
  pthread_mutex_lock(&m_flush_mutex);
  m_graylog_log = log;
  m_graylog_crash = crash;
  pthread_mutex_unlock(&m_flush_mutex);
}

void Log::start_graylog()
{
  pthread_mutex_lock(&m_flush_mutex);
  if (! m_graylog.get())
    m_graylog = std::make_shared<Graylog>(m_subs, "dlog");
  pthread_mutex_unlock(&m_flush_mutex);
}


void Log::stop_graylog()
{
  pthread_mutex_lock(&m_flush_mutex);
  m_graylog.reset();
  pthread_mutex_unlock(&m_flush_mutex);
}

void Log::submit_entry(Entry *e)
{
  e->finish();

  pthread_mutex_lock(&m_queue_mutex);
  m_queue_mutex_holder = pthread_self();

  if (m_inject_segv)
    *(volatile int *)(0) = 0xdead;

  // wait for flush to catch up
  while (m_new.m_len > m_max_new)
    pthread_cond_wait(&m_cond_loggers, &m_queue_mutex);

  m_new.enqueue(e);
  pthread_cond_signal(&m_cond_flusher);
  m_queue_mutex_holder = 0;
  pthread_mutex_unlock(&m_queue_mutex);
}


Entry *Log::create_entry(int level, int subsys, const char* msg)
{
  if (true) {
    return new Entry(clock.now(),
		     pthread_self(),
		     level, subsys, msg);
  } else {
    // kludge for perf testing
    Entry *e = m_recent.dequeue();
    e->m_stamp = clock.now();
    e->m_thread = pthread_self();
    e->m_prio = level;
    e->m_subsys = subsys;
    return e;
  }
}

Entry *Log::create_entry(int level, int subsys, size_t* expected_size)
{
  if (true) {
    ANNOTATE_BENIGN_RACE_SIZED(expected_size, sizeof(*expected_size),
                               "Log hint");
    size_t size = __atomic_load_n(expected_size, __ATOMIC_RELAXED);
    void *ptr = ::operator new(sizeof(Entry) + size);
    return new(ptr) Entry(clock.now(),
       pthread_self(), level, subsys,
       reinterpret_cast<char*>(ptr) + sizeof(Entry), size, expected_size);
  } else {
    // kludge for perf testing
    Entry *e = m_recent.dequeue();
    e->m_stamp = clock.now();
    e->m_thread = pthread_self();
    e->m_prio = level;
    e->m_subsys = subsys;
    return e;
  }
}

void Log::flush()
{
  pthread_mutex_lock(&m_flush_mutex);
  m_flush_mutex_holder = pthread_self();
  pthread_mutex_lock(&m_queue_mutex);
  m_queue_mutex_holder = pthread_self();
  EntryQueue t;
  t.swap(m_new);
  pthread_cond_broadcast(&m_cond_loggers);
  m_queue_mutex_holder = 0;
  pthread_mutex_unlock(&m_queue_mutex);
  _flush(&t, &m_recent, false);

  // trim
  while (m_recent.m_len > m_max_recent) {
    m_recent.dequeue()->destroy();
  }

  m_flush_mutex_holder = 0;
  pthread_mutex_unlock(&m_flush_mutex);
}

void Log::_log_safe_write(const char* what, size_t write_len)
{
  if (m_fd < 0)
    return;
  int r = safe_write(m_fd, what, write_len);
  if (r != m_fd_last_error) {
    if (r < 0)
      cerr << "problem writing to " << m_log_file
           << ": " << cpp_strerror(r)
           << std::endl;
    m_fd_last_error = r;
  }
}

void Log::_flush_logbuf()
{
  if (m_log_buf_pos) {
    _log_safe_write(m_log_buf, m_log_buf_pos);
    m_log_buf_pos = 0;
  }
}

void Log::_flush(EntryQueue *t, EntryQueue *requeue, bool crash)
{
  Entry *e = nullptr;
  long len = 0;
  if (crash) {
    len = t->m_len;
  }
  if (!requeue) {
    e = t->m_head;
    if (!e) {
      return;
    }
  }
  while (true) {
    if (requeue) {
      e = t->dequeue();
      if (!e) {
	break;
      }
      requeue->enqueue(e);
    } else {
      e = e->m_next;
      if (!e) {
	break;
      }
    }

    unsigned sub = e->m_subsys;

    bool should_log = crash || m_subs->get_log_level(sub) >= e->m_prio;
    bool do_fd = m_fd >= 0 && should_log;
    bool do_syslog = m_syslog_crash >= e->m_prio && should_log;
    bool do_stderr = m_stderr_crash >= e->m_prio && should_log;
    bool do_graylog2 = m_graylog_crash >= e->m_prio && should_log;

    e->hint_size();
    if (do_fd || do_syslog || do_stderr) {
      size_t line_used = 0;

      char *line;
      size_t line_size = 80 + e->size();
      bool need_dynamic = line_size >= MAX_LOG_BUF;

      // this flushes the existing buffers if either line is longer
      // than our buffer, or buffer is too full to fit it
      if (m_log_buf_pos + line_size >= MAX_LOG_BUF) {
	_flush_logbuf();
      }
      if (need_dynamic) {
        line = new char[line_size];
      } else {
        line = &m_log_buf[m_log_buf_pos];
      }

      if (crash) {
        line_used += snprintf(line, line_size, "%6ld> ", -(--len));
      }
      line_used += append_time(e->m_stamp, line + line_used, line_size - line_used);
      line_used += snprintf(line + line_used, line_size - line_used, " %lx %2d ",
			(unsigned long)e->m_thread, e->m_prio);

      line_used += e->snprintf(line + line_used, line_size - line_used - 1);
      ceph_assert(line_used < line_size - 1);

      if (do_syslog) {
        syslog(LOG_USER|LOG_INFO, "%s", line);
      }

      if (do_stderr) {
        cerr << m_log_stderr_prefix << line << std::endl;
      }

      if (do_fd) {
	line[line_used] = '\n';
	if (need_dynamic) {
	  _log_safe_write(line, line_used + 1);
	  m_log_buf_pos = 0;
	} else {
	  m_log_buf_pos += line_used + 1;
	}
      } else {
	m_log_buf_pos = 0;
      }

      if (need_dynamic) {
        delete[] line;
      }
    }

    if (do_graylog2 && m_graylog) {
      m_graylog->log_entry(e);
    }

  }

  _flush_logbuf();

}

void Log::_log_message(const char *s, bool crash)
{
  if (m_fd >= 0) {
    size_t len = strlen(s);
    std::string b;
    b.reserve(len + 1);
    b.append(s, len);
    b += '\n';
    int r = safe_write(m_fd, b.c_str(), b.size());
    if (r < 0)
      cerr << "problem writing to " << m_log_file << ": " << cpp_strerror(r) << std::endl;
  }
  if ((crash ? m_syslog_crash : m_syslog_log) >= 0) {
    syslog(LOG_USER|LOG_INFO, "%s", s);
  }

  if ((crash ? m_stderr_crash : m_stderr_log) >= 0) {
    cerr << s << std::endl;
  }
}

void Log::dump_recent()
{
  pthread_mutex_lock(&m_flush_mutex);
  m_flush_mutex_holder = pthread_self();

  pthread_mutex_lock(&m_queue_mutex);
  m_queue_mutex_holder = pthread_self();

  EntryQueue t;
  t.swap(m_new);

  m_queue_mutex_holder = 0;
  pthread_mutex_unlock(&m_queue_mutex);
  _flush(&t, &m_recent, false);
  _flush_logbuf();

  _log_message("--- begin dump of recent events ---", true);
  _flush(&m_recent, nullptr, true);

  char buf[4096];
  _log_message("--- logging levels ---", true);
  for (const auto& p : m_subs->m_subsys) {
    snprintf(buf, sizeof(buf), "  %2d/%2d %s", p.log_level, p.gather_level, p.name);
    _log_message(buf, true);
  }

  sprintf(buf, "  %2d/%2d (syslog threshold)", m_syslog_log, m_syslog_crash);
  _log_message(buf, true);
  sprintf(buf, "  %2d/%2d (stderr threshold)", m_stderr_log, m_stderr_crash);
  _log_message(buf, true);
  sprintf(buf, "  max_recent %9d", m_max_recent);
  _log_message(buf, true);
  sprintf(buf, "  max_new    %9d", m_max_new);
  _log_message(buf, true);
  sprintf(buf, "  log_file %s", m_log_file.c_str());
  _log_message(buf, true);

  _log_message("--- end dump of recent events ---", true);

  _flush_logbuf();

  m_flush_mutex_holder = 0;
  pthread_mutex_unlock(&m_flush_mutex);
}

void Log::start()
{
  ceph_assert(!is_started());
  pthread_mutex_lock(&m_queue_mutex);
  m_stop = false;
  pthread_mutex_unlock(&m_queue_mutex);
  create("log");
}

void Log::stop()
{
  if (is_started()) {
    pthread_mutex_lock(&m_queue_mutex);
    m_stop = true;
    pthread_cond_signal(&m_cond_flusher);
    pthread_cond_broadcast(&m_cond_loggers);
    pthread_mutex_unlock(&m_queue_mutex);
    join();
  }
}

void *Log::entry()
{
  pthread_mutex_lock(&m_queue_mutex);
  m_queue_mutex_holder = pthread_self();
  while (!m_stop) {
    if (!m_new.empty()) {
      m_queue_mutex_holder = 0;
      pthread_mutex_unlock(&m_queue_mutex);
      flush();
      pthread_mutex_lock(&m_queue_mutex);
      m_queue_mutex_holder = pthread_self();
      continue;
    }

    pthread_cond_wait(&m_cond_flusher, &m_queue_mutex);
  }
  m_queue_mutex_holder = 0;
  pthread_mutex_unlock(&m_queue_mutex);
  flush();
  return NULL;
}

bool Log::is_inside_log_lock()
{
  return
    pthread_self() == m_queue_mutex_holder ||
    pthread_self() == m_flush_mutex_holder;
}

void Log::inject_segv()
{
  m_inject_segv = true;
}

void Log::reset_segv()
{
  m_inject_segv = false;
}

} // ceph::logging::
} // ceph::
