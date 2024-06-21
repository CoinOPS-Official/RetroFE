#pragma once

#include "ThreadPool.h"
#include <memory>

class SingletonThreadPool {
public:
  static ThreadPool &getInstance() {
    static ThreadPool instance(std::thread::hardware_concurrency());
    return instance;
  }

private:
  SingletonThreadPool() = default;
  ~SingletonThreadPool() = default;
  SingletonThreadPool(const SingletonThreadPool &) = delete;
  SingletonThreadPool &operator=(const SingletonThreadPool &) = delete;
};
