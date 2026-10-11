/**
 * [svr2011] Title-side online play: the hooks a title's host program sets to
 * give the game peer-to-peer networking (system link / player matches) of
 * its own - addresses, QoS probes, session messages - instead of the SDK's
 * offline stubs. Members left empty keep the SDK's behaviour.
 */
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace rex::kernel::xam {

struct OnlineHooks {
  // XNetGetTitleXnAddr: fills the 36-byte XNADDR (guest layout: in_addr ina,
  // inaOnline, be16 wPortOnline, abEnet[6], abOnline[20]); returns its
  // XNET_GET_XNADDR_* status.
  std::function<uint32_t(uint8_t* xnaddr)> title_xnaddr;
  // XNetXnAddrToInAddr: the in_addr (host byte order) the game uses for the
  // peer at xnaddr in its socket calls; 0: unknown.
  std::function<uint32_t(const uint8_t* xnaddr)> xnaddr_to_inaddr;
  // XNetInAddrToXnAddr / the peer behind an in_addr: false if unknown.
  std::function<bool(uint32_t in_addr, uint8_t* xnaddr)> inaddr_to_xnaddr;

  // XNetQosListen(xnkid, data, size, flags): what a host answers QoS probes
  // for session xnkid with (the flags: XNET_QOS_LISTEN_*).
  std::function<uint32_t(const uint8_t* xnkid, const uint8_t* data, uint32_t size, uint32_t flags)>
      qos_listen;
  struct QosTarget {
    std::array<uint8_t, 36> xnaddr;
    std::array<uint8_t, 8> xnkid;
  };
  struct QosResult {
    bool contacted = false;
    uint16_t rtt_ms = 0;
    std::vector<uint8_t> data;  // the host's QoS data
  };
  // XNetQosLookup: probes each target (called on a thread of its own; may
  // take a while).
  std::function<std::vector<QosResult>(const std::vector<QosTarget>& targets)> qos_lookup;

  // Socket addresses (host byte order). to_host: a destination the game
  // gives (sendto, connect) to the real one; to_guest: a real source
  // (recvfrom, accept) to what the game knows it as; bind_port: the real
  // port for a port the game binds.
  std::function<void(uint32_t& addr, uint16_t& port)> to_host;
  std::function<void(uint32_t& addr, uint16_t& port)> to_guest;
  std::function<uint16_t(uint16_t port)> bind_port;
  std::function<uint16_t(uint16_t port)> guest_port;  // (bind_port's inverse: getsockname)

  // XGI session messages (app 0xFB: B0006 context ... B0026): the result, or
  // nullopt for the SDK's own handling. session objects are the guest
  // pointers XamSessionRefObjByHandle gives (one per session).
  std::function<std::optional<uint32_t>(uint32_t message, uint32_t buffer, uint32_t length)> xgi;
};

// Sets the hooks (once, at startup, before the game's threads run).
void SetOnlineHooks(OnlineHooks hooks);
const OnlineHooks& online_hooks();

}  // namespace rex::kernel::xam
