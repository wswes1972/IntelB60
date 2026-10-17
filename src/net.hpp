// IntelB60 - Pearl (PRL) miner for Intel Arc GPUs (fork of IAminer)
// Copyright (C) 2026 IAminer contributors. GPL-3.0-or-later (see LICENSE).
#pragma once

#include <memory>
#include <string>

namespace intelb60 {

// Line-oriented TCP/TLS client. Not thread-safe: every call must come from the owning thread.
class LineConnection {
 public:
  LineConnection();
  ~LineConnection();
  LineConnection(const LineConnection&) = delete;
  LineConnection& operator=(const LineConnection&) = delete;

  bool open(const std::string& host, int port, bool tls, std::string& error, int timeout_ms = 10000);
  void close();
  bool is_open() const;
  // 1 = got a line (without the trailing newline), 0 = timeout, -1 = closed or error.
  int read_line(std::string& line, int timeout_ms);
  bool write(const std::string& data);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

void net_global_init();

}  // namespace intelb60
