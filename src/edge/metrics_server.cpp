#include "edge/metrics_server.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string_view>

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace rv::edge {

namespace {

void set_nonblocking(int fd) noexcept {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}

void send_all(int fd, const char* data, size_t len) noexcept {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
    if (n > 0) {
      sent += static_cast<size_t>(n);
    } else if (n < 0 && (errno == EAGAIN)) {
      struct pollfd pfd{};
      pfd.fd = fd;
      pfd.events = POLLOUT;
      if (::poll(&pfd, 1, 100) <= 0) break;
    } else {
      break;
    }
  }
}

}  // namespace

MetricsServer::MetricsServer() noexcept = default;

MetricsServer::~MetricsServer() noexcept {
  stop();
}

bool MetricsServer::bind_and_listen(uint16_t port) noexcept {
  if (listen_fd_ >= 0) return true;

  if (::pipe(stop_pipe_) != 0) {
    return false;
  }
  set_nonblocking(stop_pipe_[0]);
  set_nonblocking(stop_pipe_[1]);

  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    ::close(stop_pipe_[0]);
    ::close(stop_pipe_[1]);
    stop_pipe_[0] = stop_pipe_[1] = -1;
    return false;
  }

  int opt = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  set_nonblocking(listen_fd_);

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);

  if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(listen_fd_);
    ::close(stop_pipe_[0]);
    ::close(stop_pipe_[1]);
    listen_fd_ = stop_pipe_[0] = stop_pipe_[1] = -1;
    return false;
  }

  if (port == 0) {
    struct sockaddr_in bound_addr{};
    socklen_t addr_len = sizeof(bound_addr);
    if (::getsockname(listen_fd_, reinterpret_cast<struct sockaddr*>(&bound_addr), &addr_len) ==
        0) {
      bound_port_ = ntohs(bound_addr.sin_port);
    }
  } else {
    bound_port_ = port;
  }

  if (::listen(listen_fd_, 32) != 0) {
    ::close(listen_fd_);
    ::close(stop_pipe_[0]);
    ::close(stop_pipe_[1]);
    listen_fd_ = stop_pipe_[0] = stop_pipe_[1] = -1;
    return false;
  }

  return true;
}

bool MetricsServer::start(uint16_t port, const ObservabilityCollector& collector) noexcept {
  if (running_.load(std::memory_order_acquire)) return true;

  collector_ = &collector;
  if (!bind_and_listen(port)) {
    return false;
  }

  running_.store(true, std::memory_order_release);
  worker_thread_ = std::thread([this]() { run_loop(); });
  return true;
}

void MetricsServer::stop() noexcept {
  if (!running_.exchange(false, std::memory_order_acq_rel)) {
    // Se não estava rodando mas socket estava aberto
    if (listen_fd_ >= 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
    }
    if (stop_pipe_[0] >= 0) {
      ::close(stop_pipe_[0]);
      ::close(stop_pipe_[1]);
      stop_pipe_[0] = stop_pipe_[1] = -1;
    }
    return;
  }

  // Notifica o pipe para destravar o poll
  char dummy = 1;
  (void)::write(stop_pipe_[1], &dummy, 1);

  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }

  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (stop_pipe_[0] >= 0) {
    ::close(stop_pipe_[0]);
    ::close(stop_pipe_[1]);
    stop_pipe_[0] = stop_pipe_[1] = -1;
  }
}

void MetricsServer::run_loop() noexcept {
  while (running_.load(std::memory_order_relaxed)) {
    poll_once(100);
  }
}

void MetricsServer::poll_once(int timeout_ms) noexcept {
  if (listen_fd_ < 0) return;

  struct pollfd pfds[2];
  pfds[0].fd = listen_fd_;
  pfds[0].events = POLLIN;
  pfds[0].revents = 0;

  pfds[1].fd = stop_pipe_[0];
  pfds[1].events = POLLIN;
  pfds[1].revents = 0;

  int ret = ::poll(pfds, 2, timeout_ms);
  if (ret <= 0) return;

  if (pfds[1].revents & POLLIN) {
    char buf[16];
    (void)::read(stop_pipe_[0], buf, sizeof(buf));
    return;
  }

  if (pfds[0].revents & POLLIN) {
    while (true) {
      struct sockaddr_in client_addr{};
      socklen_t client_len = sizeof(client_addr);
      int client_fd =
          ::accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
      if (client_fd < 0) {
        if (errno == EAGAIN) break;
        break;
      }
      set_nonblocking(client_fd);
      handle_connection(client_fd);
      ::close(client_fd);
    }
  }
}

void MetricsServer::handle_connection(int client_fd) noexcept {
  struct pollfd pfd{};
  pfd.fd = client_fd;
  pfd.events = POLLIN;
  if (::poll(&pfd, 1, 1000) <= 0) return;

  char req_buf[2048];
  ssize_t n = ::recv(client_fd, req_buf, sizeof(req_buf) - 1, 0);
  if (n <= 0) return;
  req_buf[n] = '\0';

  std::string_view req(req_buf, static_cast<size_t>(n));

  // Parse básico do método e path na primeira linha
  const auto eol = req.find("\r\n");
  const auto line = (eol != std::string_view::npos) ? req.substr(0, eol) : req;

  if (!line.starts_with("GET ")) {
    const char* resp405 =
        "HTTP/1.1 405 Method Not Allowed\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 19\r\n"
        "Connection: close\r\n\r\n"
        "Method Not Allowed\n";
    send_all(client_fd, resp405, std::strlen(resp405));
    return;
  }

  const auto path_start = 4;  // após "GET "
  const auto path_end = line.find(' ', path_start);
  const auto path = (path_end != std::string_view::npos)
                        ? line.substr(path_start, path_end - path_start)
                        : line.substr(path_start);

  // Buffers pré-alocados para respostas
  static thread_local std::array<char, 128 * 1024> payload_buf;
  char header_buf[256];

  if (path == "/metrics") {
    size_t payload_len = 0;
    if (collector_) {
      payload_len = collector_->render_prometheus(payload_buf);
    }
    int hdr_len = std::snprintf(header_buf, sizeof(header_buf),
                                "HTTP/1.1 200 OK\r\n"
                                "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
                                "Content-Length: %zu\r\n"
                                "Connection: close\r\n\r\n",
                                payload_len);
    if (hdr_len > 0) {
      send_all(client_fd, header_buf, static_cast<size_t>(hdr_len));
      send_all(client_fd, payload_buf.data(), payload_len);
    }
  } else if (path == "/healthz") {
    const bool healthy = collector_ ? collector_->is_healthy() : false;
    const char* body = healthy ? "ok\n" : "degraded\n";
    const size_t body_len = std::strlen(body);
    const int code = healthy ? 200 : 503;
    const char* status_str = healthy ? "OK" : "Service Unavailable";

    int hdr_len = std::snprintf(header_buf, sizeof(header_buf),
                                "HTTP/1.1 %d %s\r\n"
                                "Content-Type: text/plain\r\n"
                                "Content-Length: %zu\r\n"
                                "Connection: close\r\n\r\n",
                                code, status_str, body_len);
    if (hdr_len > 0) {
      send_all(client_fd, header_buf, static_cast<size_t>(hdr_len));
      send_all(client_fd, body, body_len);
    }
  } else if (path == "/ready") {
    const bool ready = collector_ ? collector_->is_ready() : false;
    const char* body = ready ? "ready\n" : "not ready\n";
    const size_t body_len = std::strlen(body);
    const int code = ready ? 200 : 503;
    const char* status_str = ready ? "OK" : "Service Unavailable";

    int hdr_len = std::snprintf(header_buf, sizeof(header_buf),
                                "HTTP/1.1 %d %s\r\n"
                                "Content-Type: text/plain\r\n"
                                "Content-Length: %zu\r\n"
                                "Connection: close\r\n\r\n",
                                code, status_str, body_len);
    if (hdr_len > 0) {
      send_all(client_fd, header_buf, static_cast<size_t>(hdr_len));
      send_all(client_fd, body, body_len);
    }
  } else if (path == "/status") {
    size_t payload_len = 0;
    if (collector_) {
      payload_len = collector_->render_json_status(payload_buf);
    }
    int hdr_len = std::snprintf(header_buf, sizeof(header_buf),
                                "HTTP/1.1 200 OK\r\n"
                                "Content-Type: application/json\r\n"
                                "Content-Length: %zu\r\n"
                                "Connection: close\r\n\r\n",
                                payload_len);
    if (hdr_len > 0) {
      send_all(client_fd, header_buf, static_cast<size_t>(hdr_len));
      send_all(client_fd, payload_buf.data(), payload_len);
    }
  } else {
    const char* body = "not found\n";
    const size_t body_len = std::strlen(body);
    int hdr_len = std::snprintf(header_buf, sizeof(header_buf),
                                "HTTP/1.1 404 Not Found\r\n"
                                "Content-Type: text/plain\r\n"
                                "Content-Length: %zu\r\n"
                                "Connection: close\r\n\r\n",
                                body_len);
    if (hdr_len > 0) {
      send_all(client_fd, header_buf, static_cast<size_t>(hdr_len));
      send_all(client_fd, body, body_len);
    }
  }
}

}  // namespace rv::edge
