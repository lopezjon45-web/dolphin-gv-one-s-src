// Stand-alone test (no Dolphin or gtest needed), build and run on any host:
//   clang++ -std=c++20 -I Source/Core -I Source/DolphinWinRT Source/DolphinWinRT/tests/retry_policy_test.cpp -o retry_policy_test && ./retry_policy_test
#include <cstdio>
#include <string>
#include <vector>

#include "Common/PathCompare.h"
#include "UWPRetryPolicy.h"

static int failures = 0;
#define CHECK(x)                                                                                   \
  do                                                                                               \
  {                                                                                                \
    if (!(x))                                                                                      \
    {                                                                                              \
      std::printf("FAIL line %d: %s\n", __LINE__, #x);                                             \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

static const std::string kLocal =
    "U:\\Users\\UserMgr0\\AppData\\Local\\Packages\\GamerDad.DolphinForGameVault_w8c8c10g47ykp\\LocalState";
static const std::wstring kLocalW(kLocal.begin(), kLocal.end());
static const std::wstring kInstallW = L"S:\\Program Files\\WindowsApps\\GamerDad.DolphinForGameVault_1.1.9.130_x64__w8c8c10g47ykp";

static void PathCompare()
{
  CHECK(File::IsSameOrUnder<char>("U:/Users/UserMgr0/AppData/Local/Packages/GamerDad.DolphinForGameVault_w8c8c10g47ykp/LocalState/Cache/", kLocal));
  CHECK(File::IsSameOrUnder<char>("u:\\users\\usermgr0\\appdata\\local\\packages\\gamerdad.dolphinforgamevault_w8c8c10g47ykp\\localstate", kLocal));
  CHECK(!File::IsSameOrUnder<char>(kLocal + "Other\\x", kLocal));
  CHECK(!File::IsSameOrUnder<char>("U:/Users/", kLocal));
  CHECK(!File::IsSameOrUnder<char>("E:/Wii/game.wbfs", kLocal));

  CHECK(File::IsStrictAncestor<char>("U:/", kLocal));
  CHECK(File::IsStrictAncestor<char>("U:/Users/", kLocal));
  CHECK(File::IsStrictAncestor<char>("U:/Users/UserMgr0/AppData/", kLocal));
  CHECK(File::IsStrictAncestor<char>("U:/Users/UserMgr0/AppData/Local/Packages/GamerDad.DolphinForGameVault_w8c8c10g47ykp", kLocal));
  CHECK(!File::IsStrictAncestor<char>(kLocal, kLocal));
  CHECK(!File::IsStrictAncestor<char>(kLocal + "/", kLocal));
  CHECK(!File::IsStrictAncestor<char>(kLocal + "/Cache/", kLocal));
  CHECK(!File::IsStrictAncestor<char>("U:/Users2/", kLocal));
  CHECK(!File::IsStrictAncestor<char>("E:/", kLocal));

  CHECK(File::DriveOf<wchar_t>(L"E:/Wii/x.wbfs") == L'E');
  CHECK(File::DriveOf<wchar_t>(L"e:\\Wii") == L'E');
  CHECK(File::DriveOf<wchar_t>(L"relative\\path") == 0);
  CHECK(File::DriveOf<wchar_t>(L"") == 0);
}

// The sub paths CreateFullPath (FileUtil.cpp) looks at for a missing LocalState\Cache\GameCovers\, in order
static std::vector<std::string> WalkedSubPaths(const std::string &fullPath, const std::string &local)
{
  std::vector<std::string> walked;
  const bool below_local = File::IsSameOrUnder<char>(fullPath, local);
  for (size_t position = 0; (position = fullPath.find('/', position)) != std::string::npos; position++)
  {
    const std::string sub = fullPath.substr(0, position + 1);
    if (below_local && File::IsStrictAncestor<char>(sub, local))
      continue;
    walked.push_back(sub);
  }
  return walked;
}

static void StartupWalk()
{
  const std::string pkg = "U:/Users/UserMgr0/AppData/Local/Packages/GamerDad.DolphinForGameVault_w8c8c10g47ykp";
  const auto walked = WalkedSubPaths(pkg + "/LocalState/Cache/GameCovers/", kLocal);
  CHECK(walked.size() == 3);
  CHECK(walked.size() == 3 && walked[0] == pkg + "/LocalState/");
  CHECK(walked.size() == 3 && walked[2] == pkg + "/LocalState/Cache/GameCovers/");

  const auto usb = WalkedSubPaths("E:/dolphin/Cache/", kLocal);
  CHECK(usb.size() == 3 && usb[0] == "E:/");
}

static void RetryPolicy()
{
  const std::wstring own[] = {kLocalW, kInstallW};

  {
    // the start-up walk of 2026-10-26: every denied ancestor of LocalState, none may be retried
    UWPRetry::DrivePolicy p;
    CHECK(!p.AllowRetry(L"U:/", own, 2));
    CHECK(!p.AllowRetry(L"U:/Users/", own, 2));
    CHECK(!p.AllowRetry(L"U:/Users/UserMgr0/AppData/", own, 2));
    CHECK(!p.AllowRetry(L"S:\\Program Files\\", own, 2));
  }
  {
    // the removable drive: the first denied call is retried once, later denied calls are not
    UWPRetry::DrivePolicy p;
    CHECK(p.AllowRetry(L"E:/Wii/game.wbfs", own, 2));
    CHECK(!p.AllowRetry(L"E:/Wii/game.wbfs", own, 2));
    CHECK(!p.AllowRetry(L"E:/GameCube/other.rvz", own, 2));
    CHECK(p.AllowRetry(L"F:/Wii/game.wbfs", own, 2));
  }
  {
    // a drive whose first call worked is settled: a later denial is a real one
    UWPRetry::DrivePolicy p;
    p.Settle(L"E:/Wii/game.wbfs");
    CHECK(!p.AllowRetry(L"E:/GameCube/locked.rvz", own, 2));
  }
  {
    // the app's own folders are not known yet: no retry at all
    UWPRetry::DrivePolicy p;
    CHECK(!p.AllowRetry(L"E:/Wii/game.wbfs", nullptr, 0));
  }
  {
    // no drive letter (a relative or device path): nothing to key the budget on
    UWPRetry::DrivePolicy p;
    CHECK(!p.AllowRetry(L"relative\\file", own, 2));
  }
}

int main()
{
  PathCompare();
  StartupWalk();
  RetryPolicy();
  std::printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
