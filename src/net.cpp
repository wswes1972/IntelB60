// IntelB60 - Pearl (PRL) miner for Intel Arc GPUs (fork of IAminer)
// Copyright (C) 2026 IAminer contributors. GPL-3.0-or-later (see LICENSE).
#include "net.hpp"

#include <cstring>
#include <mutex>

#include <openssl/err.h>
#include <openssl/ssl.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static const socket_t kBadSocket = INVALID_SOCKET;
static void close_socket(socket_t s) { closesocket(s); }
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static const socket_t kBadSocket = -1;
static void close_socket(socket_t s) { ::close(s); }
#endif

namespace intelb60 {

void net_global_init() {
  static std::once_flag once;
  std::call_once(once, [] {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    OPENSSL_init_ssl(0, nullptr);
  });
}

struct LineConnection::Impl {
  socket_t sock = kBadSocket;
  SSL_CTX* ctx = nullptr;
  SSL* ssl = nullptr;
  std::string buffer;

  void reset() {
    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); ssl = nullptr; }
    if (ctx) { SSL_CTX_free(ctx); ctx = nullptr; }
    if (sock != kBadSocket) { close_socket(sock); sock = kBadSocket; }
    buffer.clear();
  }

  // Wait until the socket is readable. 1 = readable, 0 = timeout, -1 = error.
  int wait_readable(int timeout_ms) {
#ifdef _WIN32
    WSAPOLLFD p{sock, POLLRDNORM, 0};
    int rc = WSAPoll(&p, 1, timeout_ms);
#else
    pollfd p{sock, POLLIN, 0};
    int rc = ::poll(&p, 1, timeout_ms);
#endif
    if (rc < 0) return -1;
    return rc > 0 ? 1 : 0;
  }
};

LineConnection::LineConnection() : impl_(std::make_unique<Impl>()) { net_global_init(); }
LineConnection::~LineConnection() { impl_->reset(); }

bool LineConnection::is_open() const { return impl_->sock != kBadSocket; }
void LineConnection::close() { impl_->reset(); }

bool LineConnection::open(const std::string& host, int port, bool tls, std::string& error, int timeout_ms) {
  impl_->reset();
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  std::string port_s = std::to_string(port);
  if (getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res) != 0 || !res) {
    error = "cannot resolve " + host;
    return false;
  }
  for (addrinfo* ai = res; ai; ai = ai->ai_next) {
    socket_t s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == kBadSocket) continue;
#ifdef _WIN32
    DWORD tv = (DWORD)timeout_ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);
#else
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
    if (::connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0) {
      int one = 1;
      setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
      setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char*)&one, sizeof one);
      impl_->sock = s;
      break;
    }
    close_socket(s);
  }
  freeaddrinfo(res);
  if (impl_->sock == kBadSocket) {
    error = "cannot connect to " + host + ":" + port_s;
    return false;
  }
  if (tls) {
    impl_->ctx = SSL_CTX_new(TLS_client_method());
    // Mining pools commonly present self-signed certificates; TLS here protects the session from
    // passive observation, it does not authenticate the pool.
    SSL_CTX_set_verify(impl_->ctx, SSL_VERIFY_NONE, nullptr);
    impl_->ssl = SSL_new(impl_->ctx);
    SSL_set_tlsext_host_name(impl_->ssl, host.c_str());
    SSL_set_fd(impl_->ssl, (int)impl_->sock);
    if (SSL_connect(impl_->ssl) != 1) {
      char buf[256];
      ERR_error_string_n(ERR_get_error(), buf, sizeof buf);
      error = std::string("TLS handshake failed: ") + buf;
      impl_->reset();
      return false;
    }
  }
  return true;
}

int LineConnection::read_line(std::string& line, int timeout_ms) {
  Impl& s = *impl_;
  if (s.sock == kBadSocket) return -1;
  for (;;) {
    size_t nl = s.buffer.find('\n');
    if (nl != std::string::npos) {
      line = s.buffer.substr(0, nl);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      s.buffer.erase(0, nl + 1);
      return 1;
    }
    if (!(s.ssl && SSL_pending(s.ssl) > 0)) {
      int w = s.wait_readable(timeout_ms);
      if (w <= 0) return w;
    }
    char buf[16384];
    int n;
    if (s.ssl) {
      n = SSL_read(s.ssl, buf, sizeof buf);
      if (n <= 0) {
        int e = SSL_get_error(s.ssl, n);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
        return -1;
      }
    } else {
      n = (int)::recv(s.sock, buf, sizeof buf, 0);
      if (n <= 0) return -1;
    }
    s.buffer.append(buf, (size_t)n);
    if (s.buffer.size() > (8u << 20)) return -1;  // runaway line guard
    timeout_ms = 0;  // a partial line arrived; keep draining without blocking long
  }
}

bool LineConnection::write(const std::string& data) {
  Impl& s = *impl_;
  if (s.sock == kBadSocket) return false;
  size_t off = 0;
  while (off < data.size()) {
    int n;
    if (s.ssl) n = SSL_write(s.ssl, data.data() + off, (int)(data.size() - off));
    else n = (int)::send(s.sock, data.data() + off, (int)(data.size() - off), 0);
    if (n <= 0) return false;
    off += (size_t)n;
  }
  return true;
}

}  // namespace intelb60
