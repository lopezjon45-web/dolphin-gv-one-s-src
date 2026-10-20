// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Standard Library
#include <atomic>
#include <chrono>
#include <future>
#include <thread>

// Third-party
#include <fmt/format.h>
#include <imgui.h>
#include <imgui_internal.h>

// AudioCommon
#include "AudioCommon/AudioCommon.h"
#include "AudioCommon/WASAPIStream.h"

// Common
#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/FileSearch.h"
#include "Common/IniFile.h"
#include "Common/StringUtil.h"

// Core
#include "Core/AchievementManager.h"
#include "Core/Boot/Boot.h"
#include "Core/CommonTitles.h"
#include "Core/Config/AchievementSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/Config/UISettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/Extension/Extension.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/TitleDatabase.h"
#include "Core/WiiUtils.h"

// DiscIO
#include "DiscIO/NANDImporter.h"

// InputCommon
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/DualShockUDPClient/DualShockUDPClient.h"
#include "InputCommon/InputConfig.h"

// UICommon
#include "UICommon/UICommon.h"

// VideoCommon
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"

// Local UI files
#include "ImGuiFrontend.h"
#include "ImGuiMappingWindow.h"
#include "WinRTKeyboard.h"

// DolphinWinRT
#include "DolphinWinRT/UWPUtils.h"

namespace ImGuiFrontend
{
constexpr const char* PROFILES_DIR = "Profiles/";
extern float m_frame_scale;
extern std::vector<std::string> m_wiimote_profiles;
extern std::string m_selected_wiimote_profile[4];

static std::vector<std::string> m_gc_profiles;
static std::array<std::string, 4> m_selected_gc_profile = {"None", "None", "None", "None"};

static void EnsureGCProfilesLoaded()
{
  if (!m_gc_profiles.empty())
    return;

  const std::string profiles_path =
      File::GetUserPath(D_CONFIG_IDX) + std::string(PROFILES_DIR) + Pad::GetConfig()->GetProfileDirectoryName();
  for (const auto& filename : Common::DoFileSearch({profiles_path}, {".ini"}))
  {
    std::string basename;
    SplitPath(filename, nullptr, &basename, nullptr);
    if (!basename.empty())
      m_gc_profiles.emplace_back(basename);
  }

  m_gc_profiles.emplace_back("None");
  m_gc_profiles.emplace_back("Default");
}
void CreateGeneralTab(UIState* state)
{
  const Core::CPUThreadGuard guard(Core::System::GetInstance());

  bool dualCore = Config::Get(Config::MAIN_CPU_THREAD);
  if (ImGui::Checkbox("Dual Core", &dualCore))
  {
    Config::SetBaseOrCurrent(Config::MAIN_CPU_THREAD, dualCore);
    Config::Save();
  }

  bool SyncGPU = Config::Get(Config::MAIN_SYNC_GPU);
  if (ImGui::Checkbox("Synchronize GPU Thread", &SyncGPU))
  {
    Config::SetBaseOrCurrent(Config::MAIN_SYNC_GPU, SyncGPU);
    Config::Save();
  }
  
  bool cheats = Config::Get(Config::MAIN_ENABLE_CHEATS);
  if (ImGui::Checkbox("Enable Cheats", &cheats))
  {
    Config::SetBaseOrCurrent(Config::MAIN_ENABLE_CHEATS, cheats);
    Config::Save();
  }

  bool mismatchedRegion = Config::Get(Config::MAIN_OVERRIDE_REGION_SETTINGS);
  if (ImGui::Checkbox("Allow Mismatched Region Settings", &mismatchedRegion))
  {
    Config::SetBaseOrCurrent(Config::MAIN_OVERRIDE_REGION_SETTINGS, mismatchedRegion);
    Config::Save();
  }

  bool changeDiscs = Config::Get(Config::MAIN_AUTO_DISC_CHANGE);
  if (ImGui::Checkbox("Change Discs Automatically", &changeDiscs))
  {
    Config::SetBaseOrCurrent(Config::MAIN_AUTO_DISC_CHANGE, changeDiscs);
    Config::Save();
  }

  static const char* speed_limit_items[] = {
    "Unlimited", 
    "10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%", "90%",
    "100% (Normal Speed)",
    "110%", "120%", "130%", "140%", "150%", "160%", "170%", "180%", "190%", "200%"
  };

  float current_speed = Config::Get(Config::MAIN_EMULATION_SPEED);
  static int current_speed_index = current_speed <= 0.0f ? 0 : static_cast<int>(current_speed * 10);
  
  if (ImGui::Combo("Speed Limit", &current_speed_index, speed_limit_items,
                   IM_ARRAYSIZE(speed_limit_items)))
  {
    float new_speed = current_speed_index <= 0 ? 0.0f : current_speed_index * 0.1f;
    Config::SetBaseOrCurrent(Config::MAIN_EMULATION_SPEED, new_speed);
    Config::Save();
  }

  const auto fallback = Config::Get(Config::MAIN_FALLBACK_REGION);
  if (ImGui::CollapsingHeader("Fallback Region"))
  {
    if (ImGui::RadioButton("NTSC-J", fallback == DiscIO::Region::NTSC_J))
    {
      Config::SetBaseOrCurrent(Config::MAIN_FALLBACK_REGION, DiscIO::Region::NTSC_J);
      Config::Save();
    }

    if (ImGui::RadioButton("NTSC-U", fallback == DiscIO::Region::NTSC_U))
    {
      Config::SetBaseOrCurrent(Config::MAIN_FALLBACK_REGION, DiscIO::Region::NTSC_U);
      Config::Save();
    }

    if (ImGui::RadioButton("PAL", fallback == DiscIO::Region::PAL))
    {
      Config::SetBaseOrCurrent(Config::MAIN_FALLBACK_REGION, DiscIO::Region::PAL);
      Config::Save();
    }

    if (ImGui::RadioButton("Unknown", fallback == DiscIO::Region::Unknown))
    {
      Config::SetBaseOrCurrent(Config::MAIN_FALLBACK_REGION, DiscIO::Region::Unknown);
      Config::Save();
    }

    if (ImGui::RadioButton("NTSC-K", fallback == DiscIO::Region::NTSC_K))
    {
      Config::SetBaseOrCurrent(Config::MAIN_FALLBACK_REGION, DiscIO::Region::NTSC_K);
      Config::Save();
    }
  }
}

void CreateInterfaceTab(UIState* state)
{
  bool showOSD = Config::Get(Config::MAIN_OSD_MESSAGES);
  if (ImGui::Checkbox("Show On-Screen Messages", &showOSD))
  {
    Config::SetBaseOrCurrent(Config::MAIN_OSD_MESSAGES, showOSD);
    Config::Save();
  }

  ImGui::Spacing();

  if (ImGui::BeginCombo("Theme", m_selected_theme->GetName().c_str()))
  {
    for (auto& theme : m_themes)
    {
      if (ImGui::Selectable(theme.first.c_str(),
                            m_selected_theme->GetName() == theme.second.GetName()))
      {
        m_selected_theme = &theme.second;
        Config::SetBaseOrCurrent(Config::FRONTEND_SELECTED_THEME, theme.first);
      }
    }

    ImGui::EndCombo();
  }

  ImGui::Spacing();

    static int current_theme = Config::Get(Config::FRONTEND_COLOR_THEME);
    const char* theme_names[] = {
      "Dark Theme", "Light Theme", "Classic Theme", "Ocean Blue", "Purple Night",
      "Forest Green", "Cherry Red", "Amber Gold", "Steel Blue", "Sunset Orange",
      "Cyber Pink", "Mint Fresh"
    };

    if (ImGui::Combo("Color Theme", &current_theme, theme_names, IM_ARRAYSIZE(theme_names)))
    {
      ApplyColorTheme(current_theme);

      ApplyModernStyling();

      Config::SetBaseOrCurrent(Config::FRONTEND_COLOR_THEME, current_theme);
      Config::Save();
    }
    if (ImGui::IsItemHovered())
    {
      ImGui::SetTooltip("Sets the style of Dolphin's user interface.\n"
                       "If unsure, select Dark Theme.");
    }

  bool useTitleDatabase = Config::Get(Config::MAIN_USE_BUILT_IN_TITLE_DATABASE);
  if (ImGui::Checkbox("Use Built-In Database of Game Names", &useTitleDatabase))
  {
    Config::SetBaseOrCurrent(Config::MAIN_USE_BUILT_IN_TITLE_DATABASE, useTitleDatabase);
    Config::Save();
  }
  ImGui::TextWrapped("Use Dolphin's built-in database of game names instead of reading from "
                     "the game files.\n\nIf unsure, leave this checked.");

  bool disableScreensaver = Config::Get(Config::MAIN_DISABLE_SCREENSAVER);
  if (ImGui::Checkbox("Disable Screensaver", &disableScreensaver))
  {
    Config::SetBaseOrCurrent(Config::MAIN_DISABLE_SCREENSAVER, disableScreensaver);
    Config::Save();
  }
  ImGui::TextWrapped("Disable the system screensaver while Dolphin is running.\n\n"
                     "If unsure, leave this checked.");

  bool focusedHotkeys = Config::Get(Config::MAIN_FOCUSED_HOTKEYS);
  if (ImGui::Checkbox("Hotkeys Require Window Focus", &focusedHotkeys))
  {
    Config::SetBaseOrCurrent(Config::MAIN_FOCUSED_HOTKEYS, focusedHotkeys);
    Config::Save();
  }
  ImGui::TextWrapped("When checked, hotkeys will only work when Dolphin has focus.\n\n"
                     "If unsure, leave this checked.");

  bool confirmOnStop = Config::Get(Config::MAIN_CONFIRM_ON_STOP);
  if (ImGui::Checkbox("Confirm on Stop", &confirmOnStop))
  {
    Config::SetBaseOrCurrent(Config::MAIN_CONFIRM_ON_STOP, confirmOnStop);
    Config::Save();
  }
  ImGui::TextWrapped("Show a confirmation dialog when stopping emulation.\n\n"
                     "If unsure, leave this checked.");

  bool timeTracking = Config::Get(Config::MAIN_TIME_TRACKING);
  if (ImGui::Checkbox("Enable Play Time Tracking", &timeTracking))
  {
    Config::SetBaseOrCurrent(Config::MAIN_TIME_TRACKING, timeTracking);
    Config::Save();
  }
  ImGui::TextWrapped("Track play time for games.\n\n"
                     "If unsure, leave this checked.");

  if (ImGui::CollapsingHeader("Advanced Interface Options"))
  {
    bool usePanicHandlers = Config::Get(Config::MAIN_USE_PANIC_HANDLERS);
    if (ImGui::Checkbox("Use Panic Handlers", &usePanicHandlers))
    {
      Config::SetBaseOrCurrent(Config::MAIN_USE_PANIC_HANDLERS, usePanicHandlers);
      Config::Save();
    }
    ImGui::TextWrapped("Handle panic events (crashes) internally instead of letting the OS handle them. "
                       "Useful for debugging.\n\nIf unsure, leave this checked.");

    bool keepWindowOnTop = Config::Get(Config::MAIN_KEEP_WINDOW_ON_TOP);
    if (ImGui::Checkbox("Keep Window on Top", &keepWindowOnTop))
    {
      Config::SetBaseOrCurrent(Config::MAIN_KEEP_WINDOW_ON_TOP, keepWindowOnTop);
      Config::Save();
    }
    ImGui::TextWrapped("Keep the Dolphin window on top of other windows.\n\n"
                       "If unsure, leave this unchecked.");
  }
}

void CreateGraphicsTab(UIState* state)
{
  const Core::CPUThreadGuard guard(Core::System::GetInstance());
  if (ImGui::CollapsingHeader("General"))
  {
    // Add backend selection
    std::vector<std::string> backend_names;
    for (auto& backend : VideoBackendBase::GetAvailableBackends())
    {
      backend_names.push_back(backend->GetName());
    }

    std::string current_backend = Config::Get(Config::MAIN_GFX_BACKEND);
    int current_backend_idx = 0;
    for (size_t i = 0; i < backend_names.size(); ++i)
    {
      if (backend_names[i] == current_backend)
      {
        current_backend_idx = static_cast<int>(i);
        break;
      }
    }

    if (ImGui::Combo(
            "Backend", &current_backend_idx,
            [](void* data, int idx, const char** out_text) {
              auto* backends = static_cast<std::vector<std::string>*>(data);
              *out_text = backends->at(idx).c_str();
              return true;
            },
            &backend_names, static_cast<int>(backend_names.size())))
    {
      Config::SetBaseOrCurrent(Config::MAIN_GFX_BACKEND, backend_names[current_backend_idx]);
      Config::Save();
    }

    /* For now we don't need it but you never know so I'll just keep it but commented out for now.
    if (!g_backend_info.Adapters.empty())
    {
      int current_adapter = Config::Get(Config::GFX_ADAPTER);
      if (ImGui::Combo("Adapter", &current_adapter, [](void* data, int idx, const char** out_text) {
        auto* adapters = static_cast<std::vector<std::string>*>(data);
        *out_text = adapters->at(idx).c_str();
        return true;
      }, &g_backend_info.Adapters, static_cast<int>(g_backend_info.Adapters.size())))
      {
        Config::SetBaseOrCurrent(Config::GFX_ADAPTER, current_adapter);
        Config::Save();
      }
    }
    */

    bool vSync = Config::Get(Config::GFX_VSYNC);
    if (ImGui::Checkbox("V-Sync", &vSync))
    {
      Config::SetBaseOrCurrent(Config::GFX_VSYNC, vSync);
      Config::Save();
    }

    const char* aspect_items[] = {"Auto", "Force 16:9", "Force 4:3", "Stretch"};
        int aspect_idx = 0;
        auto aspect = Config::Get(Config::GFX_ASPECT_RATIO);
        switch (aspect)
        {
        case AspectMode::Auto:
          aspect_idx = 0;
          break;
        case AspectMode::ForceWide:
          aspect_idx = 1;
          break;
        case AspectMode::ForceStandard:
          aspect_idx = 2;
          break;
        case AspectMode::Stretch:
          aspect_idx = 3;
          break;
        case AspectMode::Custom:
          aspect_idx = 4;
          break;
        case AspectMode::CustomStretch:
          aspect_idx = 5;
          break;
        }

        if (ImGui::Combo("Aspect Ratio", &aspect_idx, aspect_items, IM_ARRAYSIZE(aspect_items)))
        {
          switch (aspect_idx)
          {
          case 0:
            Config::SetBaseOrCurrent(Config::GFX_ASPECT_RATIO, AspectMode::Auto);
            break;
          case 1:
            Config::SetBaseOrCurrent(Config::GFX_ASPECT_RATIO, AspectMode::ForceWide);
            break;
          case 2:
            Config::SetBaseOrCurrent(Config::GFX_ASPECT_RATIO, AspectMode::ForceStandard);
            break;
          case 3:
            Config::SetBaseOrCurrent(Config::GFX_ASPECT_RATIO, AspectMode::Stretch);
            break;
          case 4:
            Config::SetBaseOrCurrent(Config::GFX_ASPECT_RATIO, AspectMode::Custom);
            break;
          case 5:
            Config::SetBaseOrCurrent(Config::GFX_ASPECT_RATIO, AspectMode::CustomStretch);
            break;
          }
          Config::Save();
        }

        const char* ir_items[] = {"Auto (Multiple of 640x528)",      "Native (640x528)",
                              "2x Native (1280x1056) for 720p",  "3x Native (1920x1584) for 1080p",
                              "4x Native (2560x2112) for 1440p", "5x Native (3200x2640)",
                              "6x Native (3840x3168) for 4K",    "7x Native (4480x3696)",
                              "8x Native (5120x4224) for 5K"};

    int ir_idx = Config::Get(Config::GFX_EFB_SCALE);
    if (ImGui::Combo("Internal Resolution", &ir_idx, ir_items, IM_ARRAYSIZE(ir_items)))
    {
      Config::SetBase(Config::GFX_EFB_SCALE, ir_idx);
      Config::Save();
    }

    const char* shader_items[] = {"Synchronous", "Hybrid Ubershaders", "Exclusive Ubershaders",
                                  "Skip Drawing"};
        int shader_idx = 0;
        auto shader = Config::Get(Config::GFX_SHADER_COMPILATION_MODE);
        switch (shader)
        {
        case ShaderCompilationMode::Synchronous:
          shader_idx = 0;
          break;
        case ShaderCompilationMode::SynchronousUberShaders:
          shader_idx = 1;
          break;
        case ShaderCompilationMode::AsynchronousUberShaders:
          shader_idx = 2;
          break;
        case ShaderCompilationMode::AsynchronousSkipRendering:
          shader_idx = 3;
          break;
        }

        if (ImGui::Combo("Shader Compilation Mode", &shader_idx, shader_items, IM_ARRAYSIZE(shader_items)))
        {
          switch (shader_idx)
          {
          case 0:
            Config::SetBaseOrCurrent(Config::GFX_SHADER_COMPILATION_MODE, ShaderCompilationMode::Synchronous);
            break;
          case 1:
            Config::SetBaseOrCurrent(Config::GFX_SHADER_COMPILATION_MODE, ShaderCompilationMode::SynchronousUberShaders);
            break;
          case 2:
            Config::SetBaseOrCurrent(Config::GFX_SHADER_COMPILATION_MODE, ShaderCompilationMode::AsynchronousUberShaders);
            break;
          case 3:
            Config::SetBaseOrCurrent(Config::GFX_SHADER_COMPILATION_MODE, ShaderCompilationMode::AsynchronousSkipRendering);
            break;
          }
          Config::Save();
        }
        ImGui::TextWrapped("Specialized: Ubershaders are never used. Hybrid: Ubershaders prevent stuttering during compilation. Exclusive: Always use ubershaders. Skip Drawing: Prevents stuttering by not rendering waiting objects.");

        bool waitForCompile = Config::Get(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING);
        if (ImGui::Checkbox("Compile Shaders Before Starting", &waitForCompile))
        {
          Config::SetBaseOrCurrent(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING, waitForCompile);
          Config::Save();
        }
        ImGui::TextWrapped("Waits for all shaders to finish compiling before starting a game.");

    bool shaderCache = Config::Get(Config::GFX_SHADER_CACHE);
    if (ImGui::Checkbox("Shader Cache", &shaderCache))
    {
      Config::SetBaseOrCurrent(Config::GFX_SHADER_CACHE, shaderCache);
      Config::Save();
    }
  }

  if (ImGui::CollapsingHeader("Enhancements"))
  {
    bool perPixelLighting = Config::Get(Config::GFX_ENABLE_PIXEL_LIGHTING);
    if (ImGui::Checkbox("Per-Pixel Lighting", &perPixelLighting))
    {
      Config::SetBaseOrCurrent(Config::GFX_ENABLE_PIXEL_LIGHTING, perPixelLighting);
      Config::Save();
    }

    bool disableFog = Config::Get(Config::GFX_DISABLE_FOG);
    if (ImGui::Checkbox("Disable Fog", &disableFog))
    {
      Config::SetBaseOrCurrent(Config::GFX_DISABLE_FOG, disableFog);
      Config::Save();
    }

    bool disableCopyFilter = Config::Get(Config::GFX_ENHANCE_DISABLE_COPY_FILTER);
    if (ImGui::Checkbox("Disable Copy Filter", &disableCopyFilter))
    {
      Config::SetBaseOrCurrent(Config::GFX_ENHANCE_DISABLE_COPY_FILTER, disableCopyFilter);
      Config::Save();
    }

    bool forceTrueColor = Config::Get(Config::GFX_ENHANCE_FORCE_TRUE_COLOR);
    if (ImGui::Checkbox("Force True Color", &forceTrueColor))
    {
      Config::SetBaseOrCurrent(Config::GFX_ENHANCE_FORCE_TRUE_COLOR, forceTrueColor);
          Config::Save();
        }
        ImGui::TextWrapped("Disables the blending of adjacent rows when copying the EFB. May improve quality in some cases.");

    bool arbitraryMipmapDetection = Config::Get(Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION);
    if (ImGui::Checkbox("Arbitrary Mipmap Detection", &arbitraryMipmapDetection))
    {
      Config::SetBaseOrCurrent(Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION,
                               arbitraryMipmapDetection);
          Config::Save();
        }
        ImGui::TextWrapped("Enables detection of arbitrary mipmaps, which some games use for special distance-based effects.");

    if (arbitraryMipmapDetection)
    {
      float threshold = Config::Get(Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION_THRESHOLD);
      if (ImGui::SliderFloat("Mipmap Detection Threshold", &threshold, 1.0f, 50.0f, "%.1f"))
      {
        Config::SetBaseOrCurrent(Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION_THRESHOLD,
                                 threshold);
        Config::Save();
      }
      ImGui::TextWrapped("Adjusts the threshold for detecting arbitrary mipmaps. Higher values "
                         "increase detection sensitivity but may cause false positives.");
    }

#ifndef WINRT_XBOX  // I don't have a hdr impl yet for UWP.
    bool hdrOutput = Config::Get(Config::GFX_ENHANCE_HDR_OUTPUT);
    if (ImGui::Checkbox("HDR Output", &hdrOutput))
    {
      Config::SetBaseOrCurrent(Config::GFX_ENHANCE_HDR_OUTPUT, hdrOutput);
      Config::Save();
    }
    ImGui::TextWrapped(
        "Enables HDR output when supported by the display. Requires a compatible GPU and display.");
#endif
    const char* anisolevel_items[] = {"1x", "2x", "4x", "8x", "16x"};
    auto aniso = Config::Get(Config::GFX_ENHANCE_MAX_ANISOTROPY);
    int aniso_idx = static_cast<int>(aniso);
    if (ImGui::Combo("Anisotropic Filtering", &aniso_idx, anisolevel_items,
                     IM_ARRAYSIZE(anisolevel_items)))
    {
      Config::SetBaseOrCurrent(Config::GFX_ENHANCE_MAX_ANISOTROPY,
                               static_cast<AnisotropicFilteringMode>(aniso_idx));
      Config::Save();
    }

    bool ssaa = Config::Get(Config::GFX_SSAA);
    if (ImGui::RadioButton("MSAA", !ssaa))
    {
      Config::SetBaseOrCurrent(Config::GFX_SSAA, false);
      Config::Save();
    }

    if (ImGui::RadioButton("SSAA", ssaa))
    {
      Config::SetBaseOrCurrent(Config::GFX_SSAA, true);
      Config::Save();
    }

    const char* aalevel_items[] = {"None", "2x", "4x", "8x"};
    auto msaa = Config::Get(Config::GFX_MSAA);
    int msaa_idx = msaa;
    if (ImGui::Combo("MSAA Level", &msaa_idx, aalevel_items, IM_ARRAYSIZE(aalevel_items)))
    {
      Config::SetBaseOrCurrent(Config::GFX_MSAA, msaa_idx);
      Config::Save();
    }
  }

  if (ImGui::CollapsingHeader("Hacks"))
  {
    if (ImGui::CollapsingHeader("Embedded Frame Buffer (EFB)"))
    {
      bool skipEfbCpu = Config::Get(Config::GFX_HACK_EFB_ACCESS_ENABLE);
      if (ImGui::Checkbox("Enable EFB Access from CPU", &skipEfbCpu))
      {
        Config::SetBaseOrCurrent(Config::GFX_HACK_EFB_ACCESS_ENABLE, skipEfbCpu);
        Config::Save();
      }

        bool ignoreFormatChanges = Config::Get(Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES);
        if (ImGui::Checkbox("Emulate Format Changes", &ignoreFormatChanges))
        {
          Config::SetBaseOrCurrent(Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES, ignoreFormatChanges);
          Config::Save();
        }
        ImGui::TextWrapped("Emulates EFB format changes in software. Required for some games to render correctly.");

        bool storeEfbCopies = Config::Get(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM);
        if (ImGui::Checkbox("Store EFB Copies to Texture Only", &storeEfbCopies))
        {
          Config::SetBaseOrCurrent(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM, storeEfbCopies);
          Config::Save();
        }
        ImGui::TextWrapped("Stores EFB copies in GPU texture objects instead of RAM. Improves performance but breaks some games.");

      bool deferEfbCopies = Config::Get(Config::GFX_HACK_DEFER_EFB_COPIES);
      if (ImGui::Checkbox("Defer EFB Copies to RAM", &deferEfbCopies))
      {
        Config::SetBaseOrCurrent(Config::GFX_HACK_DEFER_EFB_COPIES, deferEfbCopies);
        Config::Save();
      }
    }

    if (ImGui::CollapsingHeader("Texture Cache"))
      {
        int accuracy = Config::Get(Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES);

        int slider_index = 0;
        if (accuracy == 512)
          slider_index = 1;
        else if (accuracy == 128)
          slider_index = 2;

        const char* accuracy_labels[] = {"Safe (0)", "Normal (512)", "Fast (128)"};
        if (ImGui::Combo("Texture Cache Accuracy", &slider_index, accuracy_labels, 3))
        {
          int new_accuracy = 0;
          if (slider_index == 1)
            new_accuracy = 512;
          else if (slider_index == 2)
            new_accuracy = 128;

          Config::SetBaseOrCurrent(Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES, new_accuracy);
          Config::Save();
        }
        ImGui::TextWrapped("Adjusts how often the texture cache is invalidated. Safe provides most compatibility.");

      bool gpuTextureDecoding = Config::Get(Config::GFX_ENABLE_GPU_TEXTURE_DECODING);
      if (ImGui::Checkbox("GPU Texture Decoding", &gpuTextureDecoding))
      {
        Config::SetBaseOrCurrent(Config::GFX_ENABLE_GPU_TEXTURE_DECODING, gpuTextureDecoding);
        Config::Save();
      }
    }

    if (ImGui::CollapsingHeader("External Frame Buffer (XFB)"))
    {
      bool storeXfbCopies = Config::Get(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM);
      if (ImGui::Checkbox("Store XFB Copies to Texture Only", &storeXfbCopies))
      {
        Config::SetBaseOrCurrent(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM, storeXfbCopies);
        Config::Save();
      }

      bool immediateXfb = Config::Get(Config::GFX_HACK_IMMEDIATE_XFB);
      if (ImGui::Checkbox("Immediately Present XFB", &immediateXfb))
      {
        Config::SetBaseOrCurrent(Config::GFX_HACK_IMMEDIATE_XFB, immediateXfb);
        Config::Save();
      }

      bool skipDuplicateXfbs = Config::Get(Config::GFX_HACK_SKIP_DUPLICATE_XFBS);
      if (ImGui::Checkbox("Skip Presenting Duplicate Frames", &skipDuplicateXfbs))
      {
        Config::SetBaseOrCurrent(Config::GFX_HACK_SKIP_DUPLICATE_XFBS, skipDuplicateXfbs);
        Config::Save();
      }
    }

    if (ImGui::CollapsingHeader("Other"))
      {
        bool fastDepthCalculation = Config::Get(Config::GFX_FAST_DEPTH_CALC);
        if (ImGui::Checkbox("Fast Depth Calculation", &fastDepthCalculation))
        {
          Config::SetBaseOrCurrent(Config::GFX_FAST_DEPTH_CALC, fastDepthCalculation);
          Config::Save();
        }
        ImGui::TextWrapped("Uses a less accurate algorithm to calculate depth values.");

        bool enableBoundingBox = Config::Get(Config::GFX_HACK_BBOX_ENABLE);
        if (ImGui::Checkbox("Enable Bounding Box", &enableBoundingBox))
        {
          Config::SetBaseOrCurrent(Config::GFX_HACK_BBOX_ENABLE, enableBoundingBox);
          Config::Save();
        }
        ImGui::TextWrapped("Enables bounding box emulation. Required for some games.");

      bool vertexRounding = Config::Get(Config::GFX_HACK_VERTEX_ROUNDING);
      if (ImGui::Checkbox("Vertex Rounding", &vertexRounding))
      {
        Config::SetBaseOrCurrent(Config::GFX_HACK_VERTEX_ROUNDING, vertexRounding);
        Config::Save();
      }

      bool saveTextureCacheState = Config::Get(Config::GFX_SAVE_TEXTURE_CACHE_TO_STATE);
      if (ImGui::Checkbox("Save Texture Cache to State", &saveTextureCacheState))
      {
        Config::SetBaseOrCurrent(Config::GFX_SAVE_TEXTURE_CACHE_TO_STATE, saveTextureCacheState);
        Config::Save();
      }

        bool viSkip = Config::Get(Config::GFX_HACK_VI_SKIP);
        if (ImGui::Checkbox("VBI Skip", &viSkip))
        {
          Config::SetBaseOrCurrent(Config::GFX_HACK_VI_SKIP, viSkip);
          Config::Save();
        }
    }
  }

  if (ImGui::CollapsingHeader("Advanced"))
  {
    if (ImGui::CollapsingHeader("Performance Statistics"))
    {
      bool showFps = Config::Get(Config::GFX_SHOW_FPS);
      if (ImGui::Checkbox("Show FPS", &showFps))
      {
        Config::SetBaseOrCurrent(Config::GFX_SHOW_FPS, showFps);
        Config::Save();
      }

        bool showFTimes = Config::Get(Config::GFX_SHOW_FTIMES);
        if (ImGui::Checkbox("Show Frame Times", &showFTimes))
        {
          Config::SetBaseOrCurrent(Config::GFX_SHOW_FTIMES, showFTimes);
          Config::Save();
        }

      bool showVps = Config::Get(Config::GFX_SHOW_VPS);
      if (ImGui::Checkbox("Show VPS", &showVps))
      {
        Config::SetBaseOrCurrent(Config::GFX_SHOW_VPS, showVps);
        Config::Save();
      }

        bool showVTimes = Config::Get(Config::GFX_SHOW_VTIMES);
        if (ImGui::Checkbox("Show VI Times", &showVTimes))
        {
          Config::SetBaseOrCurrent(Config::GFX_SHOW_VTIMES, showVTimes);
          Config::Save();
        }

      bool showSpeed = Config::Get(Config::GFX_SHOW_SPEED);
      if (ImGui::Checkbox("Show Speed", &showSpeed))
      {
        Config::SetBaseOrCurrent(Config::GFX_SHOW_SPEED, showSpeed);
        Config::Save();
      }

      bool showSpeedColors = Config::Get(Config::GFX_SHOW_SPEED_COLORS);
      if (ImGui::Checkbox("Show Speed Colors", &showSpeedColors))
      {
        Config::SetBaseOrCurrent(Config::GFX_SHOW_SPEED_COLORS, showSpeedColors);
        Config::Save();
      }

      bool overlayStats = Config::Get(Config::GFX_OVERLAY_STATS);
      if (ImGui::Checkbox("Show Rendering Statistics", &overlayStats))
      {
        Config::SetBaseOrCurrent(Config::GFX_OVERLAY_STATS, overlayStats);
        Config::Save();
      }

        bool projStats = Config::Get(Config::GFX_OVERLAY_PROJ_STATS);
        if (ImGui::Checkbox("Show Projection Statistics", &projStats))
        {
          Config::SetBaseOrCurrent(Config::GFX_OVERLAY_PROJ_STATS, projStats);
          Config::Save();
        }

        bool scissorStats = Config::Get(Config::GFX_OVERLAY_SCISSOR_STATS);
        if (ImGui::Checkbox("Show Scissor Statistics", &scissorStats))
        {
          Config::SetBaseOrCurrent(Config::GFX_OVERLAY_SCISSOR_STATS, scissorStats);
          Config::Save();
        }

      bool logRenderTime = Config::Get(Config::GFX_LOG_RENDER_TIME_TO_FILE);
      if (ImGui::Checkbox("Log Render Time to File", &logRenderTime))
      {
        Config::SetBaseOrCurrent(Config::GFX_LOG_RENDER_TIME_TO_FILE, logRenderTime);
        Config::Save();
      }
    }

    if (ImGui::CollapsingHeader("Debugging"))
    {
      bool enableWireframe = Config::Get(Config::GFX_ENABLE_WIREFRAME);
      if (ImGui::Checkbox("Enable Wireframe", &enableWireframe))
      {
        Config::SetBaseOrCurrent(Config::GFX_ENABLE_WIREFRAME, enableWireframe);
        Config::Save();
      }

      bool showProjStats = Config::Get(Config::GFX_OVERLAY_PROJ_STATS);
      if (ImGui::Checkbox("Show Projection Statistics", &showProjStats))
      {
        Config::SetBaseOrCurrent(Config::GFX_OVERLAY_PROJ_STATS, showProjStats);
        Config::Save();
      }

      bool enableFormatOverlay = Config::Get(Config::GFX_TEXFMT_OVERLAY_ENABLE);
      if (ImGui::Checkbox("Texture Format Overlay", &enableFormatOverlay))
      {
        Config::SetBaseOrCurrent(Config::GFX_TEXFMT_OVERLAY_ENABLE, enableFormatOverlay);
        Config::Save();
      }

      bool enableApiValidation = Config::Get(Config::GFX_ENABLE_VALIDATION_LAYER);
      if (ImGui::Checkbox("Enable API Validation Layers", &enableApiValidation))
      {
        Config::SetBaseOrCurrent(Config::GFX_ENABLE_VALIDATION_LAYER, enableApiValidation);
        Config::Save();
      }
    }

    if (ImGui::CollapsingHeader("Utility"))
    {
      bool loadCustomTextures = Config::Get(Config::GFX_HIRES_TEXTURES);
      if (ImGui::Checkbox("Load Custom Textures", &loadCustomTextures))
      {
        Config::SetBaseOrCurrent(Config::GFX_HIRES_TEXTURES, loadCustomTextures);
        Config::Save();
      }

      bool prefetchCustomTextures = Config::Get(Config::GFX_CACHE_HIRES_TEXTURES);
      if (ImGui::Checkbox("Prefetch Custom Textures", &prefetchCustomTextures))
      {
        Config::SetBaseOrCurrent(Config::GFX_CACHE_HIRES_TEXTURES, prefetchCustomTextures);
        Config::Save();
      }

      bool disableVramCopies = Config::Get(Config::GFX_HACK_DISABLE_COPY_TO_VRAM);
      if (ImGui::Checkbox("Disable EFB VRAM Copies", &disableVramCopies))
      {
        Config::SetBaseOrCurrent(Config::GFX_HACK_DISABLE_COPY_TO_VRAM, disableVramCopies);
        Config::Save();
      }

      bool enableGraphicsMods = Config::Get(Config::GFX_MODS_ENABLE);
      if (ImGui::Checkbox("Enable Graphics Mods", &enableGraphicsMods))
      {
        Config::SetBaseOrCurrent(Config::GFX_MODS_ENABLE, enableGraphicsMods);
        Config::Save();
      }
    }

    if (ImGui::CollapsingHeader("Texture Dumping"))
    {
      bool dumpTextures = Config::Get(Config::GFX_DUMP_TEXTURES);
      if (ImGui::Checkbox("Enable", &dumpTextures))
      {
        Config::SetBaseOrCurrent(Config::GFX_DUMP_TEXTURES, dumpTextures);
        Config::Save();
      }

      bool dumpBaseTextures = Config::Get(Config::GFX_DUMP_BASE_TEXTURES);
      if (ImGui::Checkbox("Dump Base Textures", &dumpBaseTextures))
      {
        Config::SetBaseOrCurrent(Config::GFX_DUMP_BASE_TEXTURES, dumpBaseTextures);
        Config::Save();
      }

      bool dumpMipTextures = Config::Get(Config::GFX_DUMP_MIP_TEXTURES);
      if (ImGui::Checkbox("Dump Mip Maps", &dumpMipTextures))
      {
        Config::SetBaseOrCurrent(Config::GFX_DUMP_MIP_TEXTURES, dumpMipTextures);
        Config::Save();
      }
    }

    if (ImGui::CollapsingHeader("Frame Dumping"))
    {
      const char* resolution_items[] = {"Window Resolution",
                                        "Aspect Ratio Corrected Internal Resolution",
                                        "Raw Internal Resolution"};
      auto resolution_type = Config::Get(Config::GFX_FRAME_DUMPS_RESOLUTION_TYPE);
      int resolution_idx = static_cast<int>(resolution_type);
      if (ImGui::Combo("Resolution Type", &resolution_idx, resolution_items,
                       IM_ARRAYSIZE(resolution_items)))
      {
        Config::SetBaseOrCurrent(Config::GFX_FRAME_DUMPS_RESOLUTION_TYPE,
                                 static_cast<FrameDumpResolutionType>(resolution_idx));
        Config::Save();
      }

      int pngCompression = Config::Get(Config::GFX_PNG_COMPRESSION_LEVEL);
      if (ImGui::SliderInt("PNG Compression Level", &pngCompression, 0, 9))
      {
        Config::SetBaseOrCurrent(Config::GFX_PNG_COMPRESSION_LEVEL, pngCompression);
        Config::Save();
      }
    }

    if (ImGui::CollapsingHeader("Misc"))
    {
      bool enableProgScan = Config::Get(Config::SYSCONF_PROGRESSIVE_SCAN);
      if (ImGui::Checkbox("Enable Progressive Scan", &enableProgScan))
      {
        Config::SetBaseOrCurrent(Config::SYSCONF_PROGRESSIVE_SCAN, enableProgScan);
        Config::Save();
      }

#ifndef WINRT_XBOX  // This currently only works on the Vulkan backend
        bool backendMultithreading = Config::Get(Config::GFX_BACKEND_MULTITHREADING);
        if (ImGui::Checkbox("Backend Multithreading", &backendMultithreading))
        {
          Config::SetBaseOrCurrent(Config::GFX_BACKEND_MULTITHREADING, backendMultithreading);
          Config::Save();
        }
        ImGui::TextWrapped("Enables multithreaded command submission. Currently only works with Vulkan backend.");
#endif

        int commandBufferInterval = Config::Get(Config::GFX_COMMAND_BUFFER_EXECUTE_INTERVAL);
        if (ImGui::SliderInt("Command Buffer Execute Interval", &commandBufferInterval, 0, 1000))
        {
          Config::SetBaseOrCurrent(Config::GFX_COMMAND_BUFFER_EXECUTE_INTERVAL, commandBufferInterval);
          Config::Save();
        }
        ImGui::TextWrapped("Submits command buffers every N draw calls. Lower values may reduce latency.");

        int shaderCompilerThreads = Config::Get(Config::GFX_SHADER_COMPILER_THREADS);
        if (ImGui::SliderInt("Shader Compiler Threads", &shaderCompilerThreads, 1, 8))
        {
          Config::SetBaseOrCurrent(Config::GFX_SHADER_COMPILER_THREADS, shaderCompilerThreads);
          Config::Save();
        }

        int shaderPrecompilerThreads = Config::Get(Config::GFX_SHADER_PRECOMPILER_THREADS);
        if (ImGui::SliderInt("Shader Precompiler Threads", &shaderPrecompilerThreads, -1, 8))
        {
          Config::SetBaseOrCurrent(Config::GFX_SHADER_PRECOMPILER_THREADS, shaderPrecompilerThreads);
          Config::Save();
        }
        ImGui::TextWrapped("-1 = Automatic, 0 = Disabled, >0 = Specific number of threads");

      bool preferVsForLinePoint = Config::Get(Config::GFX_PREFER_VS_FOR_LINE_POINT_EXPANSION);
      if (ImGui::Checkbox("Prefer VS for Point/Line Expansion", &preferVsForLinePoint))
      {
        Config::SetBaseOrCurrent(Config::GFX_PREFER_VS_FOR_LINE_POINT_EXPANSION,
                                 preferVsForLinePoint);
        Config::Save();
      }

      bool cpuCull = Config::Get(Config::GFX_CPU_CULL);
      if (ImGui::Checkbox("Cull Vertices on the CPU", &cpuCull))
      {
        Config::SetBaseOrCurrent(Config::GFX_CPU_CULL, cpuCull);
        Config::Save();
      }
    }

    if (ImGui::CollapsingHeader("Experimental"))
    {
      bool deferEfbAccess = Config::Get(Config::GFX_HACK_EFB_DEFER_INVALIDATION);
      if (ImGui::Checkbox("Defer EFB Cache Invalidation", &deferEfbAccess))
      {
        Config::SetBaseOrCurrent(Config::GFX_HACK_EFB_DEFER_INVALIDATION, deferEfbAccess);
        Config::Save();
      }

      bool manualTextureSampling = Config::Get(Config::GFX_HACK_FAST_TEXTURE_SAMPLING);
      if (ImGui::Checkbox("Fast Texture Sampling", &manualTextureSampling))
      {
        Config::SetBaseOrCurrent(Config::GFX_HACK_FAST_TEXTURE_SAMPLING, manualTextureSampling);
        Config::Save();
      }
    }
  }
}

void CreateAudioTab(UIState* state)
{
  if (ImGui::CollapsingHeader("DSP Options"))
  {
    const char* dsp_items[] = {"HLE", "LLE Recompiler", "LLE Interpreter"};
    int dsp_idx =
        Config::Get(Config::MAIN_DSP_HLE) ? 0 : (Config::Get(Config::MAIN_DSP_JIT) ? 1 : 2);
    if (ImGui::Combo("DSP Emulation Engine", &dsp_idx, dsp_items, IM_ARRAYSIZE(dsp_items)))
    {
      Config::SetBaseOrCurrent(Config::MAIN_DSP_HLE, dsp_idx == 0);
      Config::SetBaseOrCurrent(Config::MAIN_DSP_JIT, dsp_idx == 1);
      Config::Save();
    }
    ImGui::TextWrapped("Selects how the Digital Signal Processor (DSP) is emulated. Determines how "
                       "the audio is processed and what system features are available.");
  }

  if (ImGui::CollapsingHeader("Volume"))
  {
    ImGui::Spacing();
    ImGui::PushID("VolumeControl");
    int volume = Config::Get(Config::MAIN_AUDIO_MUTED) ? 0 : Config::Get(Config::MAIN_AUDIO_VOLUME);
    bool slider_active = ImGui::SliderInt("Volume", &volume, 0, 100);
    if (slider_active)
    {
      Config::SetBaseOrCurrent(Config::MAIN_AUDIO_MUTED, volume == 0);
      Config::SetBaseOrCurrent(Config::MAIN_AUDIO_VOLUME, volume);
      Config::Save();
      AudioCommon::UpdateSoundStream(Core::System::GetInstance());
    }
    ImGui::PopID();
  }

  if (ImGui::CollapsingHeader("Backend Settings"))
  {
    std::vector<std::string> backends = AudioCommon::GetSoundBackends();
    std::string current_backend = Config::Get(Config::MAIN_AUDIO_BACKEND);
    int backend_idx = 0;
    for (size_t i = 0; i < backends.size(); ++i)
    {
      if (backends[i] == current_backend)
      {
        backend_idx = static_cast<int>(i);
        break;
      }
    }

    if (ImGui::Combo(
            "Audio Backend", &backend_idx,
            [](void* data, int idx, const char** out_text) {
              auto* backends = static_cast<std::vector<std::string>*>(data);
              *out_text = backends->at(idx).c_str();
              return true;
            },
            &backends, static_cast<int>(backends.size())))
    {
      Config::SetBaseOrCurrent(Config::MAIN_AUDIO_BACKEND, backends[backend_idx]);
      Config::Save();
    }

#ifdef _WIN32
    if (backends[backend_idx] == BACKEND_WASAPI)
    {
      std::vector<std::string> devices = WASAPIStream::GetAvailableDevices();
      std::string current_device = Config::Get(Config::MAIN_WASAPI_DEVICE);
      int device_idx = 0;
      for (size_t i = 0; i < devices.size(); ++i)
      {
        if (devices[i] == current_device)
        {
          device_idx = static_cast<int>(i);
          break;
        }
      }

      if (ImGui::Combo(
              "Output Device", &device_idx,
              [](void* data, int idx, const char** out_text) {
                auto* devices = static_cast<std::vector<std::string>*>(data);
                *out_text = devices->at(idx).c_str();
                return true;
              },
              &devices, static_cast<int>(devices.size())))
      {
        Config::SetBaseOrCurrent(Config::MAIN_WASAPI_DEVICE, devices[device_idx]);
        Config::Save();
      }
    }
#endif

    if (AudioCommon::SupportsLatencyControl(backends[backend_idx]))
    {
      int latency = Config::Get(Config::MAIN_AUDIO_LATENCY);
      if (ImGui::SliderInt("Latency", &latency, 0, 200))
      {
        Config::SetBaseOrCurrent(Config::MAIN_AUDIO_LATENCY, latency);
        Config::Save();
      }
      ImGui::TextWrapped(
          "Sets the audio latency in milliseconds. Higher values may reduce audio crackling.");
    }
  }
#ifndef WINRT_XBOX  // TODO: Can't figure out how this one works will look into more if it's
                    // requested or is required
  if (ImGui::CollapsingHeader("Dolby Pro Logic II"))
  {
    bool dpl2 = Config::Get(Config::MAIN_DPL2_DECODER);
    if (ImGui::Checkbox("Enable Dolby Pro Logic II", &dpl2))
    {
      Config::SetBaseOrCurrent(Config::MAIN_DPL2_DECODER, dpl2);
      Config::Save();
    }
    ImGui::TextWrapped(
        "Enables Dolby Pro Logic II emulation using 5.1 surround. Certain backends only.");
    if (dpl2)
    {
      const char* quality_items[] = {"Lowest (Latency ~10 ms)", "Low (Latency ~20 ms)",
                                     "High (Latency ~40 ms)", "Highest (Latency ~80 ms)"};
      int quality_idx = static_cast<int>(Config::Get(Config::MAIN_DPL2_QUALITY));
      if (ImGui::Combo("Decoding Quality", &quality_idx, quality_items,
                       IM_ARRAYSIZE(quality_items)))
      {
        Config::SetBaseOrCurrent(Config::MAIN_DPL2_QUALITY,
                                 static_cast<AudioCommon::DPL2Quality>(quality_idx));
        Config::Save();
      }
    }
  }
#endif
}

void CreateGCPort(int index, std::vector<std::string> devices)
{
  if (ImGui::BeginChild(fmt::format("gc-port-{}", index).c_str(), ImVec2(-1, 75 * m_frame_scale),
                        true))
  {
    auto controller = Pad::GetConfig()->GetController(index);
    auto default_device = controller->GetDefaultDevice().name;

    ImGui::Text("GameCube Port %d", index + 1);

    if (ImGui::BeginCombo("Device", default_device.c_str()))
    {
      for (auto device : devices)
      {
        if (ImGui::Selectable(device.c_str(), strcmp(default_device.c_str(), device.c_str()) == 0))
        {
          controller->SetDefaultDevice(device);
          controller->UpdateReferences(g_controller_interface);
          Pad::GetConfig()->SaveConfig();
        }
      }

      ImGui::EndCombo();
    }

    // Game Vault: the box shows what the port is set to now (None when switched off, else the profile picked,
    // Default when none was picked this session)
    std::string gc_shown = m_selected_gc_profile[index];
    if (Config::Get(Config::GetInfoForSIDevice(index)) == SerialInterface::SIDevices::SIDEVICE_NONE)
      gc_shown = "None";
    else if (gc_shown.empty() || gc_shown == "None")
      gc_shown = "Default";

    if (ImGui::BeginCombo("Profile", gc_shown.c_str()))
    {
      for (auto profile : m_gc_profiles)
      {
        if (ImGui::Selectable(profile.c_str(), gc_shown == profile))
        {
          m_selected_gc_profile[index] = profile;

          if (m_selected_gc_profile[index] == "None")
          {
            // Loading an empty inifile section clears everything.
            Common::IniFile::Section sec;

            controller->LoadConfig(&sec);
            controller->SetDefaultDevice(default_device);
            Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(index),
                                     SerialInterface::SIDevices::SIDEVICE_NONE);
          }
          else if (m_selected_gc_profile[index] == "Default")
          {
            controller->LoadDefaults(g_controller_interface);
            Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(index),
                                     SerialInterface::SIDevices::SIDEVICE_GC_CONTROLLER);
          }
          else
          {
            Common::IniFile ini;
            ini.Load(File::GetUserPath(D_CONFIG_IDX) + PROFILES_DIR +
                     Pad::GetConfig()->GetProfileDirectoryName() + "/" + profile + ".ini");

            controller->LoadConfig(ini.GetOrCreateSection("Profile"));
            Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(index),
                                     SerialInterface::SIDevices::SIDEVICE_GC_CONTROLLER);
          }

          controller->UpdateReferences(g_controller_interface);
          Pad::GetConfig()->SaveConfig();
          Config::Save();
        }
      }

      ImGui::EndCombo();
    }
  }

  ImGui::EndChild();
}

void CreateWiiPort(int index, std::vector<std::string> devices)
{
  if (ImGui::BeginChild(fmt::format("gc-wii-{}", index).c_str(), ImVec2(-1, 75 * m_frame_scale),
                        true))
  {
    auto controller = Wiimote::GetConfig()->GetController(index);
    auto default_device = controller->GetDefaultDevice().name;

    ImGui::Text("Wiimote Port %d", index + 1);

    if (ImGui::BeginCombo("Device", default_device.c_str()))
    {
      for (auto device : devices)
      {
        if (ImGui::Selectable(device.c_str(), strcmp(default_device.c_str(), device.c_str()) == 0))
        {
          controller->SetDefaultDevice(device);
          controller->UpdateReferences(g_controller_interface);
          Wiimote::GetConfig()->SaveConfig();
        }
      }

      ImGui::EndCombo();
    }

    // Game Vault: Default / None like the GameCube ports, and the box shows what the port is set to now
    // (None: no Wii Remote, for games played with a GameCube controller such as Mario Kart Wii)
    std::string wii_shown = m_selected_wiimote_profile[index];
    if (Config::Get(Config::GetInfoForWiimoteSource(index)) == WiimoteSource::None)
      wii_shown = "None";
    else if (wii_shown.empty() || wii_shown == "None" || wii_shown == "Wiimote + Nunchuk")
      wii_shown = "Default";

    if (ImGui::BeginCombo("Profile", wii_shown.c_str()))
    {
      for (auto profile : m_wiimote_profiles)
      {
        if (ImGui::Selectable(profile.c_str(), wii_shown == profile))
        {
          m_selected_wiimote_profile[index] = profile;

          if (m_selected_wiimote_profile[index] == "None")
          {
            Common::IniFile::Section sec;

            controller->LoadConfig(&sec);
            controller->SetDefaultDevice(default_device);
            Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(index), WiimoteSource::None);
          }
          else if (m_selected_wiimote_profile[index] == "Default" ||
                   m_selected_wiimote_profile[index] == "Wiimote + Nunchuk")
          {
            controller->LoadDefaults(g_controller_interface);

            Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(index),
                                     WiimoteSource::Emulated);
          }
          else if (m_selected_wiimote_profile[index] == "Classic Controller")
          {
            Common::IniFile ini;
            ini.Load(File::GetSysDirectory() + PROFILES_DIR +
                     Wiimote::GetConfig()->GetProfileDirectoryName() + "/Classic.ini");

            controller->LoadConfig(ini.GetOrCreateSection("Profile"));
            Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(index),
                                     WiimoteSource::Emulated);
          }
          else if (m_selected_wiimote_profile[index] == "Sideways Wiimote")
          {
            Common::IniFile ini;
            ini.Load(File::GetSysDirectory() + PROFILES_DIR +
                     Wiimote::GetConfig()->GetProfileDirectoryName() + "/Sideways.ini");

            controller->LoadConfig(ini.GetOrCreateSection("Profile"));
            Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(index),
                                     WiimoteSource::Emulated);
          }
          else
          {
            Common::IniFile ini;
            ini.Load(File::GetUserPath(D_CONFIG_IDX) + PROFILES_DIR +
                     Wiimote::GetConfig()->GetProfileDirectoryName() + "/" + profile + ".ini");

            controller->LoadConfig(ini.GetOrCreateSection("Profile"));
            Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(index),
                                     WiimoteSource::Emulated);
          }

          controller->UpdateReferences(g_controller_interface);
          Wiimote::GetConfig()->SaveConfig();
          Config::Save();
        }
      }

      ImGui::EndCombo();
    }
  }

  ImGui::EndChild();
}

void CreateControlsTab(UIState* state)
{
  EnsureGCProfilesLoaded();
  auto devices = g_controller_interface.GetAllDeviceStrings();

  if (ImGui::BeginTabBar("controlsbar"))
  {
    if (ImGui::BeginTabItem("GameCube"))
    {
      if (ImGui::BeginTable("gc_ports_tbl", 2, ImGuiTableFlags_SizingStretchProp))
      {
        ImGui::TableSetupColumn("Port", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Map", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableHeadersRow();
        for (int i = 0; i < 4; i++)
        {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          CreateGCPort(i, devices);
          ImGui::TableNextColumn();
          if (ImGui::Button(fmt::format("Map##gc{}", i).c_str(), ImVec2(-1, 0)))
          {
            state->showMappingWindow = true;
            state->mappingWindowPort = i;
            state->mappingWindowIsWii = false;
          }
        }
        ImGui::EndTable();
      }
      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Wii"))
    {
      if (ImGui::BeginTable("wii_ports_tbl", 2, ImGuiTableFlags_SizingStretchProp))
      {
        ImGui::TableSetupColumn("Port", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Map", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableHeadersRow();
        for (int i = 0; i < 4; i++)
        {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          CreateWiiPort(i, devices);
          ImGui::TableNextColumn();
          if (ImGui::Button(fmt::format("Map##wii{}", i).c_str(), ImVec2(-1, 0)))
          {
            state->showMappingWindow = true;
            state->mappingWindowPort = i;
            state->mappingWindowIsWii = true;
          }
        }
        ImGui::EndTable();
      }
      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Alternate Input Sources"))
    {
      bool dsu_enabled = Config::Get(ciface::DualShockUDPClient::Settings::SERVERS_ENABLED);
      if (ImGui::Checkbox("Enable DSU", &dsu_enabled))
      {
        Config::SetBaseOrCurrent(ciface::DualShockUDPClient::Settings::SERVERS_ENABLED,
                                 dsu_enabled);
        Config::Save();
        if (dsu_enabled)
          g_controller_interface.RefreshDevices();
      }
      ImGui::Separator();
      std::string servers = Config::Get(ciface::DualShockUDPClient::Settings::SERVERS);
      std::vector<std::string> entries;
      for (size_t p = 0, n; p < servers.size(); p = n + 1)
      {
        n = servers.find(';', p);
        if (n == std::string::npos)
          break;
        auto e = servers.substr(p, n - p);
        if (!e.empty())
          entries.push_back(e);
      }
      static int selected_dsu = -1;
      for (int i = 0; i < (int)entries.size(); ++i)
      {
        if (ImGui::Selectable(entries[i].c_str(), selected_dsu == i))
          selected_dsu = i;
      }
      ImGui::Separator();
      static char dsu_addr[64] = "";
      static int dsu_port = ciface::DualShockUDPClient::DEFAULT_SERVER_PORT;
      ImGui::Text("Address");
      ImGui::SameLine();
      if (ImGui::Button("Edit IP"))
      {
        UWP::ShowKeyboard();
        ImGui::SetKeyboardFocusHere();
      }
      ImGui::SameLine();
      ImGui::InputText("##dsu_addr", dsu_addr, sizeof(dsu_addr));
      ImGui::InputInt("Port", &dsu_port);
      if (ImGui::Button("Add Server"))
      {
        auto entry = fmt::format("DS4:{}:{};", dsu_addr, dsu_port);
        Config::SetBaseOrCurrent(ciface::DualShockUDPClient::Settings::SERVERS, servers + entry);
        Config::Save();
        servers = Config::Get(ciface::DualShockUDPClient::Settings::SERVERS);
      }
      ImGui::SameLine();
      if (ImGui::Button("Remove Selected") && selected_dsu >= 0)
      {
        std::string rebuilt;
        for (int i = 0; i < (int)entries.size(); ++i)
        {
          if (i == selected_dsu)
            continue;
          rebuilt += entries[i] + ";";
        }
        Config::SetBaseOrCurrent(ciface::DualShockUDPClient::Settings::SERVERS, rebuilt);
        Config::Save();
        selected_dsu = -1;
      }
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }

  if (state->showMappingWindow)
    ImGuiMappingWindow::Draw(state->mappingWindowPort, state);
}

void CreateGameCubeTab(UIState* state)
{
  if (ImGui::CollapsingHeader("IPL Settings"))
  {
    const char* language_items[] = {"English", "German", "French", "Spanish", "Italian", "Dutch"};
    auto lang = Config::Get(Config::MAIN_GC_LANGUAGE);

    for (int i = 0; i < 6; i++)
    {
      ImGui::PushID(i);
      if (ImGui::RadioButton(language_items[i], i == lang))
      {
        Config::SetBaseOrCurrent(Config::MAIN_GC_LANGUAGE, i);
        Config::Save();
      }
      ImGui::PopID();
    }

    bool have_menu = false;
    for (const std::string dir : {"USA", "JAP", "EUR"})
    {
      const auto path = "/" + dir + "/" + GC_IPL;
      if (File::Exists(File::GetUserPath(D_GCUSER_IDX) + path) ||
          File::Exists(File::GetSysDirectory() + GC_SYS_DIR + path))
      {
        have_menu = true;
        break;
      }
    }

    bool skip_ipl = Config::Get(Config::MAIN_SKIP_IPL);
    if (ImGui::Checkbox("Skip Main Menu", &skip_ipl))
    {
      Config::SetBaseOrCurrent(Config::MAIN_SKIP_IPL, skip_ipl);
      Config::Save();
    }
    if (!have_menu)
    {
      ImGui::TextWrapped("No GameCube BIOS found. Put IPL ROMs in User/GC/<region>/IPL.bin to enable IPL functionality.");
    }
  }
  if (ImGui::CollapsingHeader("Load GameCube Main Menu"))
  {
    auto has_ipl = [](DiscIO::Region region) {
      std::string region_dir;
      switch (region)
      {
      case DiscIO::Region::NTSC_J:
        region_dir = "JAP";
        break;
      case DiscIO::Region::NTSC_U:
        region_dir = "USA";
        break;
      case DiscIO::Region::PAL:
        region_dir = "EUR";
        break;
      default:
        return false;
      }
      return File::Exists(File::GetUserPath(D_GCUSER_IDX) + "/" + region_dir + "/" + GC_IPL) ||
             File::Exists(File::GetSysDirectory() + GC_SYS_DIR + "/" + region_dir + "/" + GC_IPL);
    };
    auto BootGameCubeIPL = [state](DiscIO::Region region) {
      state->pending_boot_params = std::make_unique<BootParameters>(BootParameters::IPL{region});
    };


    if (ImGui::Button("NTSC-J (JAP)"))
    {
      if (has_ipl(DiscIO::Region::NTSC_J))
        BootGameCubeIPL(DiscIO::Region::NTSC_J);
    }
    if (ImGui::Button("NTSC-U (USA)"))
    {
      if (has_ipl(DiscIO::Region::NTSC_U))
        BootGameCubeIPL(DiscIO::Region::NTSC_U);
    }
    if (ImGui::Button("PAL (EUR)"))
    {
      if (has_ipl(DiscIO::Region::PAL))
        BootGameCubeIPL(DiscIO::Region::PAL);
    }
  }
}

void CreateWiiTab(UIState* state)
{
  static std::atomic<bool> import_in_progress{false};
  static std::atomic<bool> import_complete{false};
  static std::atomic<bool> import_canceled{false};
  static std::thread import_thread;
  static std::string import_status;
  static bool show_import_progress_modal = false;
  static bool show_import_result_modal = false;
  static bool import_success = false;
  static std::chrono::steady_clock::time_point import_begin_tp{};
  static std::atomic<bool> check_in_progress{false};
  static bool show_check_result_modal = false;
  static WiiUtils::NANDCheckResult last_check_result{};

  static WiiUtils::UpdateResult last_res = WiiUtils::UpdateResult::Cancelled;
  static std::atomic<bool> show_update_progress_modal{false};
  static std::atomic<bool> update_complete{false};
  static std::atomic<float> update_progress{0.0f};
  static std::thread update_thread;
  static std::atomic<bool> show_update_result_modal{false};
  static WiiUtils::UpdateResult async_res = WiiUtils::UpdateResult::Cancelled;
  if (PrimaryButton("Load Wii System Menu") && !state->controlsDisabled)
  {
    state->controlsDisabled = true;
    state->showSettingsWindow = false;
    state->pending_boot_params =
        std::make_unique<BootParameters>(BootParameters::NANDTitle{Titles::SYSTEM_MENU});
  }
  ImGui::TextWrapped("Boots the Wii System Menu from the NAND.");

  bool pal60 = Config::Get(Config::SYSCONF_PAL60);
  if (ImGui::Checkbox("Enable PAL60", &pal60))
  {
    Config::SetBaseOrCurrent(Config::SYSCONF_PAL60, pal60);
    Config::Save();
  }
  ImGui::TextWrapped("Enables PAL60 mode (480p) instead of standard PAL (576i) for PAL games.");

  bool wiiScreensaver = Config::Get(Config::SYSCONF_SCREENSAVER);
  if (ImGui::Checkbox("Enable Wii Screensaver", &wiiScreensaver))
  {
    Config::SetBaseOrCurrent(Config::SYSCONF_SCREENSAVER, wiiScreensaver);
    Config::Save();
  }
  ImGui::TextWrapped("Enables the Wii system screensaver after a period of inactivity.");

  bool enable_wiilink = Config::Get(Config::MAIN_WII_WIILINK_ENABLE);
  if (ImGui::Checkbox("Enable WiiConnect24 via WiiLink", &enable_wiilink))
  {
    Config::SetBaseOrCurrent(Config::MAIN_WII_WIILINK_ENABLE, enable_wiilink);
    Config::Save();
  }
  ImGui::TextWrapped("Enables WiiConnect24 features via WiiLink, such as the Wii Mail service.");

  const char* language_items[] = {"Japanese",
                                  "English",
                                  "German",
                                  "French",
                                  "Spanish",
                                  "Italian",
                                  "Dutch",
                                  "Simplified Chinese",
                                  "Traditional Chinese",
                                  "Korean"};
  auto lang = Config::Get(Config::SYSCONF_LANGUAGE);

  if (ImGui::CollapsingHeader("System Language"))
  {
    for (u32 i = 0; i < 10; i++)
    {
      ImGui::PushID(i);
      if (ImGui::RadioButton(language_items[i], i == lang))
      {
        Config::SetBaseOrCurrent(Config::SYSCONF_LANGUAGE, i);
        Config::Save();
      }
      ImGui::PopID();
    }
  }
  const char* sound_items[] = {"Mono", "Stereo", "Surround"};
  auto sound = Config::Get(Config::SYSCONF_SOUND_MODE);

  if (ImGui::CollapsingHeader("Sound"))
  {
    for (u32 i = 0; i < 3; i++)
    {
      ImGui::PushID(i);
      if (ImGui::RadioButton(sound_items[i], i == sound))
      {
        Config::SetBaseOrCurrent(Config::SYSCONF_SOUND_MODE, i);
        Config::Save();
      }
      ImGui::PopID();
    }
  }

  if (ImGui::CollapsingHeader("SD Card Settings"))
  {
    bool insert_sd_card = Config::Get(Config::MAIN_WII_SD_CARD);
    if (ImGui::Checkbox("Insert SD Card", &insert_sd_card))
    {
      Config::SetBaseOrCurrent(Config::MAIN_WII_SD_CARD, insert_sd_card);
      Config::Save();
    }
    if (ImGui::IsItemHovered())
    {
      ImGui::SetTooltip("Inserts a virtual SD card into the emulated Wii.\n"
                       "If unsure, leave this checked.");
    }

    bool allow_sd_writes = Config::Get(Config::MAIN_ALLOW_SD_WRITES);
    if (ImGui::Checkbox("Allow Writes to SD Card", &allow_sd_writes))
    {
      Config::SetBaseOrCurrent(Config::MAIN_ALLOW_SD_WRITES, allow_sd_writes);
      Config::Save();
    }

    std::string sd_card_path = Config::Get(Config::MAIN_WII_SD_CARD_IMAGE_PATH);
    char sd_card_path_buf[256];
    strncpy(sd_card_path_buf, sd_card_path.c_str(), sizeof(sd_card_path_buf));
    ImGui::InputText("SD Card Path", sd_card_path_buf, sizeof(sd_card_path_buf),
                     ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    if (ImGui::Button("Browse##SDCard"))
    {
      state->controlsDisabled = true;
      UWP::OpenDiscPicker();
      state->controlsDisabled = false;
    }

    bool sync_sd_folder = Config::Get(Config::MAIN_WII_SD_CARD_ENABLE_FOLDER_SYNC);
    if (ImGui::Checkbox("Automatically Sync with Folder", &sync_sd_folder))
    {
      Config::SetBaseOrCurrent(Config::MAIN_WII_SD_CARD_ENABLE_FOLDER_SYNC, sync_sd_folder);
      Config::Save();
    }
    ImGui::TextWrapped(
        "Synchronizes the SD Card with the SD Sync Folder when starting and ending emulation.");

    std::string sd_sync_folder = Config::Get(Config::MAIN_WII_SD_CARD_SYNC_FOLDER_PATH);
    char sd_sync_folder_buf[256];
    strncpy(sd_sync_folder_buf, sd_sync_folder.c_str(), sizeof(sd_sync_folder_buf));
    ImGui::InputText("SD Sync Folder", sd_sync_folder_buf, sizeof(sd_sync_folder_buf),
                     ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    if (ImGui::Button("Browse##SDSync"))
    {
      state->controlsDisabled = true;
      UWP::OpenGameFolderPicker([state](std::string path) {
        if (path != "")
        {
          Config::SetBaseOrCurrent(Config::MAIN_WII_SD_CARD_SYNC_FOLDER_PATH, path);
          Config::Save();
        }
        state->controlsDisabled = false;
      });
    }

    const char* sd_size_items[] = {"128 MB", "256 MB", "512 MB", "1 GB", "2 GB",
                                   "4 GB",   "8 GB",   "16 GB",  "32 GB"};
    u64 sd_size = Config::Get(Config::MAIN_WII_SD_CARD_FILESIZE);
    int sd_size_index = 0;
    for (size_t i = 0; i < IM_ARRAYSIZE(sd_size_items); ++i)
    {
      if (sd_size == (128 * 1024 * 1024) * (1ULL << i))
      {
        sd_size_index = static_cast<int>(i);
        break;
      }
    }
    if (ImGui::Combo("SD Card File Size", &sd_size_index, sd_size_items,
                     IM_ARRAYSIZE(sd_size_items)))
    {
      Config::SetBaseOrCurrent(Config::MAIN_WII_SD_CARD_FILESIZE,
                               (128 * 1024 * 1024) * (1ULL << sd_size_index));
      Config::Save();
    }
  }

  if (ImGui::CollapsingHeader("Wii Remote Settings"))
  {
    bool wiimote_motor = Config::Get(Config::SYSCONF_WIIMOTE_MOTOR);
    if (ImGui::Checkbox("Enable Rumble", &wiimote_motor))
    {
      Config::SetBaseOrCurrent(Config::SYSCONF_WIIMOTE_MOTOR, wiimote_motor);
      Config::Save();
    }

    const char* sensor_position_items[] = {"Top", "Bottom"};
    int sensor_position = Config::Get(Config::SYSCONF_SENSOR_BAR_POSITION);
    if (ImGui::Combo("Sensor Bar Position", &sensor_position, sensor_position_items,
                     IM_ARRAYSIZE(sensor_position_items)))
    {
      Config::SetBaseOrCurrent(Config::SYSCONF_SENSOR_BAR_POSITION, sensor_position);
      Config::Save();
    }
    if (ImGui::IsItemHovered())
    {
      ImGui::SetTooltip("Sets the position of the sensor bar relative to the screen.\n"
                       "If unsure, select Top.");
    }

    int ir_sensitivity = Config::Get(Config::SYSCONF_SENSOR_BAR_SENSITIVITY);
    if (ImGui::SliderInt("IR Sensitivity", &ir_sensitivity, 1, 5))
    {
      Config::SetBaseOrCurrent(Config::SYSCONF_SENSOR_BAR_SENSITIVITY, ir_sensitivity);
      Config::Save();
    }
    if (ImGui::IsItemHovered())
    {
      ImGui::SetTooltip("Adjusts the sensitivity of the Wii Remote's IR sensor.\n"
                       "Higher values increase sensitivity.\n"
                       "If unsure, leave this at 3.");
    }

    int speaker_volume = Config::Get(Config::SYSCONF_SPEAKER_VOLUME);
    if (ImGui::SliderInt("Speaker Volume", &speaker_volume, 0, 127))
    {
      Config::SetBaseOrCurrent(Config::SYSCONF_SPEAKER_VOLUME, speaker_volume);
      Config::Save();
    }
  }

  ImGui::Separator();
  if (ImGui::CollapsingHeader("Manage NAND"))
  {
    if (ImGui::Button("Import BootMii NAND Backup...") && !state->controlsDisabled &&
        !import_in_progress.load())
    {
      state->controlsDisabled = true;
      show_import_progress_modal = true;
      import_status = "Waiting for file picker...";

      UWP::OpenNANDBinPicker([&](std::string path) {
        if (path.empty())
        {
          import_status = "Import cancelled.";
          import_canceled.store(true);
          return;
        }
        import_in_progress.store(true);
        import_complete.store(false);
        import_status = "Importing...";
        import_begin_tp = std::chrono::steady_clock::now();
        import_thread = std::thread([path]() {
          try
          {
            DiscIO::NANDImporter importer;
            importer.ImportNANDBin(
                path,
                [] {},
                []() {
                  // Ask user for keys via picker
                  std::promise<std::string> p;
                  auto f = p.get_future();
                  UWP::OpenBootMiiKeysPicker([&p](std::string keys) { p.set_value(keys); });
                  return f.get();
                });
            import_success = true;
          }
          catch (...)
          {
            import_success = false;
          }
          import_in_progress.store(false);
          import_complete.store(true);
        });
      });
    }

    if (ImGui::Button("Check NAND...") && !state->controlsDisabled && !check_in_progress.load())
    {
      check_in_progress.store(true);
      std::thread([] {
        IOS::HLE::Kernel ios;
        last_check_result = WiiUtils::CheckNAND(ios);
        check_in_progress.store(false);
        }).detach();
      show_check_result_modal = true;
    }

    if (ImGui::Button("Extract Certificates from NAND") && !state->controlsDisabled)
    {
      const bool ok = DiscIO::NANDImporter().ExtractCertificates();
      ImGui::OpenPopup(ok ? "Certificates Extracted" : "Extraction Failed");
    }

    ImGui::SetNextWindowSizeConstraints(ImVec2(520.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::BeginPopupModal("Certificates Extracted", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
      ImGui::TextWrapped("Successfully extracted certificates from NAND.");
      if (ImGui::Button("OK"))
        ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(520.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::BeginPopupModal("Extraction Failed", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
      ImGui::TextWrapped("Failed to extract certificates from NAND.");
      if (ImGui::Button("OK"))
        ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
  }

  if (show_import_progress_modal)
  {
    ImGui::OpenPopup("Importing NAND backup...");
    show_import_progress_modal = false;
  }
  ImGui::SetNextWindowSize(ImVec2(420, 110), ImGuiCond_Always);
  if (ImGui::BeginPopupModal("Importing NAND backup...", nullptr, ImGuiWindowFlags_NoResize))
  {
    ImGui::TextWrapped("%s", import_status.c_str());
    float anim = fmodf(static_cast<float>(ImGui::GetTime()) * 0.5f, 1.0f);
    ImGui::ProgressBar(anim, ImVec2(-FLT_MIN, 0.0f));
    if (import_begin_tp.time_since_epoch().count() != 0)
    {
      auto secs = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - import_begin_tp)
                      .count();
      ImGui::Text("Elapsed: %llds", static_cast<long long>(secs));
    }
    if (import_canceled.load())
    {
      import_canceled.store(false);
      ImGui::CloseCurrentPopup();
      state->controlsDisabled = false;
      import_begin_tp = {};
      ImGui::OpenPopup("Import Cancelled");
    }
    if (import_complete.load())
    {
      if (import_thread.joinable())
        import_thread.join();
      state->controlsDisabled = false;
      ImGui::CloseCurrentPopup();
      import_begin_tp = {};
      show_import_result_modal = true;
    }
    ImGui::EndPopup();
  }
  ImGui::SetNextWindowSizeConstraints(ImVec2(520.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
  if (ImGui::BeginPopupModal("Import Cancelled", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    ImGui::TextWrapped("NAND import was cancelled.");
    if (ImGui::Button("OK"))
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  if (show_import_result_modal)
  {
    ImGui::OpenPopup(import_success ? "Import Complete" : "Import Failed");
    show_import_result_modal = false;
  }
  ImGui::SetNextWindowSizeConstraints(ImVec2(520.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
  if (ImGui::BeginPopupModal("Import Complete", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    ImGui::TextWrapped("Successfully imported NAND backup.");
    if (ImGui::Button("OK"))
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  ImGui::SetNextWindowSizeConstraints(ImVec2(520.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
  if (ImGui::BeginPopupModal("Import Failed", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    ImGui::TextWrapped("Failed to import NAND backup.");
    if (ImGui::Button("OK"))
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  if (show_check_result_modal)
  {
    ImGui::OpenPopup("NAND Check");
    show_check_result_modal = false;
  }
  ImGui::SetNextWindowSizeConstraints(ImVec2(520.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
  if (ImGui::BeginPopupModal("NAND Check", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    if (!last_check_result.bad)
    {
      const bool overfull = last_check_result.used_clusters_user > IOS::HLE::FS::USER_CLUSTERS ||
                            last_check_result.used_clusters_system > IOS::HLE::FS::SYSTEM_CLUSTERS;
      if (overfull)
        ImGui::TextWrapped("Your NAND contains more data than allowed. Wii software may behave incorrectly or not allow saving.");
      else
        ImGui::TextWrapped("No issues have been detected.");
    }
    else
    {
      ImGui::TextWrapped("The emulated NAND is damaged. Attempt repair?");
      if (ImGui::Button("Repair"))
      {
        IOS::HLE::Kernel ios;
        const bool ok = WiiUtils::RepairNAND(ios);
        ImGui::OpenPopup(ok ? "NAND Repaired" : "Repair Failed");
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel"))
        ImGui::CloseCurrentPopup();
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(520.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::BeginPopupModal("NAND Repaired", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
      ImGui::TextWrapped("The NAND has been repaired.");
      if (ImGui::Button("OK"))
        ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(520.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::BeginPopupModal("Repair Failed", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
      ImGui::TextWrapped("The NAND could not be repaired. It is recommended to back up your current data and start over with a fresh NAND.");
      if (ImGui::Button("OK"))
        ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
    if (ImGui::Button("OK"))
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  static bool show_online_update_modal = false;
  if (ImGui::Button("Online System Update"))
    show_online_update_modal = true;
  if (show_online_update_modal)
  {
    ImGui::OpenPopup("Online System Update");
    show_online_update_modal = false;
  }
  if (ImGui::BeginPopupModal("Online System Update", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    ImGui::Text("Select Region:");
    static int region_idx = 0;
    const char* region_items[] = {"JPN", "USA", "EUR", "KOR"};
    ImGui::Combo("Region", &region_idx, region_items, IM_ARRAYSIZE(region_items));
    if (ImGui::Button("OK"))
    {
      show_update_progress_modal = true;
      update_complete = false;
      update_progress = 0.0f;
      std::string region = region_items[region_idx];
      update_thread = std::thread([region]() {
        async_res = WiiUtils::DoOnlineUpdate(
            [](size_t done, size_t total, u64) {
              update_progress = total ? float(done) / float(total) : 0.0f;
              return true;
            },
            region);
        update_complete = true;
      });
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  if (show_update_progress_modal)
  {
    ImGui::OpenPopup("Updating...");
    show_update_progress_modal = false;
  }
  ImGui::SetNextWindowSize(ImVec2(600, 100), ImGuiCond_Once);
  if (ImGui::BeginPopupModal("Updating...", nullptr, ImGuiWindowFlags_NoResize))
  {
    ImGui::ProgressBar(update_progress.load(), ImVec2(-FLT_MIN, 0.0f));
    if (update_complete.load())
    {
      if (update_thread.joinable())
        update_thread.join();
      state->controlsDisabled = false;
      ImGui::CloseCurrentPopup();
      last_res = async_res;
      show_update_result_modal = true;
    }
    ImGui::EndPopup();
  }

  if (show_update_result_modal)
  {
    ImGui::OpenPopup("Update Result");
    show_update_result_modal = false;
  }
  ImGui::SetNextWindowSize(ImVec2(600, 150), ImGuiCond_Once);
  if (ImGui::BeginPopupModal("Update Result", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    const char* msg = "";
    switch (last_res)
    {
    case WiiUtils::UpdateResult::Succeeded:
      msg = "Update completed successfully.";
      break;
    case WiiUtils::UpdateResult::AlreadyUpToDate:
      msg = "Already up-to-date.";
      break;
    default:
      msg = "Update failed or cancelled.";
      break;
    }
    ImGui::TextWrapped("%s", msg);
    if (ImGui::Button("OK"))
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  ImGui::Separator();
  if (ImGui::Button("Install WAD"))
  {
    state->controlsDisabled = true;
    UWP::OpenWADPicker();
    state->controlsDisabled = false;
  }
}

void CreateAdvancedTab(UIState* state)
{
  const Core::CPUThreadGuard guard(Core::System::GetInstance());

  if (ImGui::CollapsingHeader("CPU Options"))
  {
#ifdef WINRT_XBOX
    const char* cpu_cores[] = {"Interpreter", "JIT64", "Cached Interpreter"};
    const PowerPC::CPUCore cpu_core_values[] = {PowerPC::CPUCore::Interpreter,
                                                PowerPC::CPUCore::JIT64,
                                                PowerPC::CPUCore::CachedInterpreter};
    const int cpu_cores_count = 3;
#else
    const char* cpu_cores[] = {"Interpreter", "JIT64", "JITARM64", "Cached Interpreter"};
    const PowerPC::CPUCore cpu_core_values[] = {PowerPC::CPUCore::Interpreter,
                                                PowerPC::CPUCore::JIT64, PowerPC::CPUCore::JITARM64,
                                                PowerPC::CPUCore::CachedInterpreter};
    const int cpu_cores_count = 4;
#endif
    PowerPC::CPUCore config_core = Config::Get(Config::MAIN_CPU_CORE);
    int combo_index = 0;
    for (int i = 0; i < cpu_cores_count; ++i)
    {
      if (cpu_core_values[i] == config_core)
      {
        combo_index = i;
        break;
      }
    }
    if (ImGui::Combo("CPU Emulation Engine", &combo_index, cpu_cores, cpu_cores_count))
    {
      Config::SetBaseOrCurrent(Config::MAIN_CPU_CORE, cpu_core_values[combo_index]);
      Config::Save();
    }

    bool mmuEnable = Config::Get(Config::MAIN_MMU);
    if (ImGui::Checkbox("Enable MMU", &mmuEnable))
    {
      Config::SetBaseOrCurrent(Config::MAIN_MMU, mmuEnable);
      Config::Save();
    }
    ImGui::TextWrapped("Enables the Memory Management Unit, needed for some games. (ON = Compatible, "
                       "OFF = Fast)\n\nIf unsure, leave this unchecked.");

    bool pauseOnPanic = Config::Get(Config::MAIN_PAUSE_ON_PANIC);
    if (ImGui::Checkbox("Pause on Panic", &pauseOnPanic))
    {
      Config::SetBaseOrCurrent(Config::MAIN_PAUSE_ON_PANIC, pauseOnPanic);
      Config::Save();
    }
    ImGui::TextWrapped("Pauses the emulation if a Read/Write or Unknown Instruction panic "
                       "occurs.\nEnabling will affect performance.\nThe performance impact is the "
                       "same as having Enable MMU on.\n\nIf unsure, leave this unchecked.");

    bool accurateCPUCache = Config::Get(Config::MAIN_ACCURATE_CPU_CACHE);
    if (ImGui::Checkbox("Enable Write-Back Cache (slow)", &accurateCPUCache))
    {
      Config::SetBaseOrCurrent(Config::MAIN_ACCURATE_CPU_CACHE, accurateCPUCache);
      Config::Save();
    }
    ImGui::TextWrapped("Enables emulation of the CPU write-back cache.\nEnabling will have a "
                       "significant impact on performance.\nThis should be left disabled unless "
                       "absolutely needed.\n\nIf unsure, leave this unchecked.");

    if (ImGui::CollapsingHeader("Advanced CPU Options"))
    {
      bool jitFollowBranch = Config::Get(Config::MAIN_JIT_FOLLOW_BRANCH);
      if (ImGui::Checkbox("JIT Follow Branch", &jitFollowBranch))
      {
        Config::SetBaseOrCurrent(Config::MAIN_JIT_FOLLOW_BRANCH, jitFollowBranch);
        Config::Save();
      }
      ImGui::TextWrapped("Enables JIT to follow branches for optimization. Can improve performance "
                         "in some cases.\n\nIf unsure, leave this unchecked.");

      bool fastmem = Config::Get(Config::MAIN_FASTMEM);
      if (ImGui::Checkbox("Enable Fastmem", &fastmem))
      {
        Config::SetBaseOrCurrent(Config::MAIN_FASTMEM, fastmem);
        Config::Save();
      }
      ImGui::TextWrapped("Enables faster memory access by using virtual memory mapping. Can improve "
                         "performance significantly.\n\nIf unsure, leave this checked.");

      bool largeEntryPoints = Config::Get(Config::MAIN_LARGE_ENTRY_POINTS_MAP);
      if (ImGui::Checkbox("Large Entry Points Map", &largeEntryPoints))
      {
        Config::SetBaseOrCurrent(Config::MAIN_LARGE_ENTRY_POINTS_MAP, largeEntryPoints);
        Config::Save();
      }
      ImGui::TextWrapped("Uses a larger map for JIT entry points. Can improve performance for games "
                         "with many functions.\n\nIf unsure, leave this unchecked.");
    }
  }

  if (ImGui::CollapsingHeader("Timing"))
  {
    bool syncOnSkipIdle = Config::Get(Config::MAIN_SYNC_ON_SKIP_IDLE);
    if (ImGui::Checkbox("Sync on Skip Idle", &syncOnSkipIdle))
    {
      Config::SetBaseOrCurrent(Config::MAIN_SYNC_ON_SKIP_IDLE, syncOnSkipIdle);
      Config::Save();
    }
    ImGui::TextWrapped("Synchronizes the GPU thread with the CPU thread when the CPU is idle.\nThis "
                       "helps prevent desynchronization between the CPU and GPU, which can cause "
                       "visual glitches or crashes.\n\nIf unsure, leave this checked.");

    bool emulateDiscSpeed = !Config::Get(Config::MAIN_FAST_DISC_SPEED);
    if (ImGui::Checkbox("Emulate Disc Speed", &emulateDiscSpeed))
    {
      Config::SetBaseOrCurrent(Config::MAIN_FAST_DISC_SPEED, !emulateDiscSpeed);
      Config::Save();
    }
    ImGui::TextWrapped(
        "When checked, the emulator uses normal disc speed (ON = Stock, OFF = Fast).\nDisabling "
        "emulation accelerates the disc transfer rate, removing loading times but may cause issues "
        "with games that rely on precise timing.\n\nIf unsure, leave this checked.");

    bool correctTimeDrift = Config::Get(Config::MAIN_CORRECT_TIME_DRIFT);
    if (ImGui::Checkbox("Correct Time Drift", &correctTimeDrift))
    {
      Config::SetBaseOrCurrent(Config::MAIN_CORRECT_TIME_DRIFT, correctTimeDrift);
      Config::Save();
    }
    ImGui::TextWrapped("Allow the emulated console to run fast after stutters, pursuing accurate "
                       "overall elapsed time unless paused or speed-adjusted.\n\nThis may be useful "
                       "for internet play.\n\nIf unsure, leave this unchecked.");

    if (ImGui::CollapsingHeader("Advanced Timing Options"))
    {
      int timingVariance = Config::Get(Config::MAIN_TIMING_VARIANCE);
      if (ImGui::SliderInt("Timing Variance (ms)", &timingVariance, 0, 100))
      {
        Config::SetBaseOrCurrent(Config::MAIN_TIMING_VARIANCE, timingVariance);
        Config::Save();
      }
      ImGui::TextWrapped("Simulates timing variance in the emulated system. Higher values add more "
                         "randomness to timing.\n\nIf unsure, leave this at 0.");
    }
  }

  if (ImGui::CollapsingHeader("Clock Override"))
  {
    ImGui::Text("WARNING: Changing this from the default (1.0 = 100%) can and will break\ngames "
                "and cause glitches. Do so at your own risk. \nPlease do not report bugs that "
                "occur with a non-default clock. \nThis is not a magical performance slider!");

    bool overclockEnable = Config::Get(Config::MAIN_OVERCLOCK_ENABLE);
    if (ImGui::Checkbox("Enable Emulated CPU Clock Override", &overclockEnable))
    {
      Config::SetBaseOrCurrent(Config::MAIN_OVERCLOCK_ENABLE, overclockEnable);
      Config::Save();
    }

    if (overclockEnable)
    {
      float clockOverride = Config::Get(Config::MAIN_OVERCLOCK);
      if (ImGui::SliderFloat("Emulated CPU Clock Speed Override", &clockOverride, 0.01f, 4.0f))
      {
        Config::SetBaseOrCurrent(Config::MAIN_OVERCLOCK, clockOverride);
        Config::Save();
      }

      int percent = static_cast<int>(std::round(clockOverride * 100.0f));
      int clock = static_cast<int>(std::round(clockOverride * 486.0f));
      ImGui::Text("%d%% (%d MHz)", percent, clock);
    }

    ImGui::TextWrapped(
        "Adjusts the emulated CPU's clock rate.\n\nOn games that have an unstable frame rate "
        "despite full emulation speed, higher values can improve their performance, requiring a "
        "powerful device. Lower values reduce the emulated console's performance, but improve the "
        "emulation speed.\n\nWARNING: Changing this from the default (100%) can and will break "
        "games and cause glitches. Do so at your own risk. Please do not report bugs that occur "
        "with a non-default clock.\n\nIf unsure, leave this unchecked.");
  }

  if (ImGui::CollapsingHeader("VBI Frequency Override"))
  {
    ImGui::Text("WARNING: Changing this from the default (1.0 = 100%) can and will break\ngames "
                "and cause glitches. Do so at your own risk. \nPlease do not report bugs that "
                "occur with a non-default frequency.");

    bool vbiOverclockEnable = Config::Get(Config::MAIN_VI_OVERCLOCK_ENABLE);
    if (ImGui::Checkbox("Enable VBI Frequency Override", &vbiOverclockEnable))
    {
      Config::SetBaseOrCurrent(Config::MAIN_VI_OVERCLOCK_ENABLE, vbiOverclockEnable);
      Config::Save();
    }

    if (vbiOverclockEnable)
    {
      float vbiOverclock = Config::Get(Config::MAIN_VI_OVERCLOCK);
      if (ImGui::SliderFloat("VBI Frequency Override", &vbiOverclock, 0.01f, 5.0f))
      {
        Config::SetBaseOrCurrent(Config::MAIN_VI_OVERCLOCK, vbiOverclock);
        Config::Save();
      }

      int percent = static_cast<int>(std::round(vbiOverclock * 100.0f));
      float vps = 59.94f * vbiOverclock;
      ImGui::Text("%d%% (%.2f VPS)", percent, vps);
    }

    ImGui::TextWrapped(
        "Adjusts the VBI frequency. Also adjusts the emulated CPU's clock rate, to keep it "
        "relatively the same.\n\nMakes games run at a different frame rate, making the "
        "emulation less demanding when lowered, or improving smoothness when increased. This "
        "may affect gameplay speed, as it is often tied to the frame rate.\n\nWARNING: "
        "Changing this from the default (100%) can and will break games and cause glitches. "
        "Do so at your own risk. Please do not report bugs that occur with a non-default "
        "frequency.\n\nIf unsure, leave this unchecked.");
  }

  if (ImGui::CollapsingHeader("Memory Override"))
  {
    bool ramOverrideEnable = Config::Get(Config::MAIN_RAM_OVERRIDE_ENABLE);
    if (ImGui::Checkbox("Enable Emulated Memory Size Override", &ramOverrideEnable))
    {
      Config::SetBaseOrCurrent(Config::MAIN_RAM_OVERRIDE_ENABLE, ramOverrideEnable);
      Config::Save();
    }

    if (ramOverrideEnable)
    {
      u32 mem1Size = Config::Get(Config::MAIN_MEM1_SIZE) / 0x100000;
      if (ImGui::SliderInt("MEM1 Size (MB)", (int*)&mem1Size, 24, 64))
      {
        Config::SetBaseOrCurrent(Config::MAIN_MEM1_SIZE, mem1Size * 0x100000);
        Config::Save();
      }

      u32 mem2Size = Config::Get(Config::MAIN_MEM2_SIZE) / 0x100000;
      if (ImGui::SliderInt("MEM2 Size (MB)", (int*)&mem2Size, 64, 128))
      {
        Config::SetBaseOrCurrent(Config::MAIN_MEM2_SIZE, mem2Size * 0x100000);
        Config::Save();
      }
    }

    ImGui::TextWrapped(
        "Adjusts the amount of RAM in the emulated console.\n\n"
        "WARNING: Enabling this will completely break many games. Only a small number "
        "of games can benefit from this.");
  }

  if (ImGui::CollapsingHeader("Custom RTC Options"))
  {
    bool customRTCEnable = Config::Get(Config::MAIN_CUSTOM_RTC_ENABLE);
    if (ImGui::Checkbox("Enable Custom RTC", &customRTCEnable))
    {
      Config::SetBaseOrCurrent(Config::MAIN_CUSTOM_RTC_ENABLE, customRTCEnable);
      Config::Save();
    }

    if (customRTCEnable)
    {
      u64 customRTC = Config::Get(Config::MAIN_CUSTOM_RTC_VALUE);
      time_t current_time = customRTC;
      struct tm* timeinfo = gmtime(&current_time);

      // Create a date/time picker
      int year = timeinfo->tm_year + 1900;
      int month = timeinfo->tm_mon + 1;
      int day = timeinfo->tm_mday;
      int hour = timeinfo->tm_hour;
      int minute = timeinfo->tm_min;
      int second = timeinfo->tm_sec;

      bool date_changed = false;
      date_changed |= ImGui::InputInt("Year", &year, 1, 100);
      date_changed |= ImGui::InputInt("Month", &month, 1, 12);
      date_changed |= ImGui::InputInt("Day", &day, 1, 31);
      date_changed |= ImGui::InputInt("Hour", &hour, 1, 23);
      date_changed |= ImGui::InputInt("Minute", &minute, 1, 59);
      date_changed |= ImGui::InputInt("Second", &second, 1, 59);

      if (date_changed)
      {
        year = std::clamp(year, 2000, 2099);
        month = std::clamp(month, 1, 12);
        day = std::clamp(day, 1, 31);
        hour = std::clamp(hour, 0, 23);
        minute = std::clamp(minute, 0, 59);
        second = std::clamp(second, 0, 59);

        struct tm new_time = {};
        new_time.tm_year = year - 1900;
        new_time.tm_mon = month - 1;
        new_time.tm_mday = day;
        new_time.tm_hour = hour;
        new_time.tm_min = minute;
        new_time.tm_sec = second;
        new_time.tm_isdst = -1;

        time_t new_time_t = _mkgmtime(&new_time);
        if (new_time_t != -1)
        {
          Config::SetBaseOrCurrent(Config::MAIN_CUSTOM_RTC_VALUE, static_cast<u64>(new_time_t));
          Config::Save();
        }
      }
    }
  }
}

void CreatePathsTab(UIState* state)
{
  ImGui::Text("Game Folders");
  if (ImGui::BeginListBox("##folders"))
  {
    for (auto path : m_paths)
    {
      if (ImGui::Selectable(path.c_str()))
      {
        state->selectedPath = path;
      }
    }
    ImGui::EndListBox();
  }

  if (state->selectedPath == "")
  {
    ImGui::BeginDisabled();
  }

  if (ImGui::Button("Remove Path"))
  {
    if (state->selectedPath != "")
    {
      m_paths.erase(std::remove(m_paths.begin(), m_paths.end(), state->selectedPath),
                    m_paths.end());
      Config::SetIsoPaths(m_paths);
      Config::Save();
    }
  }

  if (state->selectedPath == "")
  {
    ImGui::EndDisabled();
  }

  ImGui::SameLine();
  if (ImGui::Button("Add Path"))
  {
    state->controlsDisabled = true;
    UWP::OpenGameFolderPicker([state](std::string path) {
      if (path != "")
      {
        if (!UWP::TestPathPermissions(path))
        {
          m_show_path_warning = true;
        }
        else
        {
          m_paths.emplace_back(path);
          Config::SetIsoPaths(m_paths);
          Config::Save();
        }
      }

      state->controlsDisabled = false;
    });
  }

  ImGui::Spacing();
  ImGui::Separator();

  if (ImGui::Button("Set Dolphin User Folder Location"))
  {
    state->controlsDisabled = true;
    UWP::OpenNewUserPicker([=](std::string path) {
      if (path != "")
      {
        if (!UWP::TestPathPermissions(path))
        {
          m_show_path_warning = true;
        }
      }

      state->controlsDisabled = false;
    });

    UICommon::Shutdown();
    UICommon::SetUserDirectory(UWP::GetUserLocation());
    UICommon::CreateDirectories();
    UICommon::Init();
  }

  if (ImGui::Button("Reset Dolphin User Folder Location"))
  {
    UWP::ResetUserLocation();

    UICommon::Shutdown();
    UICommon::SetUserDirectory(UWP::GetUserLocation());
    UICommon::CreateDirectories();
    UICommon::Init();
  }

  if (m_show_path_warning)
  {
    ImGui::OpenPopup("Warning");
    m_show_path_warning = false;
    ImGui::SetNextWindowSize(ImVec2(950 * m_frame_scale, 80 * m_frame_scale));
  }

  if (ImGui::BeginPopupModal("Warning"))
  {
    ImGui::TextWrapped("The folder path you have selected is not writable! Please check that you "
                       "have the correct permissions for the folder you have selected.");
    ImGui::Separator();
    if (ImGui::Button("OK"))
    {
      ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextWrapped("Note: Please remember to do your USB filesystem setup, or paths to "
                     "your USB will not work properly!");
}

void CreateAchievementsTab(UIState* state)
{
  const Core::CPUThreadGuard guard(Core::System::GetInstance());

#ifdef USE_RETRO_ACHIEVEMENTS
  static bool integration_enabled = Config::Get(Config::RA_ENABLED);
  static bool hardcore_enabled = Config::Get(Config::RA_HARDCORE_ENABLED);
  static bool unofficial_enabled = Config::Get(Config::RA_UNOFFICIAL_ENABLED);
  static bool encore_enabled = Config::Get(Config::RA_ENCORE_ENABLED);
  static bool spectator_enabled = Config::Get(Config::RA_SPECTATOR_ENABLED);
  static bool discord_presence_enabled = Config::Get(Config::RA_DISCORD_PRESENCE_ENABLED);
  static bool progress_enabled = Config::Get(Config::RA_PROGRESS_ENABLED);
  static char username[256] = "";
  static char password[256] = "";
  static bool show_password = false;
  static std::string login_error;

  const bool is_running = Core::GetState(Core::System::GetInstance()) != Core::State::Uninitialized;
  const bool logged_in = !Config::Get(Config::RA_API_TOKEN).empty();

  if (ImGui::Checkbox("Enable RetroAchievements.org Integration", &integration_enabled))
  {
    Config::SetBaseOrCurrent(Config::RA_ENABLED, integration_enabled);
    Config::Save();
  }
  if (ImGui::IsItemHovered())
  {
    ImGui::SetTooltip(
        "Enable integration with RetroAchievements for earning achievements and competing in "
        "leaderboards.\n\n"
        "Must log in with a RetroAchievements account to use. Dolphin does not save your "
        "password locally and uses an API token to maintain login.");
  }

  if (integration_enabled)
  {
    if (!logged_in)
    {
      ImGui::Text("Username");
      ImGui::SameLine();
      if (ImGui::Button("Edit Username"))
      {
        UWP::ShowKeyboard();
        ImGui::SetKeyboardFocusHere();
      }
      ImGui::SameLine();
      if (ImGui::InputText("##username", username, sizeof(username),
                           ImGuiInputTextFlags_EnterReturnsTrue))
      {
        UWP::HideKeyboard();
        ImGui::SetKeyboardFocusHere(-1);
      }

      ImGui::Spacing();

      ImGui::Text("Password");
      ImGui::SameLine();
      if (ImGui::Button("Edit Password"))
      {
        UWP::ShowKeyboard();
        ImGui::SetKeyboardFocusHere();
      }
      ImGui::SameLine();
      ImGuiInputTextFlags pwd_flags =
          ImGuiInputTextFlags_EnterReturnsTrue | (show_password ? 0 : ImGuiInputTextFlags_Password);
      if (ImGui::InputText("##password", password, sizeof(password), pwd_flags))
      {
        UWP::HideKeyboard();
        show_password = !show_password;
        ImGui::SetKeyboardFocusHere(-1);
      }

      ImGui::BeginDisabled(is_running);
      if (ImGui::Button("Log In"))
      {
#ifdef _WIN32
        OutputDebugStringA(
            ("RetroAchievements: Login attempt for user: " + std::string(username) + "\n").c_str());
#endif

        if (std::string(username).empty())
        {
          login_error = "Username cannot be empty";
#ifdef _WIN32
          OutputDebugStringA(("RetroAchievements Login Error: " + login_error + "\n").c_str());
#endif
        }
        else if (std::string(password).empty())
        {
          login_error = "Password cannot be empty";
#ifdef _WIN32
          OutputDebugStringA(("RetroAchievements Login Error: " + login_error + "\n").c_str());
#endif
        }
        else
        {
          Config::SetBaseOrCurrent(Config::RA_USERNAME, username);

          try
          {
            std::string before_token = Config::Get(Config::RA_API_TOKEN);

            AchievementManager::GetInstance().Login(std::string(password));

            std::string after_token = Config::Get(Config::RA_API_TOKEN);

            if (after_token == before_token && after_token.empty())
            {
              login_error = "Authentication failed. Please check your credentials.";
#ifdef _WIN32
              OutputDebugStringA(("RetroAchievements Login Error: " + login_error + "\n").c_str());
#endif
            }
            else
            {
              login_error.clear();
#ifdef _WIN32
              OutputDebugStringA("RetroAchievements: Login successful\n");
#endif
            }
          }
          catch (const std::exception& e)
          {
            login_error = std::string("Exception during login: ") + e.what();
#ifdef _WIN32
            OutputDebugStringA(("RetroAchievements Login Error: " + login_error + "\n").c_str());
#endif
          }
        }
        memset(password, 0, sizeof(password));
      }
      ImGui::EndDisabled();

      if (!login_error.empty())
      {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255));
        ImGui::Text("%s", login_error.c_str());
        ImGui::PopStyleColor();
      }
    }
    else
    {
      auto& instance = AchievementManager::GetInstance();
      const auto* user_info = rc_client_get_user_info(instance.GetClient());

      if (user_info)
      {
        ImGui::BeginGroup();

        // User name and points
        ImGui::Text("%s", user_info->display_name);
        ImGui::Text("%u points", user_info->score);

        // User badge/icon
        const AchievementManager::Badge& player_badge = instance.GetPlayerBadge();
        if (!player_badge.data.empty())
        {
          static std::shared_ptr<AbstractTexture> profile_texture;
          static std::vector<u8> last_badge_data;

          const std::vector<u8> badge_data(player_badge.data.data(),
                                           player_badge.data.data() + player_badge.data.size());

          if (last_badge_data != badge_data)
          {
            last_badge_data = badge_data;

            TextureConfig config(player_badge.width, player_badge.height, 1, 1, 1,
                                 AbstractTextureFormat::RGBA8, 0, AbstractTextureType::Texture_2D);

            profile_texture = g_gfx->CreateTexture(config, "RetroAchievements profile picture");
            if (profile_texture)
            {
              profile_texture->Load(0, player_badge.width, player_badge.height, player_badge.width,
                                    player_badge.data.data(),
                                    sizeof(u8) * 4 * player_badge.width * player_badge.height);
            }
          }

          if (profile_texture)
          {
            ImGui::Image((ImTextureID)(intptr_t)profile_texture.get(), ImVec2(64, 64));
          }
          else
          {
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImGui::GetCursorScreenPos(),
                ImVec2(ImGui::GetCursorScreenPos().x + 64, ImGui::GetCursorScreenPos().y + 64),
                IM_COL32(50, 150, 255, 255));

            ImGui::Dummy(ImVec2(64, 64));
          }
        }

        ImGui::EndGroup();

        if (ImGui::Button("Log Out"))
        {
#ifdef _WIN32
          OutputDebugStringA("RetroAchievements: User logged out\n");
#endif
          AchievementManager::GetInstance().Logout();
          Config::SetBaseOrCurrent(Config::RA_API_TOKEN, "");
          Config::Save();
        }
      }
      else
      {
        if (ImGui::Button("Log Out"))
        {
#ifdef _WIN32
          OutputDebugStringA("RetroAchievements: User logged out\n");
#endif
          AchievementManager::GetInstance().Logout();
          Config::SetBaseOrCurrent(Config::RA_API_TOKEN, "");
          Config::Save();
        }
      }

      if (instance.IsGameLoaded())
      {
        ImGui::Separator();
        rc_client_user_game_summary_t game_summary;
        rc_client_get_user_game_summary(instance.GetClient(), &game_summary);

        ImGui::Text("%s", instance.GetGameDisplayName().data());
        ImGui::Text("Unlocked %u/%u achievements worth %u/%u points",
                    game_summary.num_unlocked_achievements, game_summary.num_core_achievements,
                    game_summary.points_unlocked, game_summary.points_core);

        float progress =
            game_summary.num_core_achievements > 0 ?
                (float)game_summary.num_unlocked_achievements / game_summary.num_core_achievements :
                0.0f;
        ImGui::ProgressBar(progress, ImVec2(-1, 0),
                           (std::to_string(game_summary.num_unlocked_achievements) + "/" +
                            std::to_string(game_summary.num_core_achievements))
                               .c_str());

        const auto& rich_presence = instance.GetRichPresence();
        if (rich_presence[0] != '\0')
        {
          ImGui::TextWrapped("%s", rich_presence.data());
        }
      }
    }
    ImGui::Separator();
    ImGui::Text("Function Settings");

    ImGui::BeginDisabled(!logged_in || (is_running && !hardcore_enabled));
    if (ImGui::Checkbox("Enable Hardcore Mode", &hardcore_enabled))
    {
      Config::SetBaseOrCurrent(Config::RA_HARDCORE_ENABLED, hardcore_enabled);
      Config::Save();
    }
    if (ImGui::IsItemHovered())
    {
      ImGui::SetTooltip(
          "Enable Hardcore Mode on RetroAchievements.\n\n"
          "Hardcore Mode is intended to provide an experience as close to gaming on the original "
          "hardware as possible. RetroAchievements rankings are primarily oriented towards "
          "Hardcore "
          "points (Softcore points are tracked but not as heavily emphasized) and leaderboards "
          "require Hardcore Mode to be on.\n\n"
          "To ensure this experience, the following features will be disabled:\n"
          "- Loading states (saving states is allowed)\n"
          "- Emulator speeds below 100% (frame advance disabled, turbo allowed)\n"
          "- Cheats\n"
          "- Memory patches (file patches allowed)\n"
          "- Debug UI\n"
          "- Freelook\n\n"
          "This cannot be turned on while a game is playing.\n"
          "Close your current game before enabling.");
    }
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!logged_in);
    if (ImGui::Checkbox("Enable Unofficial Achievements", &unofficial_enabled))
    {
      Config::SetBaseOrCurrent(Config::RA_UNOFFICIAL_ENABLED, unofficial_enabled);
      Config::Save();
    }
    if (ImGui::IsItemHovered())
    {
      ImGui::SetTooltip(
          "Enable unlocking unofficial achievements as well as official achievements.\n\n"
          "Unofficial achievements may be optional or unfinished achievements that have not been "
          "deemed official by RetroAchievements and may be useful for testing or simply for "
          "fun.\n\n"
          "Setting takes effect on next game load.");
    }

    if (ImGui::Checkbox("Enable Encore Mode", &encore_enabled))
    {
      Config::SetBaseOrCurrent(Config::RA_ENCORE_ENABLED, encore_enabled);
      Config::Save();
    }
    if (ImGui::IsItemHovered())
    {
      ImGui::SetTooltip(
          "Enable unlocking achievements in Encore Mode.\n\n"
          "Encore Mode re-enables achievements the player has already unlocked on the site so that "
          "the player will be notified if they meet the unlock conditions again, useful for custom "
          "speedrun criteria or simply for fun.\n\n"
          "Setting takes effect on next game load.");
    }

    if (ImGui::Checkbox("Enable Spectator Mode", &spectator_enabled))
    {
      Config::SetBaseOrCurrent(Config::RA_SPECTATOR_ENABLED, spectator_enabled);
      Config::Save();
    }
    if (ImGui::IsItemHovered())
    {
      ImGui::SetTooltip(
          "Enable unlocking achievements in Spectator Mode.\n\n"
          "While in Spectator Mode, achievements and leaderboards will be processed and displayed "
          "on screen, but will not be submitted to the server.\n\n"
          "If this is on at game launch, it will not be turned off until game close, because a "
          "RetroAchievements session will not be created.\n\n"
          "If this is off at game launch, it can be toggled freely while the game is running.");
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::Text("Display Settings");

#ifndef WINRT_XBOX  // We cannot use Discord RPC on Xbox as you cannot launch the Discord Win32 app.
#ifdef USE_DISCORD_PRESENCE
    ImGui::BeginDisabled(!Config::Get(Config::MAIN_USE_DISCORD_PRESENCE));
    if (ImGui::Checkbox("Enable Discord Presence", &discord_presence_enabled))
    {
      Config::SetBaseOrCurrent(Config::RA_DISCORD_PRESENCE_ENABLED, discord_presence_enabled);
      Config::Save();
    }
    if (ImGui::IsItemHovered())
    {
      ImGui::SetTooltip("Use RetroAchievements rich presence in your Discord status.\n\n"
                        "Show Current Game on Discord must be enabled.");
    }
    ImGui::EndDisabled();
#endif
#endif

    if (ImGui::Checkbox("Enable Progress Notifications", &progress_enabled))
    {
      Config::SetBaseOrCurrent(Config::RA_PROGRESS_ENABLED, progress_enabled);
      Config::Save();
    }
    if (ImGui::IsItemHovered())
    {
      ImGui::SetTooltip(
          "Enable progress notifications on achievements.\n\n"
          "Displays a brief popup message whenever the player makes progress on an achievement "
          "that tracks an accumulated value, such as 60 out of 120 stars.");
    }
  }
#else
  ImGui::TextWrapped("RetroAchievements support is not enabled in this build.");
#endif
}

void DrawSettingsMenu(UIState* state, float frame_scale)
{
  if (ImGui::BeginListBox("##tabs", ImVec2(100 * frame_scale, -1)))
  {
    if (ImGui::Selectable("General", state->selectedTab == General))
    {
      state->selectedTab = General;
    }
    if (ImGui::Selectable("Interface", state->selectedTab == Interface))
    {
      state->selectedTab = Interface;
    }
    if (ImGui::Selectable("Graphics", state->selectedTab == Graphics))
    {
      state->selectedTab = Graphics;
    }
    if (ImGui::Selectable("Audio", state->selectedTab == Audio))
    {
      state->selectedTab = Audio;
    }
    if (ImGui::Selectable("Controls", state->selectedTab == Controls))
    {
      state->selectedTab = Controls;
    }
    if (ImGui::Selectable("GameCube", state->selectedTab == GC))
    {
      state->selectedTab = GC;
    }
    if (ImGui::Selectable("Wii", state->selectedTab == Wii))
    {
      state->selectedTab = Wii;
    }
    if (ImGui::Selectable("Advanced", state->selectedTab == Advanced))
    {
      state->selectedTab = Advanced;
    }
    if (ImGui::Selectable("Paths", state->selectedTab == Paths))
    {
      state->selectedTab = Paths;
    }
    if (ImGui::Selectable("Achievements", state->selectedTab == Achievements))
    {
      state->selectedTab = Achievements;
    }
    if (ImGui::Selectable("About", state->selectedTab == About))
    {
      state->selectedTab = About;
    }

    ImGui::EndListBox();
  }

  ImGui::SameLine();
  if (ImGui::BeginChild("##tabview", ImVec2(-1, -1), true))
  {
    switch (state->selectedTab)
    {
    case General:
      CreateGeneralTab(state);
      break;
    case Interface:
      CreateInterfaceTab(state);
      break;
    case Graphics:
      CreateGraphicsTab(state);
      break;
    case Audio:
      CreateAudioTab(state);
      break;
    case Controls:
      CreateControlsTab(state);
      break;
    case GC:
      CreateGameCubeTab(state);
      break;
    case Wii:
      CreateWiiTab(state);
      break;
    case Paths:
      CreatePathsTab(state);
      break;
    case Advanced:
      CreateAdvancedTab(state);
      break;
    case Achievements:
      CreateAchievementsTab(state);
      break;
    case About:
      ImGui::TextWrapped(
          "Dolphin Emulator on UWP - Version 1.1.9.1 Beta (Based on Dolphin 2506-340)\n\n"
          "This is a fork of Dolphin Emulator introducing Xbox support with a big picture "
          "frontend\n\n"
          "Credits:\n\n"
          "Dolphin: for their amazing work on Dolphin Emulator\n"
          "SirMangler: for their amazing work on porting Dolphin for UWP\n"
          "worleydl: For his oct2024-rebase branch (it helped so much)\n"
          "SirManglers Ko-Fi: https://ko-fi.com/sirmangler\n"
          "Sterns Ko-Fi: https://ko-fi.com/stern\n\n"
          "Dolphin Emulator is licensed under GPLv2+ and is not associated with Nintendo.");
      break;
    }

    ImGui::EndChild();
  }
}
} // namespace ImGuiFrontend
