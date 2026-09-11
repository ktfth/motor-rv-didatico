#include "ingress/tcp_server.hpp"

#include <array>
#include <cerrno>
#include <utility>

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace rv::ingress {

namespace {

bool set_nonblocking(int fd) noexcept {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void add_stats(IngressStats& total, const IngressStats& value) noexcept {
  total.bytes_received += value.bytes_received;
  total.events_received += value.events_received;
  total.events_routed += value.events_routed;
  total.events_broadcast += value.events_broadcast;
  total.backpressure_stalls += value.backpressure_stalls;
  total.parse_errors += value.parse_errors;
}

}  // namespace

struct TcpIngressServer::Client {
  Client(int socket_fd, std::span<core::Inbox*> inboxes) : fd(socket_fd), pipeline(inboxes) {}
  int fd{-1};
  IngressPipeline pipeline;
  bool stalled{false};
};

TcpIngressServer::TcpIngressServer() noexcept = default;
TcpIngressServer::~TcpIngressServer() noexcept {
  stop();
}

bool TcpIngressServer::start(uint16_t port, std::span<core::Inbox*> inboxes) noexcept {
  if (listen_fd_ >= 0 || inboxes.empty() || inboxes.size() > IngressPipeline::kMaxPartitions) {
    return false;
  }
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;
  int enabled = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
  if (!set_nonblocking(fd)) {
    ::close(fd);
    return false;
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      ::listen(fd, static_cast<int>(kMaxClients)) != 0) {
    ::close(fd);
    return false;
  }
  sockaddr_in bound{};
  socklen_t length = sizeof(bound);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
    ::close(fd);
    return false;
  }
  inboxes_.assign(inboxes.begin(), inboxes.end());
  clients_.reserve(kMaxClients);
  bound_port_ = ntohs(bound.sin_port);
  listen_fd_ = fd;
  retired_ = TcpIngressStats{};
  return true;
}

void TcpIngressServer::stop() noexcept {
  for (auto& client : clients_) {
    add_stats(retired_.ingress, client->pipeline.stats());
    ++retired_.connections_closed;
    if (client->fd >= 0) ::close(client->fd);
  }
  clients_.clear();
  if (listen_fd_ >= 0) ::close(listen_fd_);
  listen_fd_ = -1;
  bound_port_ = 0;
  inboxes_.clear();
}

void TcpIngressServer::accept_ready() noexcept {
  while (clients_.size() < kMaxClients) {
    const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno != EAGAIN) ++retired_.connection_errors;
      return;
    }
    clients_.push_back(std::make_unique<Client>(fd, inboxes_));
    ++retired_.connections_accepted;
  }
}

void TcpIngressServer::close_client(size_t index, bool error) noexcept {
  Client& client = *clients_[index];
  add_stats(retired_.ingress, client.pipeline.stats());
  if (error) ++retired_.connection_errors;
  ++retired_.connections_closed;
  ::close(client.fd);
  clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(index));
}

void TcpIngressServer::poll_once(int timeout_ms, uint64_t now_ns) noexcept {
  if (listen_fd_ < 0) return;
  std::vector<pollfd> descriptors;
  descriptors.reserve(clients_.size() + 1);
  descriptors.push_back(pollfd{listen_fd_, POLLIN, 0});
  for (const auto& client : clients_) {
    descriptors.push_back(pollfd{client->fd, static_cast<short>(POLLIN), 0});
  }
  const int ready = ::poll(descriptors.data(), descriptors.size(), timeout_ms);
  if (ready < 0) {
    if (errno != EINTR) ++retired_.connection_errors;
    return;
  }
  if (ready == 0) return;
  if ((descriptors[0].revents & POLLIN) != 0) accept_ready();

  std::array<std::byte, 16 * 1024> bytes{};
  for (size_t i = clients_.size(); i-- > 0;) {
    Client& client = *clients_[i];
    if (client.stalled) {
      const Status retry = client.pipeline.feed(ByteSpan{}, now_ns);
      client.stalled = retry.code() == Err::WouldBlock;
      if (client.stalled) continue;
      if (retry.is_error()) {
        close_client(i, true);
        continue;
      }
    }
    const short events = (i + 1 < descriptors.size()) ? descriptors[i + 1].revents : 0;
    if ((events & (POLLERR | POLLNVAL)) != 0) {
      close_client(i, true);
      continue;
    }
    if ((events & POLLHUP) != 0 && (events & POLLIN) == 0) {
      const bool truncated = client.pipeline.finish().is_error();
      close_client(i, truncated);
      continue;
    }
    if ((events & POLLIN) == 0) continue;
    while (true) {
      const ssize_t received = ::recv(client.fd, bytes.data(), bytes.size(), 0);
      if (received > 0) {
        const Status status =
            client.pipeline.feed(ByteSpan{bytes.data(), static_cast<size_t>(received)}, now_ns);
        if (status.code() == Err::WouldBlock) {
          client.stalled = true;
          break;
        }
        if (status.is_error()) {
          close_client(i, true);
          break;
        }
        continue;
      }
      if (received == 0) {
        const bool truncated = client.pipeline.finish().is_error();
        close_client(i, truncated);
        break;
      }
      if (errno == EAGAIN) break;
      if (errno == EINTR) continue;
      close_client(i, true);
      break;
    }
  }
}

TcpIngressStats TcpIngressServer::stats() const noexcept {
  TcpIngressStats total = retired_;
  total.active_connections = clients_.size();
  for (const auto& client : clients_) add_stats(total.ingress, client->pipeline.stats());
  return total;
}

}  // namespace rv::ingress
