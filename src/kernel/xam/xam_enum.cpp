/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstring>

#include <rex/cvar.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/apps/xmp_app.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xtypes.h>

REXCVAR_DECLARE(bool, online_enabled);  // (xam_net.cpp)

#if REX_PLATFORM_WIN32
#include <rex/platform.h>
#endif

#include <fmt/format.h>

#include <algorithm>
#include <cstring>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

// https://github.com/LestaD/SourceEngine2007/blob/master/se2007/engine/xboxsystem.cpp#L518
uint32_t xeXamEnumerate(uint32_t handle, uint32_t flags, mapped_void buffer_ptr,
                        uint32_t buffer_size, uint32_t* items_returned, uint32_t overlapped_ptr) {
  assert_true(flags == 0);

  auto e = REX_KERNEL_OBJECTS()->LookupObject<XEnumerator>(handle);
  if (!e) {
    return X_ERROR_INVALID_HANDLE;
  }

  auto run = [e, buffer_ptr](uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    X_RESULT result;
    uint32_t item_count = 0;
    if (!buffer_ptr) {
      result = X_ERROR_INVALID_PARAMETER;
    } else {
      result = e->WriteItems(buffer_ptr.guest_address(), buffer_ptr.as<uint8_t*>(), &item_count);
    }
    extended_error = X_HRESULT_FROM_WIN32(result);
    length = item_count;
    return result;
  };

  if (items_returned) {
    assert_true(!overlapped_ptr);
    uint32_t extended_error;
    uint32_t item_count;
    X_RESULT result = run(extended_error, item_count);
    *items_returned = result == X_ERROR_SUCCESS ? item_count : 0;
    return result;
  } else if (overlapped_ptr) {
    assert_true(!items_returned);
    REX_KERNEL_STATE()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
    return X_ERROR_IO_PENDING;
  } else {
    assert_always();
    return X_ERROR_INVALID_PARAMETER;
  }
}

u32 XamEnumerate_entry(u32 handle, u32 flags, mapped_void buffer, u32 buffer_length,
                       mapped_u32 items_returned, ppc_ptr_t<XAM_OVERLAPPED> overlapped) {
  uint32_t dummy;
  auto result =
      xeXamEnumerate(handle, flags, buffer, buffer_length,
                     !overlapped.guest_address() ? &dummy : nullptr, overlapped.guest_address());
  if (!overlapped && items_returned) {
    *items_returned = dummy;
  }
  return result;
}

u32 XamCreateEnumeratorHandle_entry(u32 user_index, u32 app_id, u32 open_message,
                                    u32 close_message, u32 extra_size, u32 item_count, u32 flags,
                                    mapped_u32 out_handle) {
  // [svr2011] The music player's user playlists (xmp_app.h); nothing else yet.
  using rex::kernel::xam::apps::XmpApp;
  if (!out_handle) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (app_id == 0xFC && open_message == 0x00058039 && REXCVAR_GET(online_enabled)) {
    // [svr2011] XTitleServerCreateEnumerator: the game's Xbox LIVE title
    // servers (XLSP). Its GameSpy services go through "THQSVR11_GSGW": one
    // server of that name at 127.11.0.1, which connect() sends on to
    // online_server (xam_net.cpp). XTITLE_SERVER_INFO: inaServer, dwFlags,
    // szServerInfo[200].
    constexpr uint32_t kItemSize = 212;
    auto e = make_object<XStaticUntypedEnumerator>(REX_KERNEL_STATE(), 1, kItemSize);
    void* extra = nullptr;
    if (XFAILED(e->Initialize(user_index, app_id, open_message, close_message, flags, extra_size, &extra))) {
      return X_ERROR_FUNCTION_FAILED;
    }
    uint8_t* item = e->AppendItem();
    std::memset(item, 0, kItemSize);
    const uint8_t ina[4] = {127, 11, 0, 1};  // (network order)
    std::memcpy(item, ina, 4);
    std::strcpy(reinterpret_cast<char*>(item + 8), "THQSVR11_GSGW");
    REXKRNL_INFO("XamCreateEnumeratorHandle: title servers: THQSVR11_GSGW");
    *out_handle = e->handle();
    return X_ERROR_SUCCESS;
  }
  if (app_id != 0xFA || open_message != XmpApp::kUserPlaylistEnumerateMessage) {
    // [svr2011] anything else (the online services' lists: friends ...): empty
    REXKRNL_INFO("XamCreateEnumeratorHandle: app {:X} message {:08X}: empty list", uint32_t(app_id),
                 uint32_t(open_message));
    auto e = make_object<XStaticUntypedEnumerator>(REX_KERNEL_STATE(), std::max<uint32_t>(item_count, 1),
                                                   std::max<uint32_t>(extra_size, 4));
    void* extra = nullptr;
    if (XFAILED(e->Initialize(user_index, app_id, open_message, close_message, flags, extra_size, &extra))) {
      return X_ERROR_FUNCTION_FAILED;
    }
    *out_handle = e->handle();
    return X_ERROR_SUCCESS;
  }
  auto e = make_object<XStaticUntypedEnumerator>(REX_KERNEL_STATE(), std::max<uint32_t>(item_count, 1),
                                                 XmpApp::kUserPlaylistSize);
  void* extra = nullptr;
  const auto result =
      e->Initialize(user_index, app_id, open_message, close_message, flags, extra_size, &extra);
  if (XFAILED(result)) {
    return X_ERROR_FUNCTION_FAILED;
  }
  for (const auto& item : XmpApp::UserPlaylistItems()) {
    std::memcpy(e->AppendItem(), item.data(), item.size());
  }
  REXKRNL_INFO("XamCreateEnumeratorHandle: {} user playlist(s)", e->item_count());
  *out_handle = e->handle();
  return X_ERROR_SUCCESS;
}

u32 XamGetPrivateEnumStructureFromHandle_entry(u32 handle, mapped_u32 out_object_ptr) {
  auto e = REX_KERNEL_OBJECTS()->LookupObject<XEnumerator>(handle);
  if (!e) {
    return X_STATUS_INVALID_HANDLE;
  }

  // Caller takes the reference.
  // It's released in ObDereferenceObject.
  e->RetainHandle();

  if (out_object_ptr.guest_address()) {
    *out_object_ptr = e->guest_object();
  }

  return X_STATUS_SUCCESS;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamEnumerate, rex::kernel::xam::XamEnumerate_entry)
REX_EXPORT(__imp__XamCreateEnumeratorHandle, rex::kernel::xam::XamCreateEnumeratorHandle_entry)
REX_EXPORT(__imp__XamGetPrivateEnumStructureFromHandle,
           rex::kernel::xam::XamGetPrivateEnumStructureFromHandle_entry)
