#include <unknwn.h>  // first, before any winrt header: classic COM interfaces then work with as<>()
#include "Host.h"

#include "UWPUtils.h"

#include <windows.h>
#include <iostream>

#include <cctype>
#include <mutex>
#include <thread>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/windows.graphics.display.core.h>

#include <Gamingdeviceinformation.h>

#include "UICommon/GameFile.h"
#include "UICommon/ImGuiMenu/ImGuiFrontend.h"
#include "UICommon/ImGuiMenu/ImGuiNetplay.h"
#include "UICommon/ImGuiMenu/WinRTKeyboard.h"
#include "UICommon/UICommon.h"

#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/Present.h"

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/WindowSystemInfo.h"
#include "Common/Logging/Log.h"

#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/Core.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/Host.h"
#include "Core/HotkeyManager.h"
#include "Core/IOS/STM/STM.h"

#define SDL_MAIN_HANDLED

using namespace winrt;

using namespace Windows;
using namespace Windows::Storage;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Foundation::Numerics;
using namespace Windows::UI::Composition;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Pickers;
using namespace Windows::Graphics::Display::Core;

using winrt::Windows::UI::Core::BackRequestedEventArgs;
using winrt::Windows::UI::Core::CoreProcessEventsOption;
using winrt::Windows::UI::Core::CoreWindow;

std::shared_ptr<NetPlay::NetPlayClient> g_netplay_client = nullptr;
std::shared_ptr<NetPlay::NetPlayServer> g_netplay_server = nullptr;
std::shared_ptr<ImGuiFrontend::ImGuiNetPlay> g_netplay_dialog = nullptr;

namespace UWP
{
Common::Flag m_running{false};
winrt::hstring m_launchOnExit;
Common::Flag g_shutdown_requested{false};
Common::Flag g_tried_graceful_shutdown{false};
Common::Flag g_needs_frontend_reset{false};
// Game Vault: set once OnActivate has run (it boots a game handed over by a link)
Common::Flag g_activated{false};

// Game Vault: the game file shared through the link's disctoken= (Common/IOFile.h). A drive this app may not open
// itself (seen with a player's USB drive: Wii games went straight back to the Home screen) is read through it.
// IStorageItemHandleAccess as in the SDK's WindowsStorageCOM.h, which pulls in desktop-only shell headers.
enum { GV_HAO_READ = 0x120089, GV_HSO_SHARE_READ = 0x1, GV_HO_NONE = 0 };
struct __declspec(uuid("5CA296B2-2C25-4D22-B785-B885C8201E6A")) __declspec(novtable)
GV_IStorageItemHandleAccess : public ::IUnknown
{
  virtual HRESULT STDMETHODCALLTYPE Create(DWORD accessOptions, DWORD sharingOptions, DWORD options,
                                           ::IUnknown* oplockBreakingHandler, HANDLE* interopHandle) = 0;
};
winrt::hstring g_share_token;
std::string g_share_path;
std::mutex g_share_lock;
winrt::Windows::Storage::StorageFile g_share_file{nullptr};  // redeemed once, a handle made per open

static std::string ShareKey(std::string path)
{
  for (char& c : path)
    c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return path;
}

static void* OpenGameVaultShare(const std::string& path)
{
  if (g_share_token.empty() || ShareKey(path) != g_share_path)
    return nullptr;
  std::lock_guard<std::mutex> lock(g_share_lock);
  HANDLE result = nullptr;
  const bool first = !g_share_file;
  // blocking on WinRT calls is not allowed on the app's UI thread: a thread of its own
  std::thread([&] {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try
    {
      if (!g_share_file)
      {
        g_share_file = Windows::ApplicationModel::DataTransfer::SharedStorageAccessManager::
                           RedeemTokenForFileAsync(g_share_token)
                               .get();
      }
      HANDLE h = nullptr;
      if (SUCCEEDED(g_share_file.as<GV_IStorageItemHandleAccess>()->Create(GV_HAO_READ, GV_HSO_SHARE_READ,
                                                                           GV_HO_NONE, nullptr, &h)))
        result = h;
    }
    catch (...)
    {
    }
    winrt::uninit_apartment();
  }).join();
  if (first || !result)
    NOTICE_LOG_FMT(CORE, "Game Vault trace: shared game file {}", result ? "opened" : "not opened");
  return result;
}

struct App : implements<App, IFrameworkViewSource, IFrameworkView>
{
  IFrameworkView CreateView() { return *this; }

  void Initialize(CoreApplicationView const& v)
  {
    v.Activated({this, &App::OnActivate});
    CoreApplication::EnteredBackground({this, &App::EnteredBackground});
    CoreApplication::Suspending({this, &App::Suspending});
  }

  void Load(hstring const&) {}

  void Uninitialize() {}

  void Run()
  {
    // Game Vault: wait for the activation before choosing between the menu and a game. A link's activation often
    // arrives after the first pump; the menu was then already running, and booting the game from OnActivate shut
    // down the video backend the menu still drew with (null g_vertex_manager in VertexManagerBase::Flush).
    for (int waited = 0; !g_activated.IsSet() && waited < 5000; waited += 10)
    {
      CoreWindow::GetForCurrentThread().Dispatcher().ProcessEvents(
          winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);
      if (!g_activated.IsSet())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto navigation = winrt::Windows::UI::Core::SystemNavigationManager::GetForCurrentView();
    navigation.BackRequested(
        [](const winrt::Windows::Foundation::IInspectable&,
           const winrt::Windows::UI::Core::BackRequestedEventArgs& args) { args.Handled(true); });

    Core::DeclareAsHostThread();

    while (true)
    {
      // ImGUI frontend
      if (!m_running.IsSet() || g_needs_frontend_reset.TestAndClear())
      {
        NOTICE_LOG_FMT(CORE, "Game Vault trace: starting the menu (running {})", m_running.IsSet());
        // Ensure we're fully cleaned up before starting new frontend
        if (Core::IsRunning(Core::System::GetInstance()))
        {
          Core::Stop(Core::System::GetInstance());
        }
        Core::Shutdown(Core::System::GetInstance());

        // Reset flags
        g_shutdown_requested.Clear();
        g_tried_graceful_shutdown.Clear();

        // Reinitialize UI systems
        UICommon::Shutdown();
        UICommon::Init();
        UICommon::InitControllers({});

        // Create and run new frontend
        auto frontend = new ImGuiFrontend::ImGuiFrontend();
        auto result = frontend->RunUntilSelection();
        auto game = result.game_result;

        if (game)
        {
          InitializeDolphinFromFile(game->GetFilePath());
        }
        else if (result.netplay)
        {
          InitializeDolphinForNetplay();
        }
        else if (result.boot_params)
        {
          auto& system = Core::System::GetInstance();
          if (Core::IsRunning(system))
            Core::Stop(system);
          Core::Shutdown(system);
          // Prepare window info
          CoreWindow window = CoreWindow::GetForCurrentThread();
          void* abi = winrt::get_abi(window);
          WindowSystemInfo wsi;
          wsi.type = WindowSystemType::Windows;
          wsi.render_surface = abi;
          wsi.render_width = window.Bounds().Width;
          wsi.render_height = window.Bounds().Height;
          // Boot via BootManager
          if (!BootManager::BootCore(system, std::move(result.boot_params), wsi))
          {
            fprintf(stderr, "Could not boot system menu\n");
            Core::Shutdown(system);
          }
          else
          {
            m_running.Set();
          }
        }

        delete frontend;
      }

      g_tried_graceful_shutdown.Clear();

      // Dolphin loop
      while (Core::GetState(Core::System::GetInstance()) != Core::State::Stopping)
      {
        auto& system = Core::System::GetInstance();

        if (g_shutdown_requested.TestAndClear())
        {
          NOTICE_LOG_FMT(CORE, "Game Vault trace: shutdown requested");
          if (NetPlay::IsNetPlayRunning())
            NetPlay::SendPowerButtonEvent();

          Core::Stop(system);
          Core::Shutdown(system);
          m_running.Clear();

          // Only set frontend reset after proper cleanup
          g_needs_frontend_reset.Set();
          break;
        }

        ::Core::HostDispatchJobs(system);

        if (Core::IsRunningOrStarting(Core::System::GetInstance()))
        {
          Core::UpdateInputGate(false);
          HotkeyManagerEmu::GetStatus(false);
          ControlReference::SetInputGate(true);

          HotkeyManagerEmu::GetStatus(true);

          if (HotkeyManagerEmu::IsPressed(HK_TOGGLE_ONSCREEN_MENU, false))
          {
            OSD::ToggleShowSettings();
            Core::SetState(Core::System::GetInstance(),
                           Core::GetState(Core::System::GetInstance()) == Core::State::Paused ?
                               Core::State::Running :
                               Core::State::Paused);
          }

          if (Core::GetState(Core::System::GetInstance()) == Core::State::Paused)
          {
            g_presenter->Present();
          }
        }

        CoreWindow::GetForCurrentThread().Dispatcher().ProcessEvents(
            CoreProcessEventsOption::ProcessAllIfPresent);

        constexpr u32 INTERVAL_MS = 1000 / 30;
        constexpr auto INTERVAL = std::chrono::milliseconds(INTERVAL_MS);
        std::this_thread::sleep_for(INTERVAL);
      }

      // Make sure we've shut down properly.
      NOTICE_LOG_FMT(CORE, "Game Vault trace: game loop ended, state {}",
                     static_cast<int>(Core::GetState(Core::System::GetInstance())));
      m_running.Clear();

      // If there's another frontend, boot to that.
      if (!m_launchOnExit.empty())
      {
        winrt::Windows::Foundation::Uri m_uri{m_launchOnExit};
        auto asyncOperation = winrt::Windows::System::Launcher::LaunchUriAsync(m_uri);
        asyncOperation.Completed([](winrt::Windows::Foundation::IAsyncOperation<bool> const& sender,
                                    winrt::Windows::Foundation::AsyncStatus const asyncStatus) {
          CoreApplication::Exit();
          return;
        });
      }

      // Continue looping to open the frontend again
    }
  }

  void InitializeDolphinForNetplay() { m_running.Set(); }

  winrt::fire_and_forget InitializeDolphinFromFile(std::string path)
  {
    NOTICE_LOG_FMT(CORE, "Game Vault trace: InitializeDolphinFromFile '{}'", path);
    if (path.empty())
    {
      FileOpenPicker openPicker;
      openPicker.ViewMode(PickerViewMode::List);
      openPicker.SuggestedStartLocation(PickerLocationId::HomeGroup);
      openPicker.FileTypeFilter().Append(L".iso");
      openPicker.FileTypeFilter().Append(L".ciso");
      openPicker.FileTypeFilter().Append(L".rvz");
      openPicker.FileTypeFilter().Append(L".wbfs");
      openPicker.FileTypeFilter().Append(L".gcm");
      openPicker.FileTypeFilter().Append(L".gcz");
      openPicker.FileTypeFilter().Append(L".json");
      openPicker.FileTypeFilter().Append(L".elf");
      openPicker.FileTypeFilter().Append(L".dol");
      openPicker.FileTypeFilter().Append(L".tgc");
      openPicker.FileTypeFilter().Append(L".wad");

      auto file = co_await openPicker.PickSingleFileAsync();
      if (file)
      {
        path = winrt::to_string(file.Path().data());
      }
    }

    CoreWindow window = CoreWindow::GetForCurrentThread();
    void* abi = winrt::get_abi(window);

    WindowSystemInfo wsi;
    wsi.type = WindowSystemType::Windows;
    wsi.render_surface = abi;
    wsi.render_width = window.Bounds().Width;
    wsi.render_height = window.Bounds().Height;

    GAMING_DEVICE_MODEL_INFORMATION info = {};
    GetGamingDeviceModelInformation(&info);
    if (info.vendorId == GAMING_DEVICE_VENDOR_ID_MICROSOFT)
    {
      HdmiDisplayInformation hdi = HdmiDisplayInformation::GetForCurrentView();
      if (hdi)
      {
        wsi.render_width = hdi.GetCurrentDisplayMode().ResolutionWidthInRawPixels();
        wsi.render_height = hdi.GetCurrentDisplayMode().ResolutionHeightInRawPixels();
        // Our UI is based on 1080p, and we're adding a modifier to zoom in by 80%
        wsi.render_surface_scale = ((float)wsi.render_width / 1920.0f) * 1.8f;
      }
    }

    auto& system = Core::System::GetInstance();

    // Ensure clean state before booting
    Core::Shutdown(system);

    std::unique_ptr<BootParameters> boot =
        BootParameters::GenerateFromFile(path, BootSessionData("", DeleteSavestateAfterBoot::No));

    if (!BootManager::BootCore(system, std::move(boot), wsi))
    {
      fprintf(stderr, "Could not boot the specified file\n");
      Core::Shutdown(system);
      co_return;
    }

    m_running.Set();
    co_return;
  }

  void OnClosed(const IInspectable&, const winrt::Windows::UI::Core::CoreWindowEventArgs& args)
  {
    g_shutdown_requested.Set();
  }

  void OnActivate(const winrt::Windows::ApplicationModel::Core::CoreApplicationView&,
                  const winrt::Windows::ApplicationModel::Activation::IActivatedEventArgs& args)
  {
    std::stringstream filePath;
    NOTICE_LOG_FMT(CORE, "Game Vault trace: OnActivate kind {} previous {}", static_cast<int>(args.Kind()),
                   static_cast<int>(args.PreviousExecutionState()));

    if (args.Kind() == Windows::ApplicationModel::Activation::ActivationKind::Protocol)
    {
      auto protocolActivatedEventArgs{
          args.as<Windows::ApplicationModel::Activation::ProtocolActivatedEventArgs>()};
      auto query = protocolActivatedEventArgs.Uri().QueryParsed();

      for (uint32_t i = 0; i < query.Size(); i++)
      {
        auto arg = query.GetAt(i);

        // parse command line string
        if (arg.Name() == winrt::hstring(L"cmd"))
        {
          std::string argVal = winrt::to_string(arg.Value());

          // Strip the executable from the cmd argument
          if (argVal.starts_with("dolphin.exe"))
          {
            argVal = argVal.substr(11, argVal.length());
          }

          std::istringstream iss(argVal);
          std::string s;

          // Maintain slashes while reading the quotes
          while (iss >> std::quoted(s, '"', (char)0))
          {
            filePath << s;
          }
        }
        else if (arg.Name() == winrt::hstring(L"launchOnExit"))
        {
          // For if we want to return to a frontend
          m_launchOnExit = arg.Value();
        }
        else if (arg.Name() == winrt::hstring(L"disctoken"))
        {
          g_share_token = arg.Value();
        }
      }
    }

    Core::DeclareAsHostThread();

    UICommon::SetUserDirectory(UWP::GetUserLocation());
    UICommon::CreateDirectories();
    UICommon::Init();
    UICommon::InitControllers({});

    std::string gamePath = filePath.str();
    if (!g_share_token.empty() && !gamePath.empty())
    {
      // test switch LocalState\share-test.txt: boot a path that cannot be opened, so the game is read the way a
      // refused drive's is (only through the shared file)
      if (File::Exists(File::GetUserPath(D_USER_IDX) + "share-test.txt"))
      {
        gamePath = "Z:\\gv-share-test\\" + gamePath.substr(gamePath.find_last_of("\\/") + 1);
        NOTICE_LOG_FMT(CORE, "Game Vault trace: share test, booting {}", gamePath);
      }
      g_share_path = ShareKey(gamePath);
      File::SetSharedFileOpener(OpenGameVaultShare);
    }
    if (!gamePath.empty() && gamePath != "")
    {
      InitializeDolphinFromFile(gamePath);
    }

    CoreWindow window = CoreWindow::GetForCurrentThread();
    window.Activate();
    g_activated.Set();
  }

  void EnteredBackground(const IInspectable&,
                         const winrt::Windows::ApplicationModel::EnteredBackgroundEventArgs& args)
  {
  }

  void Suspending(const IInspectable&,
                  const winrt::Windows::ApplicationModel::SuspendingEventArgs& args)
  {
    auto& system = Core::System::GetInstance();
    NOTICE_LOG_FMT(CORE, "Game Vault trace: Suspending");

    // The Series S/X quits fast, so let's immediately shutdown to ensure all the caches save.
    Core::Stop(system);
    Core::Shutdown(system);
    UICommon::Shutdown();

    if (!m_launchOnExit.empty())
    {
      winrt::Windows::Foundation::Uri m_uri{m_launchOnExit};
      auto asyncOperation = winrt::Windows::System::Launcher::LaunchUriAsync(m_uri);
      asyncOperation.Completed([](winrt::Windows::Foundation::IAsyncOperation<bool> const& sender,
                                  winrt::Windows::Foundation::AsyncStatus const asyncStatus) {
        CoreApplication::Exit();
      });
    }
  }

  void OnCharacterReceived(winrt::Windows::UI::Core::CoreWindow const& /* sender */,
                           winrt::Windows::UI::Core::CharacterReceivedEventArgs const& e /* args */)
  {
    UWP::HandleCharacter(e.KeyCode());
  }

  void SetWindow(CoreWindow const& w)
  {
    w.Closed({this, &App::OnClosed});
    w.CharacterReceived({this, &App::OnCharacterReceived});
  }
};
}  // namespace UWP

int WINAPIV WinMain()
{
  winrt::init_apartment();

  CoreApplication::Run(make<UWP::App>());

  winrt::uninit_apartment();

  return 0;
}

std::vector<std::string> Host_GetPreferredLocales()
{
  return {};
}

void Host_JitCacheInvalidation()
{
}

void Host_JitProfileDataWiped()
{
}

void Host_NotifyMapLoaded()
{
}

void Host_PPCSymbolsChanged()
{
}

void Host_PPCBreakpointsChanged()
{
}

void Host_RefreshDSPDebuggerWindow()
{
}

bool Host_UIBlocksControllerState()
{
  return false;
}

void Host_Message(HostMessageID id)
{
}

void Host_UpdateTitle(const std::string& title)
{
}

void Host_UpdateDisasmDialog()
{
}

void Host_UpdateMainFrame()
{
}

void Host_RequestRenderWindowSize(int width, int height)
{
}

bool Host_RendererHasFocus()
{
  return true;
}

bool Host_RendererHasFullFocus()
{
  // Mouse capturing isn't implemented
  return Host_RendererHasFocus();
}

bool Host_RendererIsFullscreen()
{
  return true;
}

void Host_YieldToUI()
{
}

bool Host_TASInputHasFocus()
{
  return false;
}

void Host_TitleChanged()
{
}

void Host_UpdateDiscordClientID(const std::string& client_id)
{
}

bool Host_UpdateDiscordPresenceRaw(const std::string& details, const std::string& state,
                                   const std::string& large_image_key,
                                   const std::string& large_image_text,
                                   const std::string& small_image_key,
                                   const std::string& small_image_text,
                                   const int64_t start_timestamp, const int64_t end_timestamp,
                                   const int party_size, const int party_max)
{
  return false;
}

std::unique_ptr<GBAHostInterface> Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core> core)
{
  return nullptr;
}
