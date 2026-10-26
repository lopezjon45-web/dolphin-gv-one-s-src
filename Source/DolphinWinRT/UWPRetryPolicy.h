/* Copyright (c) 2026 - GPL-2.0-or-later, like Dolphin.
 *
 * When a refused file call is made again (UWPFileRedirect.cpp). One S, measured 2026-10-25 with a probe app: the
 * first FromApp call on a removable drive fails once with ERROR_ACCESS_DENIED and every later one succeeds.
 * Everything else refused is refused for good, and a brokered refusal costs about 850 ms, so a retry anywhere
 * else only adds delay: on 2026-10-26 Dolphin's start-up walked the always-refused ancestors of LocalState
 * (U:\, U:\Users, ...) and the retries made it three times slower, until the console gave up on the start.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include "Common/PathCompare.h"

namespace UWPRetry
{
class DrivePolicy
{
public:
   /* A call on this path's drive has returned, whatever the result: the drive's first call is over. */
   void Settle(std::wstring_view path)
   {
      if (const uint32_t bit = Bit(path))
         m_settled.fetch_or(bit);
   }

   /* True once per drive, for the first refused call, and never on the drive of one of the app's own folders
    * (LocalState, the install folder): those are not removable, their refusals are real. With no own
    * folders known yet nothing is retried. */
   bool AllowRetry(std::wstring_view path, const std::wstring *own, size_t ownCount)
   {
      const wchar_t drive = File::DriveOf(path);
      if (!drive || !ownCount)
         return false;
      for (size_t i = 0; i < ownCount; i++)
      {
         if (File::DriveOf(std::wstring_view(own[i])) == drive)
            return false;
      }
      const uint32_t bit = Bit(path);
      return (m_settled.fetch_or(bit) & bit) == 0;
   }

private:
   static uint32_t Bit(std::wstring_view path)
   {
      const wchar_t drive = File::DriveOf(path);
      return drive ? 1u << (drive - L'A') : 0;
   }

   std::atomic<uint32_t> m_settled{0};
};
}
