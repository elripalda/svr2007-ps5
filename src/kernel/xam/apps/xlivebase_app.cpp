/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/kernel/xam/apps/xlivebase_app.h>

#include <cstring>

#include <rex/logging.h>
#include <rex/thread.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

XLiveBaseApp::XLiveBaseApp(KernelState* kernel_state) : App(kernel_state, 0xFC) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XLiveBaseApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                            uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x00058004: {
      // Called on startup, seems to just return a bool in the buffer.
      assert_true(!buffer_length || buffer_length == 4);
      REXKRNL_DEBUG("XLiveBaseGetLogonId({:08X})", buffer_ptr);
      memory::store_and_swap<uint32_t>(buffer + 0, 1);  // ?
      return X_E_SUCCESS;
    }
    case 0x00058006: {
      assert_true(!buffer_length || buffer_length == 4);
      REXKRNL_DEBUG("XLiveBaseGetNatType({:08X})", buffer_ptr);
      memory::store_and_swap<uint32_t>(buffer + 0, 1);  // XONLINE_NAT_OPEN
      return X_E_SUCCESS;
    }
    case 0x00058007: {
      // Occurs if title calls XOnlineGetServiceInfo, expects dwServiceId
      // and pServiceInfo. pServiceInfo should contain pointer to
      // XONLINE_SERVICE_INFO structure.
      REXKRNL_DEBUG("CXLiveLogon::GetServiceInfo({:08X}, {:08X})", buffer_ptr, buffer_length);
      return 0x80151802;  // ERROR_CONNECTION_INVALID
    }
    case 0x00058020: {
      // 0x00058004 is called right before this.
      // We should create a XamEnumerate-able empty list here, but I'm not
      // sure of the format.
      // buffer_length seems to be the same ptr sent to 0x00058004.
      REXKRNL_DEBUG("CXLiveFriends::Enumerate({:08X}, {:08X}) unimplemented", buffer_ptr,
                    buffer_length);
      return X_E_FAIL;
    }
    case 0x00058023: {
      REXKRNL_DEBUG(
          "CXLiveMessaging::XMessageGameInviteGetAcceptedInfo({:08X}, {:08X}) "
          "unimplemented",
          buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x00058046: {
      // Required to be successful for 4D530910 to detect signed-in profile
      // Doesn't seem to set anything in the given buffer, probably only takes
      // input
      REXKRNL_DEBUG("XLiveBaseUnk58046({:08X}, {:08X}) unimplemented", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058037: {
      REXKRNL_DEBUG("XPresenceInitialize({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    // [svr2011] Signed in online (online_enabled): the online services the
    // game asks about in its ONLINE menu.
    case 0x0005000C: {
      // XStringVerify (the words players type, e.g. an upload's title):
      // every string passes. The message is an XLIVEBASE_ASYNC_MESSAGE whose
      // first word is the XLIVE_ASYNC_TASK: marshalled request at +0x18
      // (size +0x1C; little-endian: title id, flags, u16 locale size, u16
      // string count, ...), results at +0x2C (size +0x30):
      // STRING_VERIFY_RESPONSE {u16 count; u32 results_ptr} + the HRESULTs.
      const uint32_t task = memory::load_and_swap<uint32_t>(buffer);
      if (!task) return X_E_INVALIDARG;
      auto t = memory_->TranslateVirtual<uint8_t*>(task);
      const uint32_t request = memory::load_and_swap<uint32_t>(t + 0x18);
      const uint32_t results = memory::load_and_swap<uint32_t>(t + 0x2C);
      const uint32_t results_size = memory::load_and_swap<uint32_t>(t + 0x30);
      if (!request || !results) return X_E_INVALIDARG;
      auto r = memory_->TranslateVirtual<uint8_t*>(request);
      const uint16_t count = uint16_t(r[10] | (r[11] << 8));
      auto out = memory_->TranslateVirtual<uint8_t*>(results);
      std::memset(out, 0, results_size);
      if (6 + 4u * count > results_size) return X_E_INVALIDARG;
      memory::store_and_swap<uint16_t>(out, count);
      memory::store_and_swap<uint32_t>(out + 2, results + 6);  // (HRESULTs: 0, all fine)
      REXKRNL_INFO("[svr2011] XStringVerify: {} string(s) accepted", count);
      return X_E_SUCCESS;
    }
    case 0x00058003: {
      REXKRNL_DEBUG("XLiveBaseLogonGetHR");
      return X_E_SUCCESS;  // (logged on)
    }
    case 0x00058009: {
      // XContentGetMarketplaceCounts {user_index, categories, results_ptr}:
      // no offers (X_OFFERING_CONTENTAVAILABLE_RESULT: new, total).
      const uint32_t results_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      if (results_ptr) {
        std::memset(memory_->TranslateVirtual(results_ptr), 0, 8);
      }
      REXKRNL_DEBUG("XContentGetMarketplaceCounts({:08X})", buffer_ptr);
      return X_E_SUCCESS;
    }
    case 0x0005800E: {
      // XUserMuteListQuery {user_index, xuid}, the bool result at buffer_length
      if (buffer_length >= 0x10000) {
        memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(buffer_length), 0);
      }
      REXKRNL_DEBUG("XUserMuteListQuery({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x0005801E:
    case 0x00058044: {
      REXKRNL_DEBUG("XPresence(Un)Subscribe {:08X}({:08X}, {:08X})", message, buffer_ptr,
                    buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058032: {
      // XOnlineGetTaskProgress {overlapped, percent_ptr, numerator_ptr, denominator_ptr}: done
      const uint32_t percent_ptr = memory::load_and_swap<uint32_t>(buffer + 4);
      const uint32_t numerator_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      const uint32_t denominator_ptr = memory::load_and_swap<uint32_t>(buffer + 12);
      if (percent_ptr) memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(percent_ptr), 100);
      if (numerator_ptr) memory::store_and_swap<uint64_t>(memory_->TranslateVirtual(numerator_ptr), 100);
      if (denominator_ptr) memory::store_and_swap<uint64_t>(memory_->TranslateVirtual(denominator_ptr), 100);
      REXKRNL_DEBUG("XOnlineGetTaskProgress({:08X})", buffer_ptr);
      return X_E_SUCCESS;
    }
  }
  REXKRNL_ERROR(
      "Unimplemented XLIVEBASE message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
