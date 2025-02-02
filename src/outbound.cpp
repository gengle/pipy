/*
 *  Copyright (c) 2019 by flomesh.io
 *
 *  Unless prior written consent has been obtained from the copyright
 *  owner, the following shall not be allowed.
 *
 *  1. The distribution of any source codes, header files, make files,
 *     or libraries of the software.
 *
 *  2. Disclosure of any source codes pertaining to the software to any
 *     additional parties.
 *
 *  3. Alteration or removal of any notices in or on the software or
 *     within the documentation included within the software.
 *
 *  ALL SOURCE CODE AS WELL AS ALL DOCUMENTATION INCLUDED WITH THIS
 *  SOFTWARE IS PROVIDED IN AN “AS IS” CONDITION, WITHOUT WARRANTY OF ANY
 *  KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 *  OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 *  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 *  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 *  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 *  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "outbound.hpp"
#include "constants.hpp"
#include "pipeline.hpp"
#include "utils.hpp"
#include "log.hpp"

#include <iostream>

namespace pipy {

using tcp = asio::ip::tcp;
using udp = asio::ip::udp;

thread_local static Data::Producer s_dp_tcp("OutboundTCP");
thread_local static Data::Producer s_dp_udp("OutboundUDP");

//
// Outbound
//

thread_local List<Outbound> Outbound::s_all_outbounds;
thread_local pjs::Ref<stats::Gauge> Outbound::s_metric_concurrency;
thread_local pjs::Ref<stats::Counter> Outbound::s_metric_traffic_in;
thread_local pjs::Ref<stats::Counter> Outbound::s_metric_traffic_out;
thread_local pjs::Ref<stats::Histogram> Outbound::s_metric_conn_time;

Outbound::Outbound(EventTarget::Input *input, const Options &options)
  : m_options(options)
  , m_input(input)
{
  init_metrics();
  Log::debug(Log::ALLOC, "[outbound %p] ++", this);
  s_all_outbounds.push(this);
}

Outbound::~Outbound() {
  Log::debug(Log::ALLOC, "[outbound %p] --", this);
  s_all_outbounds.remove(this);
}

auto Outbound::protocol_name() const -> pjs::Str* {
  thread_local static pjs::ConstStr s_TCP("TCP");
  thread_local static pjs::ConstStr s_UDP("UDP");
  switch (m_options.protocol) {
    case Protocol::TCP: return s_TCP;
    case Protocol::UDP: return s_UDP;
  }
  return nullptr;
}

auto Outbound::local_address() -> pjs::Str* {
  if (!m_local_addr_str) {
    m_local_addr_str = pjs::Str::make(m_local_addr);
  }
  return m_local_addr_str;
}

auto Outbound::remote_address() -> pjs::Str* {
  if (!m_remote_addr_str) {
    m_remote_addr_str = pjs::Str::make(m_remote_addr);
  }
  return m_remote_addr_str;
}

auto Outbound::address() -> pjs::Str* {
  if (!m_address) {
    std::string s("[");
    s += m_host;
    s += "]:";
    s += std::to_string(m_port);
    m_address = pjs::Str::make(std::move(s));
  }
  return m_address;
}

void Outbound::state(State state) {
  m_state = state;
  if (const auto &f = m_options.on_state_changed) {
    f(this);
  }
}

void Outbound::input(Event *evt) {
  m_input->input(evt);
}

void Outbound::error(StreamEnd::Error err) {
  m_error = err;
  input(StreamEnd::make(err));
  state(State::closed);
}

void Outbound::describe(char *buf, size_t len) {
  std::snprintf(
    buf, len,
    "[outbound %p] [%s]:%d -> [%s]:%d (%s)",
    this,
    m_local_addr.empty() ? "0.0.0.0" : m_local_addr.c_str(),
    m_local_port,
    m_remote_addr.c_str(),
    m_port,
    m_host.c_str()
  );
}

void Outbound::init_metrics() {
  if (!s_metric_concurrency) {
    pjs::Ref<pjs::Array> label_names = pjs::Array::make();
    label_names->length(2);
    label_names->set(0, "protocol");
    label_names->set(1, "peer");

    s_metric_concurrency = stats::Gauge::make(
      pjs::Str::make("pipy_outbound_count"),
      label_names,
      [=](stats::Gauge *gauge) {
        int total = 0;
        gauge->zero_all();
        Outbound::for_each([&](Outbound *outbound) {
          pjs::Str *k[2];
          k[0] = outbound->protocol_name();
          k[1] = outbound->address();
          auto cnt = gauge->with_labels(k, 2);
          cnt->increase();
          total++;
        });
        gauge->set(total);
      }
    );

    s_metric_traffic_in = stats::Counter::make(
      pjs::Str::make("pipy_outbound_in"),
      label_names,
      [=](stats::Counter *counter) {
        Outbound::for_each([&](Outbound *outbound) {
          auto n = outbound->get_traffic_in();
          outbound->m_metric_traffic_in->increase(n);
          s_metric_traffic_in->increase(n);
        });
      }
    );

    s_metric_traffic_out = stats::Counter::make(
      pjs::Str::make("pipy_outbound_out"),
      label_names,
      [=](stats::Counter *counter) {
        Outbound::for_each([&](Outbound *outbound) {
          auto n = outbound->get_traffic_out();
          outbound->m_metric_traffic_out->increase(n);
          s_metric_traffic_out->increase(n);
        });
      }
    );

    pjs::Ref<pjs::Array> buckets = pjs::Array::make(21);
    double limit = 1.5;
    for (int i = 0; i < 20; i++) {
      buckets->set(i, std::floor(limit));
      limit *= 1.5;
    }
    buckets->set(20, std::numeric_limits<double>::infinity());

    s_metric_conn_time = stats::Histogram::make(
      pjs::Str::make("pipy_outbound_conn_time"),
      buckets, label_names
    );
  }
}

//
// OutboundTCP
//

OutboundTCP::OutboundTCP(EventTarget::Input *output, const Outbound::Options &options)
  : pjs::ObjectTemplate<OutboundTCP, Outbound>(output, options)
  , SocketTCP(false, Outbound::m_options)
  , m_resolver(Net::context())
{
}

void OutboundTCP::bind(const std::string &ip, int port) {
  auto &s = SocketTCP::socket();
  tcp::endpoint ep(asio::ip::make_address(ip), port);
  s.open(ep.protocol());
  s.bind(ep);
  const auto &local = s.local_endpoint();
  m_local_addr = local.address().to_string();
  m_local_port = local.port();
  m_local_addr_str = nullptr;
}

void OutboundTCP::connect(const std::string &host, int port) {
  m_host = host;
  m_port = port;

  pjs::Str *keys[2];
  keys[0] = protocol_name();
  keys[1] = address();
  m_metric_traffic_out = Outbound::s_metric_traffic_out->with_labels(keys, 2);
  m_metric_traffic_in = Outbound::s_metric_traffic_in->with_labels(keys, 2);
  m_metric_conn_time = Outbound::s_metric_conn_time->with_labels(keys, 2);

  start(0);
}

void OutboundTCP::send(Event *evt) {
  SocketTCP::output(evt);
}

void OutboundTCP::close() {
  asio::error_code ec;
  switch (state()) {
    case State::resolving:
    case State::connecting:
      m_resolver.cancel();
      m_connect_timer.cancel();
      SocketTCP::socket().cancel(ec);
      break;
    case State::connected:
      SocketTCP::close();
      break;
    default: break;
  }
  m_state = State::closed;
}

void OutboundTCP::start(double delay) {
  if (delay > 0) {
    m_retry_timer.schedule(
      delay,
      [this]() {
        resolve();
      }
    );
    state(State::idle);
  } else {
    resolve();
  }
}

void OutboundTCP::resolve() {
  static const std::string s_localhost("localhost");
  static const std::string s_localhost_ip("127.0.0.1");
  const auto &host = (m_host == s_localhost ? s_localhost_ip : m_host);

  m_resolver.async_resolve(
    tcp::resolver::query(host, std::to_string(m_port)),
    [this](
      const std::error_code &ec,
      tcp::resolver::results_type results
    ) {
      InputContext ic;

      if (ec && options().connect_timeout > 0) {
        m_connect_timer.cancel();
      }

      if (ec != asio::error::operation_aborted) {
        if (ec) {
          if (Log::is_enabled(Log::ERROR)) {
            char desc[1000];
            describe(desc, sizeof(desc));
            Log::error("%s cannot resolve hostname: %s", desc, ec.message().c_str());
          }
          connect_error(StreamEnd::CANNOT_RESOLVE);

        } else if (state() == State::resolving) {
          auto &result = *results;
          const auto &target = result.endpoint();
          m_remote_addr = target.address().to_string();
          m_remote_addr_str = nullptr;
          connect(target);
        }
      }

      release();
    }
  );

  log_debug("resolving hostname...");

  if (options().connect_timeout > 0) {
    m_connect_timer.schedule(
      options().connect_timeout,
      [this]() {
        connect_error(StreamEnd::CONNECTION_TIMEOUT);
      }
    );
  }

  m_start_time = utils::now();

  if (m_retries > 0) {
    if (Log::is_enabled(Log::WARN)) {
      char desc[200];
      describe(desc, sizeof(desc));
      Log::warn("%s retry connecting... (retries = %d)", desc, m_retries);
    }
  }

  retain();
  state(State::resolving);
}

void OutboundTCP::connect(const asio::ip::tcp::endpoint &target) {
  socket().async_connect(
    target,
    [=](const std::error_code &ec) {
      InputContext ic;

      if (options().connect_timeout > 0) {
        m_connect_timer.cancel();
      }

      if (ec != asio::error::operation_aborted) {
        if (ec) {
          if (Log::is_enabled(Log::ERROR)) {
            char desc[200];
            describe(desc, sizeof(desc));
            Log::error("%s cannot connect: %s", desc, ec.message().c_str());
          }
          connect_error(StreamEnd::CONNECTION_REFUSED);

        } else if (state() == State::connecting) {
          const auto &ep = socket().local_endpoint();
          m_local_addr = ep.address().to_string();
          m_local_port = ep.port();
          m_local_addr_str = nullptr;

          auto conn_time = utils::now() - m_start_time;
          m_connection_time += conn_time;
          m_metric_conn_time->observe(conn_time);
          s_metric_conn_time->observe(conn_time);

          if (Log::is_enabled(Log::OUTBOUND)) {
            char desc[200];
            describe(desc, sizeof(desc));
            Log::debug(Log::OUTBOUND, "%s connected in %g ms", desc, conn_time);
          }

          state(State::connected);
          SocketTCP::start();
        }
      }

      release();
    }
  );

  if (Log::is_enabled(Log::OUTBOUND)) {
    char desc[200];
    describe(desc, sizeof(desc));
    Log::debug(Log::OUTBOUND, "%s connecting...", desc);
  }

  retain();
  state(State::connecting);
}

void OutboundTCP::connect_error(StreamEnd::Error err) {
  if (options().retry_count >= 0 && m_retries >= options().retry_count) {
    error(err);
  } else {
    m_retries++;
    std::error_code ec;
    socket().close(ec);
    m_resolver.cancel();
    state(State::idle);
    start(options().retry_delay);
  }
}

auto OutboundTCP::get_traffic_in() -> size_t {
  auto n = SocketTCP::m_traffic_read;
  SocketTCP::m_traffic_read = 0;
  return n;
}

auto OutboundTCP::get_traffic_out() -> size_t {
  auto n = SocketTCP::m_traffic_write;
  SocketTCP::m_traffic_write = 0;
  return n;
}

//
// OutboundUDP
//

OutboundUDP::OutboundUDP(EventTarget::Input *output, const Options &options)
  : pjs::ObjectTemplate<OutboundUDP, Outbound>(output, options)
  , m_resolver(Net::context())
  , m_socket(Net::context())
{
}

void OutboundUDP::bind(const std::string &ip, int port) {
  udp::endpoint ep(asio::ip::make_address(ip), port);
  m_socket.open(ep.protocol());
  m_socket.bind(ep);
  const auto &local = m_socket.local_endpoint();
  m_local_addr = local.address().to_string();
  m_local_port = local.port();
  m_local_addr_str = nullptr;
}

void OutboundUDP::connect(const std::string &host, int port) {
  m_host = host;
  m_port = port;
  m_connecting = true;

  pjs::Str *keys[2];
  keys[0] = protocol_name();
  keys[1] = address();
  m_metric_traffic_out = Outbound::s_metric_traffic_out->with_labels(keys, 2);
  m_metric_traffic_in = Outbound::s_metric_traffic_in->with_labels(keys, 2);
  m_metric_conn_time = Outbound::s_metric_conn_time->with_labels(keys, 2);

  start(0);
}

void OutboundUDP::send(Event *evt) {
  if (evt->is<MessageStart>()) {
    if (!m_ended) {
      m_message_started = true;
      m_buffer.clear();
    }

  } else if (auto *data = evt->as<Data>()) {
    if (m_message_started) {
      m_buffer.push(*data);
    }

  } else if (evt->is<MessageEnd>()) {
    if (m_message_started) {
      m_pending_buffer.push(Data::make(std::move(m_buffer)));
      m_message_started = false;
      pump();
    }

  } else if (evt->is<StreamEnd>()) {
    if (!m_ended) {
      m_ended = true;
      m_message_started = false;
      pump();
    }
  }
}

void OutboundUDP::close() {
  asio::error_code ec;

  if (m_connecting) {
    m_connecting = false;
    m_connect_timer.cancel();
    m_retry_timer.cancel();
    m_resolver.cancel();
    m_socket.cancel(ec);
  } else if (m_connected) {
    m_idle_timer.cancel();
  }

  m_message_started = false;
  m_ended = false;
  m_retries = 0;
  m_connected = false;
  m_buffer.clear();
  m_pending_buffer.clear();
  m_socket.shutdown(udp::socket::shutdown_both, ec);
  m_socket.close(ec);
}

void OutboundUDP::start(double delay) {
  if (delay > 0) {
    m_retry_timer.schedule(
      delay,
      [this]() {
        resolve();
      }
    );
    state(State::idle);
  } else {
    resolve();
  }
}

void OutboundUDP::resolve() {
  auto host = m_host;
  if (host == "localhost") host = "127.0.0.1";

  m_resolver.async_resolve(
    udp::resolver::query(host, std::to_string(m_port)),
    [this](
      const std::error_code &ec,
      udp::resolver::results_type results
    ) {
      InputContext ic;

      if (ec && m_options.connect_timeout > 0) {
        m_connect_timer.cancel();
      }

      if (ec != asio::error::operation_aborted) {
        if (ec) {
          if (Log::is_enabled(Log::ERROR)) {
            char desc[200];
            describe(desc, sizeof(desc));
            Log::error("%s cannot resolve hostname: %s", desc, ec.message().c_str());
          }
          restart(StreamEnd::CANNOT_RESOLVE);

        } else {
          auto &result = *results;
          const auto &target = result.endpoint();
          m_remote_addr = target.address().to_string();
          m_remote_addr_str = nullptr;
          connect(target);
        }
      }

      release();
    }
  );

  if (Log::is_enabled(Log::OUTBOUND)) {
    char desc[200];
    describe(desc, sizeof(desc));
    Log::debug(Log::OUTBOUND, "%s resolving hostname...", desc);
  }

  if (m_options.connect_timeout > 0) {
    m_connect_timer.schedule(
      m_options.connect_timeout,
      [this]() {
        asio::error_code ec;
        m_resolver.cancel();
        m_socket.cancel(ec);
        restart(StreamEnd::CONNECTION_TIMEOUT);
      }
    );
  }

  m_start_time = utils::now();

  if (m_retries > 0) {
    if (Log::is_enabled(Log::WARN)) {
      char desc[200];
      describe(desc, sizeof(desc));
      Log::warn("%s retry connecting... (retries = %d)", desc, m_retries);
    }
  }

  retain();
  state(State::resolving);
}

void OutboundUDP::connect(const asio::ip::udp::endpoint &target) {
  m_socket.async_connect(
    target,
    [=](const std::error_code &ec) {
      InputContext ic;

      if (m_options.connect_timeout > 0) {
        m_connect_timer.cancel();
      }

      if (ec != asio::error::operation_aborted) {
        if (ec) {
          if (Log::is_enabled(Log::ERROR)) {
            char desc[200];
            describe(desc, sizeof(desc));
            Log::error("%s cannot connect: %s", desc, ec.message().c_str());
          }
          restart(StreamEnd::CONNECTION_REFUSED);

        } else {
          if (Log::is_enabled(Log::OUTBOUND)) {
            char desc[200];
            describe(desc, sizeof(desc));
            Log::debug(Log::OUTBOUND, "%s connected", desc);
          }
          if (m_connecting) {
            const auto &ep = m_socket.local_endpoint();
            m_local_addr = ep.address().to_string();
            m_local_port = ep.port();
            m_local_addr_str = nullptr;
            auto conn_time = utils::now() - m_start_time;
            m_connection_time += conn_time;
            m_metric_conn_time->observe(conn_time);
            s_metric_conn_time->observe(conn_time);
            m_connected = true;
            m_connecting = false;
            state(State::connected);
            receive();
            pump();
          } else {
            close(StreamEnd::CONNECTION_CANCELED);
          }
        }
      }

      release();
    }
  );

  if (Log::is_enabled(Log::OUTBOUND)) {
    char desc[200];
    describe(desc, sizeof(desc));
    Log::debug(Log::OUTBOUND, "%s connecting...", desc);
  }

  retain();
  state(State::connecting);
}

void OutboundUDP::restart(StreamEnd::Error err) {
  if (m_options.retry_count >= 0 && m_retries >= m_options.retry_count) {
    m_connecting = false;
    error(err);
  } else {
    m_retries++;
    std::error_code ec;
    m_socket.shutdown(udp::socket::shutdown_both, ec);
    m_socket.close(ec);
    start(m_options.retry_delay);
  }
}

void OutboundUDP::receive() {
  if (!m_socket.is_open()) return;

  pjs::Ref<Data> buffer(Data::make(m_options.max_packet_size, &s_dp_udp));

  m_socket.async_receive(
    DataChunks(buffer->chunks()),
    [=](const std::error_code &ec, size_t n) {
      InputContext ic;

      if (ec != asio::error::operation_aborted) {
        if (n > 0) {
          if (m_socket.is_open()) {
            buffer->pop(buffer->size() - n);
          }
          m_metric_traffic_in->increase(buffer->size());
          s_metric_traffic_in->increase(buffer->size());
          input(MessageStart::make());
          input(buffer);
          input(MessageEnd::make());
        }

        if (ec) {
          if (ec == asio::error::eof) {
            if (Log::is_enabled(Log::OUTBOUND)) {
              char desc[200];
              describe(desc, sizeof(desc));
              Log::debug(Log::OUTBOUND, "%s connection closed by peer", desc);
            }
            close(StreamEnd::NO_ERROR);
          } else if (ec == asio::error::connection_reset) {
            if (Log::is_enabled(Log::WARN)) {
              char desc[200];
              describe(desc, sizeof(desc));
              Log::warn("%s connection reset by peer", desc);
            }
            close(StreamEnd::CONNECTION_RESET);
          } else {
            if (Log::is_enabled(Log::WARN)) {
              char desc[200];
              describe(desc, sizeof(desc));
              Log::warn("%s error reading from peer: %s", desc, ec.message().c_str());
            }
            close(StreamEnd::READ_ERROR);
          }

        } else {
          receive();
          wait();
        }
      }

      release();
    }
  );

  retain();
}

void OutboundUDP::pump() {
  if (!m_socket.is_open()) return;
  if (!m_connected) return;

  while (!m_pending_buffer.empty()) {
    auto evt = m_pending_buffer.shift();

    if (auto data = evt->as<Data>()) {
      m_socket.async_send(
        DataChunks(data->chunks()),
        [=](const std::error_code &ec, std::size_t n) {
          if (ec != asio::error::operation_aborted) {
            m_metric_traffic_out->increase(n);
            s_metric_traffic_out->increase(n);
            if (ec) {
              if (Log::is_enabled(Log::WARN)) {
                char desc[200];
                describe(desc, sizeof(desc));
                Log::warn("%s error writing to peer: %s", desc, ec.message().c_str());
              }
              close(StreamEnd::WRITE_ERROR);
            }
          }

          release();
        }
      );

      retain();
    }

    evt->release();
  }

  wait();
}

void OutboundUDP::wait() {
  if (!m_socket.is_open()) return;
  if (m_options.idle_timeout > 0) {
    m_idle_timer.cancel();
    m_idle_timer.schedule(
      m_options.idle_timeout,
      [this]() {
        InputContext ic;
        close(StreamEnd::IDLE_TIMEOUT);
      }
    );
  }
}

void OutboundUDP::close(StreamEnd::Error err) {
  if (!m_connected) return;

  m_buffer.clear();
  m_pending_buffer.clear();
  m_ended = false;
  m_retries = 0;
  m_connected = false;

  if (m_socket.is_open()) {
    std::error_code ec;
    m_socket.shutdown(udp::socket::shutdown_both, ec);
    m_socket.close(ec);

    if (ec) {
      if (Log::is_enabled(Log::ERROR)) {
        char desc[200];
        describe(desc, sizeof(desc));
        Log::error("%s error closing socket: %s", desc, ec.message().c_str());
      }
    } else {
      if (Log::is_enabled(Log::OUTBOUND)) {
        char desc[200];
        describe(desc, sizeof(desc));
        Log::debug(Log::OUTBOUND, "%s connection closed to peer", desc);
      }
    }
  }

  error(err);
}

auto OutboundUDP::get_buffered() const -> size_t {
  return 0;
}

auto OutboundUDP::get_traffic_in() -> size_t {
  return 0;
}

auto OutboundUDP::get_traffic_out() -> size_t {
  return 0;
}

} // namespace pipy

namespace pjs {

using namespace pipy;

template<> void EnumDef<Outbound::State>::init() {
  define(Outbound::State::idle, "idle");
  define(Outbound::State::resolving, "resolving");
  define(Outbound::State::connecting, "connecting");
  define(Outbound::State::connected, "connected");
  define(Outbound::State::closed, "closed");
}

template<> void ClassDef<Outbound>::init() {
  accessor("state",         [](Object *obj, Value &ret) { ret.set(EnumDef<Outbound::State>::name(obj->as<Outbound>()->state())); });
  accessor("localAddress" , [](Object *obj, Value &ret) { ret.set(obj->as<Outbound>()->local_address()); });
  accessor("localPort"    , [](Object *obj, Value &ret) { ret.set(obj->as<Outbound>()->local_port()); });
  accessor("remoteAddress", [](Object *obj, Value &ret) { ret.set(obj->as<Outbound>()->remote_address()); });
  accessor("remotePort"   , [](Object *obj, Value &ret) { ret.set(obj->as<Outbound>()->remote_port()); });
}

template<> void ClassDef<OutboundTCP>::init() {
  super<Outbound>();
}

template<> void ClassDef<OutboundUDP>::init() {
  super<Outbound>();
}

} // namespace pjs
