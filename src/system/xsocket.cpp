/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstring>
#include <utility>

#include <rex/kernel/xam/module.h>
#include <rex/platform.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xsocket.h>
// #include <rex/system/xnet.h>

#include <rex/net/socket.h>

// Standard socket types used by Xbox API emulation
#if REX_PLATFORM_WIN32
#include <WinSock2.h>

#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#endif

namespace rex::system {

XSocket::XSocket(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XSocket::XSocket(KernelState* kernel_state, uint64_t native_handle)
    : XObject(kernel_state, kObjectType), native_handle_(native_handle) {}

XSocket::~XSocket() {
  Close();
}

X_STATUS XSocket::Initialize(AddressFamily af, Type type, Protocol proto) {
  af_ = af;
  type_ = type;
  proto_ = proto;

  if (proto == Protocol::X_IPPROTO_VDP) {
    // VDP is a layer on top of UDP.
    proto = Protocol::X_IPPROTO_UDP;
  }

  native_handle_ = socket(af, type, proto);
  if (native_handle_ == -1) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Close() {
  int ret = rex::net::socket_close(native_handle_);
  if (ret != 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::SetOption(uint32_t level, uint32_t optname, void* optval_ptr, uint32_t optlen) {
  if (level == 0xFFFF && (optname == 0x5801 || optname == 0x5802)) {
    // Disable socket encryption
    secure_ = false;
    return X_STATUS_SUCCESS;
  }

  // [svr2011] The guest's values are big-endian (4-byte ints; SO_LINGER two
  // shorts) and its constants are Windows' (level 0xFFFF = SOL_SOCKET ...).
  uint8_t value[16] = {};
  const uint8_t* src = static_cast<const uint8_t*>(optval_ptr);
  const uint32_t len = std::min<uint32_t>(optlen, sizeof(value));
  if (src) std::memcpy(value, src, len);
  if (level == 0xFFFF && optname == 0x0080 && len >= 4) {  // SO_LINGER {u16 onoff, u16 linger}
    std::swap(value[0], value[1]);
    std::swap(value[2], value[3]);
  } else if (len == 4) {
    std::swap(value[0], value[3]);
    std::swap(value[1], value[2]);
  }
  int native_level = int(level), native_name = int(optname);
  const char* native_value = reinterpret_cast<const char*>(value);
  int native_len = int(len);
#if !REX_PLATFORM_WIN32
  struct timeval tv = {};
  if (level == 0xFFFF) {
    native_level = SOL_SOCKET;
    switch (optname) {
      case 0x0004: native_name = SO_REUSEADDR; break;
      case 0x0008: native_name = SO_KEEPALIVE; break;
      case 0x0020: native_name = SO_BROADCAST; break;
      case 0x0080: {
        // struct linger: two ints here
        static thread_local struct linger l;
        l.l_onoff = value[0] | (value[1] << 8);
        l.l_linger = value[2] | (value[3] << 8);
        native_name = SO_LINGER;
        native_value = reinterpret_cast<const char*>(&l);
        native_len = sizeof(l);
        break;
      }
      case 0x1001: native_name = SO_SNDBUF; break;
      case 0x1002: native_name = SO_RCVBUF; break;
      case 0x1005:
      case 0x1006: {  // SO_SNDTIMEO / SO_RCVTIMEO: milliseconds -> timeval
        uint32_t ms;
        std::memcpy(&ms, value, 4);
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        native_name = optname == 0x1005 ? SO_SNDTIMEO : SO_RCVTIMEO;
        native_value = reinterpret_cast<const char*>(&tv);
        native_len = sizeof(tv);
        break;
      }
      default:
        // (not something a POSIX socket has: accept it)
        return X_STATUS_SUCCESS;
    }
  }
#endif

  int ret = setsockopt(native_handle_, native_level, native_name, native_value, native_len);
  if (ret < 0) {
    // TODO: WSAGetLastError()
    return X_STATUS_UNSUCCESSFUL;
  }

  // SO_BROADCAST
  if (level == 0xFFFF && optname == 0x0020) {
    broadcast_socket_ = true;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::IOControl(uint32_t cmd, uint8_t* arg_ptr) {
  // [svr2011] The guest's argument is a big-endian u32 and its commands
  // Windows' (FIONBIO 0x8004667E, FIONREAD 0x4004667F).
  constexpr uint32_t kFionbio = 0x8004667E, kFionread = 0x4004667F;
  if (cmd == kFionbio || cmd == kFionread) {
    uint32_t value = 0;
    if (cmd == kFionbio && arg_ptr) {
      value = (uint32_t(arg_ptr[0]) << 24) | (uint32_t(arg_ptr[1]) << 16) | (uint32_t(arg_ptr[2]) << 8) | arg_ptr[3];
    }
#if REX_PLATFORM_WIN32
    const uint32_t native_cmd = cmd;
#else
    const uint32_t native_cmd = cmd == kFionbio ? uint32_t(FIONBIO) : uint32_t(FIONREAD);
#endif
    int native_value = int(value);
    int ret = rex::net::socket_ioctl(native_handle_, native_cmd, reinterpret_cast<uint8_t*>(&native_value));
    if (ret < 0) {
      return X_STATUS_UNSUCCESSFUL;
    }
    if (cmd == kFionread && arg_ptr) {
      const uint32_t v = uint32_t(native_value);
      arg_ptr[0] = uint8_t(v >> 24);
      arg_ptr[1] = uint8_t(v >> 16);
      arg_ptr[2] = uint8_t(v >> 8);
      arg_ptr[3] = uint8_t(v);
    }
    return X_STATUS_SUCCESS;
  }
  int ret = rex::net::socket_ioctl(native_handle_, cmd, arg_ptr);
  if (ret < 0) {
    // TODO: Get last error
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Connect(N_XSOCKADDR* name, int name_len) {
  int ret = connect(native_handle_, (sockaddr*)name, name_len);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Bind(N_XSOCKADDR_IN* name, int name_len) {
  int ret = bind(native_handle_, (sockaddr*)name, name_len);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  bound_ = true;
  bound_port_ = name->sin_port;

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Listen(int backlog) {
  int ret = listen(native_handle_, backlog);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

object_ref<XSocket> XSocket::Accept(N_XSOCKADDR* name, int* name_len) {
  sockaddr n_sockaddr;
  socklen_t n_name_len = sizeof(sockaddr);
  uintptr_t ret = accept(native_handle_, &n_sockaddr, &n_name_len);
  if (ret == -1) {
    std::memset(name, 0, *name_len);
    *name_len = 0;
    return nullptr;
  }

  std::memcpy(name, &n_sockaddr, n_name_len);
  *name_len = n_name_len;

  // Create a kernel object to represent the new socket, and copy parameters
  // over.
  auto socket = object_ref<XSocket>(new XSocket(kernel_state_, ret));
  socket->af_ = af_;
  socket->type_ = type_;
  socket->proto_ = proto_;

  return socket;
}

int XSocket::Shutdown(int how) {
  return shutdown(native_handle_, how);
}

int XSocket::Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  return recv(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags);
}

int XSocket::RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* from,
                      uint32_t* from_len) {
  // Pop from secure packets first
  // TODO(DrChat): Enable when I commit XNet
  /*
  {
    std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
    if (incoming_packets_.size()) {
      packet* pkt = (packet*)incoming_packets_.front();
      int data_len = pkt->data_len;
      std::memcpy(buf, pkt->data, std::min((uint32_t)pkt->data_len, buf_len));

      from->sin_family = 2;
      from->sin_addr = pkt->src_ip;
      from->sin_port = pkt->src_port;

      incoming_packets_.pop();
      uint8_t* pkt_ui8 = (uint8_t*)pkt;
      delete[] pkt_ui8;

      return data_len;
    }
  }
  */

  sockaddr_in nfrom;
  socklen_t nfromlen = sizeof(sockaddr_in);
  int ret = recvfrom(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags,
                     (sockaddr*)&nfrom, &nfromlen);
  if (from) {
    from->sin_family = nfrom.sin_family;
    from->sin_addr = ntohl(nfrom.sin_addr.s_addr);  // BE <- BE
    from->sin_port = ntohs(nfrom.sin_port);           // [svr2011] (the same: it was byte-swapped)
    std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
  }

  if (from_len) {
    *from_len = nfromlen;
  }

  return ret;
}

int XSocket::Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  return send(native_handle_, reinterpret_cast<const char*>(buf), buf_len, flags);
}

int XSocket::SendTo(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* to,
                    uint32_t to_len) {
  // Send 2 copies of the packet: One to XNet (for network security) and an
  // unencrypted copy for other Xenia hosts.
  // TODO(DrChat): Enable when I commit XNet.
  /*
  auto xam = kernel_state()->GetKernelModule<xam::XamModule>("xam.xex");
  auto xnet = xam->xnet();
  if (xnet) {
    xnet->SendPacket(this, to, buf, buf_len);
  }
  */

  // [svr2011] to's address and port are values (be<> fields): network order
  // here (they were stored as they were: packets went to byte-swapped
  // addresses and ports)
  sockaddr_in nto = {};
  if (to) {
    nto.sin_addr.s_addr = htonl(uint32_t(to->sin_addr));
    nto.sin_family = to->sin_family;
    nto.sin_port = htons(uint16_t(to->sin_port));
  }

  return sendto(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags,
                to ? (sockaddr*)&nto : nullptr, to_len);
}

bool XSocket::QueuePacket(uint32_t src_ip, uint16_t src_port, const uint8_t* buf, size_t len) {
  packet* pkt = reinterpret_cast<packet*>(new uint8_t[sizeof(packet) + len]);
  pkt->src_ip = src_ip;
  pkt->src_port = src_port;

  pkt->data_len = (uint16_t)len;
  std::memcpy(pkt->data, buf, len);

  std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
  incoming_packets_.push((uint8_t*)pkt);

  // TODO: Limit on number of incoming packets?
  return true;
}

}  // namespace rex::system
