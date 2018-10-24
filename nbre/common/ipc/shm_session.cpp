// Copyright (C) 2018 go-nebulas authors
//
// This file is part of the go-nebulas library.
//
// the go-nebulas library is free software: you can redistribute it and/or
// modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// the go-nebulas library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with the go-nebulas library.  If not, see
// <http://www.gnu.org/licenses/>.
//
#include "common/ipc/shm_session.h"
#include "common/common.h"
#include "common/exception_queue.h"
#include <chrono>

namespace neb {
namespace ipc {
const static char *bookkeeper_mem_name = "nbre.sessions";

void clean_shm_session_env() { clean_bookkeeper_env(bookkeeper_mem_name); }
namespace internal {
size_t max_wait_fail_times = 8;

shm_session_base::shm_session_base(const std::string &name, bool need_reset)
    : quitable_thread(), m_name(name) {
  LOG(INFO) << "session base enter";
  m_bookkeeper =
      std::unique_ptr<shm_bookkeeper>(new shm_bookkeeper(bookkeeper_mem_name));
  if (!need_reset) {
    LOG(INFO) << "session base 1";
    m_server_sema = m_bookkeeper->acquire_named_semaphore(server_sema_name());
    LOG(INFO) << "session base 2";
    m_client_sema = m_bookkeeper->acquire_named_semaphore(client_sema_name());
    LOG(INFO) << "session base end";
  }
}

shm_session_base::~shm_session_base() {
  if (m_thread) {
    m_thread->join();
    m_thread.reset();
  }
  LOG(INFO) << "to release sema";
  m_bookkeeper->release_named_semaphore(server_sema_name());
  m_bookkeeper->release_named_semaphore(client_sema_name());
}

void shm_session_base::reset() {
  LOG(INFO) << "to reset all ";
  m_bookkeeper->reset();
  LOG(INFO) << "reset bookkeeper done ";
  boost::interprocess::named_semaphore::remove(server_sema_name().c_str());
  boost::interprocess::named_semaphore::remove(client_sema_name().c_str());
  m_server_sema = m_bookkeeper->acquire_named_semaphore(server_sema_name());
  m_client_sema = m_bookkeeper->acquire_named_semaphore(client_sema_name());
  LOG(INFO) << "reset done ";
}

void shm_session_base::start_session() { start(); }

shm_session_util::shm_session_util(const std::string &name)
    : shm_session_base(name, true) {}

void shm_session_util::thread_func() {}

shm_session_server::shm_session_server(const std::string &name)
    : shm_session_base(name, false), m_client_started(false),
      m_client_alive(false) {
  LOG(INFO) << "shm_session_server cnt done ";
}

void shm_session_server::wait_until_client_start() {
  if (m_client_started)
    return;
  m_client_sema->wait();
  m_client_started = true;
}

bool shm_session_server::is_client_alive() { return m_client_alive; }

void shm_session_server::start_session() {
  start();
}
void shm_session_server::thread_func() {
  LOG(INFO) << "server thread_func started ";
  struct quit_helper {
    quit_helper(shm_bookkeeper *bk, const std::string &name)
        : m_bk(bk), m_name(name), m_to_unlock(false) {
      m_mutex = m_bk->acquire_named_mutex(m_name);
    };
    ~quit_helper() {
      if (m_to_unlock) {
        m_mutex->unlock();
      }
      m_bk->release_named_mutex(m_name);
    }
    shm_bookkeeper *m_bk;
    std::string m_name;
    std::unique_ptr<boost::interprocess::named_mutex> m_mutex;
    bool m_to_unlock;
  } _l(m_bookkeeper.get(), m_name + ".session_server.mutex");
  if (_l.m_mutex->try_lock()) {
    _l.m_to_unlock = true;
  } else {
    LOG(INFO) << "server thread_func throw shm_session_already_start";
    throw shm_session_already_start();
  }

  uint32_t fail_counter = 0;
  while (!m_exit_flag) {
    LOG(INFO) << "server loop";

    if (!m_client_started) {
      bool ret = m_client_sema->try_wait();
      if (ret) {
        m_client_started = true;
        m_client_alive = true;
      }
    } else {
      LOG(INFO) << "client started";
      bool ret = m_client_sema->try_wait();
      if (ret) {
        fail_counter = 0;
        m_client_alive = true;
      } else {
        fail_counter++;
        if (fail_counter >= max_wait_fail_times) {
          m_client_alive = false;
          throw shm_session_timeout();
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    LOG(INFO) << "sema pointer: " << (void *)m_server_sema.get();
    m_server_sema->post();
  }
}

shm_session_client::shm_session_client(const std::string &name)
    : shm_session_base(name, false), m_server_alive(false) {}

void shm_session_client::thread_func() {

  LOG(INFO) << "client thread_func start";
  struct quit_helper {
    quit_helper(shm_bookkeeper *bk, const std::string &name)
        : m_bk(bk), m_name(name), m_to_unlock(false) {
      m_mutex = m_bk->acquire_named_mutex(m_name);
    };
    ~quit_helper() {
      if (m_to_unlock) {
        m_mutex->unlock();
      }
      m_bk->release_named_mutex(m_name);
    }
    shm_bookkeeper *m_bk;
    std::string m_name;
    std::unique_ptr<boost::interprocess::named_mutex> m_mutex;
    bool m_to_unlock;
  } _l(m_bookkeeper.get(), m_name + ".session_client.mutex");

  LOG(INFO) << "client quit_helper done";
  if (_l.m_mutex->try_lock()) {
    _l.m_to_unlock = true;
  } else {
    throw shm_session_already_start();
  }

  LOG(INFO) << "client loop";
  uint32_t fail_counter = 0;
  while (!m_exit_flag) {
    LOG(INFO) << "client loop";
    bool ret = m_server_sema->try_wait();
    if (ret) {
      LOG(INFO) << "client wait succ";
      fail_counter = 0;
      m_server_alive = true;
    } else {
      LOG(INFO) << "client wait fail";
      fail_counter++;
      if (fail_counter >= max_wait_fail_times) {
        m_server_alive = false;
        LOG(INFO) << "client timeout";
        throw shm_session_timeout();
      }
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    m_client_sema->post();
  }
}

bool shm_session_client::is_server_alive() { return m_server_alive; }
}
}
}
