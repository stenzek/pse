// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "sdl_key_names.h"

#include "scmversion/scmversion.h"

#include "core/achievements.h"
#include "core/bus.h"
#include "core/controller.h"
#include "core/fullscreen_ui.h"
#include "core/game_list.h"
#include "core/gpu.h"
#include "core/gpu_backend.h"
#include "core/gpu_thread.h"
#include "core/host.h"
#include "core/imgui_overlays.h"
#include "core/settings.h"
#include "core/system.h"
#include "core/system_private.h"

#include "util/gpu_device.h"
#include "util/imgui_fullscreen.h"
#include "util/imgui_manager.h"
#include "util/ini_settings_interface.h"
#include "util/input_manager.h"
#include "util/platform_misc.h"
#include "util/sdl_input_source.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include "common/assert.h"
#include "common/crash_handler.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"
#include "common/threading.h"

#include "IconsEmoji.h"
#include "fmt/format.h"

#include <SDL.h>
#include <SDL_syswm.h>
#include <cinttypes>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <thread>

LOG_CHANNEL(Host);

namespace MiniHost {

static constexpr u32 DEFAULT_WINDOW_WIDTH = 1280;
static constexpr u32 DEFAULT_WINDOW_HEIGHT = 720;
static constexpr float DEFAULT_WINDOW_DPI = 96.0f;

static constexpr u32 SETTINGS_VERSION = 3;
static constexpr auto CPU_THREAD_POLL_INTERVAL =
  std::chrono::milliseconds(8); // how often we'll poll controllers when paused

static bool ParseCommandLineParametersAndInitializeConfig(int argc, char* argv[],
                                                          std::optional<SystemBootParameters>& autoboot);
static void PrintCommandLineVersion();
static void PrintCommandLineHelp(const char* progname);
static bool InitializeConfig(std::string settings_filename);
static void InitializeEarlyConsole();
static void HookSignals();
static void SetAppRoot();
static void SetResourcesDirectory();
static bool SetDataDirectory();
static bool SetCriticalFolders();
static void SetDefaultSettings(SettingsInterface& si, bool system, bool controller);
static std::string GetResourcePath(std::string_view name, bool allow_override);
static void ProcessCPUThreadEvents(bool block);
static bool PerformEarlyHardwareChecks();
static bool EarlyProcessStartup();
static void WarnAboutInterface();
static void RunOnUIThread(std::function<void()> func);
static void StartCPUThread();
static void StopCPUThread();
static void ProcessCPUThreadEvents(bool block);
static void ProcessCPUThreadPlatformMessages();
static void CPUThreadEntryPoint();
static void CPUThreadMainLoop();
static void GPUThreadEntryPoint();
static void UIThreadMainLoop();
static void ProcessSDLEvent(const SDL_Event* ev);
static std::string GetWindowTitle(const std::string& game_title);
static std::optional<WindowInfo> TranslateSDLWindowInfo(SDL_Window* win, Error* error);
static bool GetSavedPlatformWindowGeometry(s32* x, s32* y, s32* width, s32* height);
static void SavePlatformWindowGeometry(s32 x, s32 y, s32 width, s32 height);

struct SDLHostState
{
  // UI thread state
  ALIGN_TO_CACHE_LINE std::unique_ptr<INISettingsInterface> base_settings_interface;
  bool batch_mode = false;
  bool start_fullscreen_ui_fullscreen = false;
  bool was_paused_by_focus_loss = false;
  bool ui_thread_running = false;

  u32 func_event_id = 0;

  SDL_Window* sdl_window = nullptr;
  float sdl_window_scale = 0.0f;
  WindowInfo::PreRotation force_prerotation = WindowInfo::PreRotation::Identity;
  std::atomic_bool fullscreen{false};

  Threading::Thread cpu_thread;
  Threading::Thread gpu_thread;
  Threading::KernelSemaphore platform_window_updated;

  std::mutex state_mutex;
  FullscreenUI::BackgroundProgressCallback* game_list_refresh_progress = nullptr;

  // CPU thread state.
  ALIGN_TO_CACHE_LINE std::atomic_bool cpu_thread_running{false};
  std::mutex cpu_thread_events_mutex;
  std::condition_variable cpu_thread_event_done;
  std::condition_variable cpu_thread_event_posted;
  std::deque<std::pair<std::function<void()>, bool>> cpu_thread_events;
  u32 blocking_cpu_events_pending = 0;
};

static SDLHostState s_state;
} // namespace MiniHost

//////////////////////////////////////////////////////////////////////////
// Initialization/Shutdown
//////////////////////////////////////////////////////////////////////////

bool MiniHost::PerformEarlyHardwareChecks()
{
  Error error;
  const bool okay = System::PerformEarlyHardwareChecks(&error);
  if (okay && !error.IsValid()) [[likely]]
    return true;

  if (okay)
    Host::ReportErrorAsync("Hardware Check Warning", error.GetDescription());
  else
    Host::ReportFatalError("Hardware Check Failed", error.GetDescription());

  return okay;
}

bool MiniHost::EarlyProcessStartup()
{
  Error error;
  if (!System::ProcessStartup(&error)) [[unlikely]]
  {
    Host::ReportFatalError("Process Startup Failed", error.GetDescription());
    return false;
  }

#if !__has_include("scmversion/tag.h")
  //
  // To those distributing their own builds or packages of DuckStation, and seeing this message:
  //
  // DuckStation is licensed under the CC-BY-NC-ND-4.0 license.
  //
  // This means that you do NOT have permission to re-distribute your own modified builds of DuckStation.
  // Modifying DuckStation for personal use is fine, but you cannot distribute builds with your changes.
  // As per the CC-BY-NC-ND conditions, you can re-distribute the official builds from https://www.duckstation.org/ and
  // https://github.com/stenzek/duckstation, so long as they are left intact, without modification. I welcome and
  // appreciate any pull requests made to the official repository at https://github.com/stenzek/duckstation.
  //
  // I made the decision to switch to a no-derivatives license because of numerous "forks" that were created purely for
  // generating money for the person who knocked it off, and always died, leaving the community with multiple builds to
  // choose from, most of which were out of date and broken, and endless confusion. Other forks copy/pasted upstream
  // changes without attribution, violating copyright.
  //
  // Thanks, and I hope you understand.
  //

  const char* message = ICON_EMOJI_WARNING "WARNING! You are not using an official release! " ICON_EMOJI_WARNING "\n\n"
                                           "DuckStation is licensed under the terms of CC-BY-NC-ND-4.0, which does not "
                                           "allow modified builds to be distributed.\n\n"
                                           "This build is NOT OFFICIAL and may be broken and/or malicious.\n\n"
                                           "You should download an official build from https://www.duckstation.org/.";

  Host::AddKeyedOSDWarning("MiniWarning", message, Host::OSD_CRITICAL_ERROR_DURATION);
#endif

  return true;
}

bool MiniHost::SetCriticalFolders()
{
  SetAppRoot();
  SetResourcesDirectory();
  if (!SetDataDirectory())
    return false;

  // logging of directories in case something goes wrong super early
  DEV_LOG("AppRoot Directory: {}", EmuFolders::AppRoot);
  DEV_LOG("DataRoot Directory: {}", EmuFolders::DataRoot);
  DEV_LOG("Resources Directory: {}", EmuFolders::Resources);

  // Write crash dumps to the data directory, since that'll be accessible for certain.
  CrashHandler::SetWriteDirectory(EmuFolders::DataRoot);

  // the resources directory should exist, bail out if not
  if (!FileSystem::DirectoryExists(EmuFolders::Resources.c_str()))
  {
    Host::ReportFatalError("Error", "Resources directory is missing, your installation is incomplete.");
    return false;
  }

  return true;
}

void MiniHost::SetAppRoot()
{
  const std::string program_path = FileSystem::GetProgramPath();
  INFO_LOG("Program Path: {}", program_path);

  EmuFolders::AppRoot = Path::Canonicalize(Path::GetDirectory(program_path));
}

void MiniHost::SetResourcesDirectory()
{
#ifndef __APPLE__NOT_USED // Not using bundles yet.
  // On Windows/Linux, these are in the binary directory.
  EmuFolders::Resources = Path::Combine(EmuFolders::AppRoot, "resources");
#else
  // On macOS, this is in the bundle resources directory.
  EmuFolders::Resources = Path::Canonicalize(Path::Combine(EmuFolders::AppRoot, "../Resources"));
#endif
}

bool MiniHost::SetDataDirectory()
{
  // Already set, e.g. by -portable.
  if (EmuFolders::DataRoot.empty())
    EmuFolders::DataRoot = Host::Internal::ComputeDataDirectory();

  // make sure it exists
  if (!EmuFolders::DataRoot.empty() && !FileSystem::DirectoryExists(EmuFolders::DataRoot.c_str()))
  {
    // we're in trouble if we fail to create this directory... but try to hobble on with portable
    Error error;
    if (!FileSystem::EnsureDirectoryExists(EmuFolders::DataRoot.c_str(), false, &error))
    {
      Host::ReportFatalError("Error",
                             TinyString::from_format("Failed to create data directory: {}", error.GetDescription()));
      return false;
    }
  }

  // couldn't determine the data directory? fallback to portable.
  if (EmuFolders::DataRoot.empty())
    EmuFolders::DataRoot = EmuFolders::AppRoot;

  return true;
}

bool MiniHost::InitializeConfig(std::string settings_filename)
{
  if (!SetCriticalFolders())
    return false;

  if (settings_filename.empty())
    settings_filename = Path::Combine(EmuFolders::DataRoot, "settings.ini");

  const bool settings_exists = FileSystem::FileExists(settings_filename.c_str());
  INFO_LOG("Loading config from {}.", settings_filename);
  s_state.base_settings_interface = std::make_unique<INISettingsInterface>(std::move(settings_filename));
  Host::Internal::SetBaseSettingsLayer(s_state.base_settings_interface.get());

  u32 settings_version;
  if (!settings_exists || !s_state.base_settings_interface->Load() ||
      !s_state.base_settings_interface->GetUIntValue("Main", "SettingsVersion", &settings_version) ||
      settings_version != SETTINGS_VERSION)
  {
    if (s_state.base_settings_interface->ContainsValue("Main", "SettingsVersion"))
    {
      // NOTE: No point translating this, because there's no config loaded, so no language loaded.
      Host::ReportErrorAsync("Error", fmt::format("Settings version {} does not match expected version {}, resetting.",
                                                  settings_version, SETTINGS_VERSION));
    }

    s_state.base_settings_interface->SetUIntValue("Main", "SettingsVersion", SETTINGS_VERSION);
    SetDefaultSettings(*s_state.base_settings_interface, true, true);

    // Make sure we can actually save the config, and the user doesn't have some permission issue.
    Error error;
    if (!s_state.base_settings_interface->Save(&error))
    {
      Host::ReportFatalError(
        "Error",
        fmt::format(
          "Failed to save configuration to\n\n{}\n\nThe error was: {}\n\nPlease ensure this directory is writable. You "
          "can also try portable mode by creating portable.txt in the same directory you installed DuckStation into.",
          s_state.base_settings_interface->GetPath(), error.GetDescription()));
      return false;
    }
  }

  EmuFolders::LoadConfig(*s_state.base_settings_interface.get());
  EmuFolders::EnsureFoldersExist();

  // We need to create the console window early, otherwise it appears in front of the main window.
  if (!Log::IsConsoleOutputEnabled() && s_state.base_settings_interface->GetBoolValue("Logging", "LogToConsole", false))
    Log::SetConsoleOutputParams(true, s_state.base_settings_interface->GetBoolValue("Logging", "LogTimestamps", true));

  // imgui setup, make sure it doesn't bug out
  ImGuiManager::SetFontPathAndRange(std::string(), {0x0020, 0x00FF, 0x2022, 0x2022, 0, 0});

  return true;
}

void MiniHost::SetDefaultSettings(SettingsInterface& si, bool system, bool controller)
{
  if (system)
  {
    System::SetDefaultSettings(si);
    EmuFolders::SetDefaults();
    EmuFolders::Save(si);
  }

  if (controller)
  {
    InputManager::SetDefaultSourceConfig(si);
    Settings::SetDefaultControllerConfig(si);
    Settings::SetDefaultHotkeyConfig(si);
  }
}

void Host::ReportDebuggerMessage(std::string_view message)
{
  ERROR_LOG("ReportDebuggerMessage(): {}", message);
}

std::span<const std::pair<const char*, const char*>> Host::GetAvailableLanguageList()
{
  return {};
}

bool Host::ChangeLanguage(const char* new_language)
{
  return false;
}

void Host::AddFixedInputBindings(const SettingsInterface& si)
{
}

void Host::OnInputDeviceConnected(std::string_view identifier, std::string_view device_name)
{
  Host::AddKeyedOSDMessage(fmt::format("InputDeviceConnected-{}", identifier),
                           fmt::format("Input device {0} ({1}) connected.", device_name, identifier), 10.0f);
}

void Host::OnInputDeviceDisconnected(InputBindingKey key, std::string_view identifier)
{
  Host::AddKeyedOSDMessage(fmt::format("InputDeviceConnected-{}", identifier),
                           fmt::format("Input device {} disconnected.", identifier), 10.0f);
}

s32 Host::Internal::GetTranslatedStringImpl(std::string_view context, std::string_view msg,
                                            std::string_view disambiguation, char* tbuf, size_t tbuf_space)
{
  if (msg.size() > tbuf_space)
    return -1;
  else if (msg.empty())
    return 0;

  std::memcpy(tbuf, msg.data(), msg.size());
  return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
  TinyString count_str = TinyString::from_format("{}", count);

  std::string ret(msg);
  for (;;)
  {
    std::string::size_type pos = ret.find("%n");
    if (pos == std::string::npos)
      break;

    ret.replace(pos, pos + 2, count_str.view());
  }

  return ret;
}

SmallString Host::TranslatePluralToSmallString(const char* context, const char* msg, const char* disambiguation,
                                               int count)
{
  SmallString ret(msg);
  ret.replace("%n", TinyString::from_format("{}", count));
  return ret;
}

std::string MiniHost::GetResourcePath(std::string_view filename, bool allow_override)
{
  return allow_override ? EmuFolders::GetOverridableResourcePath(filename) :
                          Path::Combine(EmuFolders::Resources, filename);
}

bool Host::ResourceFileExists(std::string_view filename, bool allow_override)
{
  const std::string path = MiniHost::GetResourcePath(filename, allow_override);
  return FileSystem::FileExists(path.c_str());
}

std::optional<DynamicHeapArray<u8>> Host::ReadResourceFile(std::string_view filename, bool allow_override, Error* error)
{
  const std::string path = MiniHost::GetResourcePath(filename, allow_override);
  return FileSystem::ReadBinaryFile(path.c_str(), error);
}

std::optional<std::string> Host::ReadResourceFileToString(std::string_view filename, bool allow_override, Error* error)
{
  const std::string path = MiniHost::GetResourcePath(filename, allow_override);
  return FileSystem::ReadFileToString(path.c_str(), error);
}

std::optional<std::time_t> Host::GetResourceFileTimestamp(std::string_view filename, bool allow_override)
{
  const std::string path = MiniHost::GetResourcePath(filename, allow_override);
  FILESYSTEM_STAT_DATA sd;
  if (!FileSystem::StatFile(path.c_str(), &sd))
  {
    ERROR_LOG("Failed to stat resource file '{}'", filename);
    return std::nullopt;
  }

  return sd.ModificationTime;
}

void Host::LoadSettings(const SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
}

void Host::CheckForSettingsChanges(const Settings& old_settings)
{
}

void Host::CommitBaseSettingChanges()
{
  auto lock = Host::GetSettingsLock();
  Error error;
  if (!MiniHost::s_state.base_settings_interface->Save(&error))
    ERROR_LOG("Failed to save settings: {}", error.GetDescription());
}

std::optional<WindowInfo> MiniHost::TranslateSDLWindowInfo(SDL_Window* win, Error* error)
{
  if (!win)
  {
    Error::SetStringView(error, "Window handle is null.");
    return std::nullopt;
  }

  SDL_SysWMinfo swi = {};
  SDL_VERSION(&swi.version);

  if (!SDL_GetWindowWMInfo(win, &swi))
  {
    Error::SetStringFmt(error, "SDL_GetWindowWMInfo() failed: {}", SDL_GetError());
    return std::nullopt;
  }

  const u32 window_flags = SDL_GetWindowFlags(win);
  int window_width = 1, window_height = 1;
  int window_px_width = 1, window_px_height = 1;
  SDL_GetWindowSize(win, &window_width, &window_height);
  SDL_GetWindowSizeInPixels(win, &window_px_width, &window_px_height);
  s_state.sdl_window_scale =
    static_cast<float>(std::max(window_px_width, 1)) / static_cast<float>(std::max(window_width, 1));

  SDL_DisplayMode dispmode;
  bool dispmode_valid = false;

  if (window_flags & SDL_WINDOW_FULLSCREEN)
  {
    dispmode_valid = (SDL_GetWindowDisplayMode(win, &dispmode) == 0);
    if (!dispmode_valid)
      ERROR_LOG("SDL_GetWindowDisplayMode() failed: {}", SDL_GetError());
  }

  if (const int display_index = SDL_GetWindowDisplayIndex(win); display_index >= 0)
  {
    float ddpi, hdpi, vdpi;
    if (SDL_GetDisplayDPI(display_index, &ddpi, &hdpi, &vdpi) == 0)
      s_state.sdl_window_scale = std::max(ddpi / DEFAULT_WINDOW_DPI, 0.5f);

    if (!(window_flags & SDL_WINDOW_FULLSCREEN))
    {
      dispmode_valid = (SDL_GetDesktopDisplayMode(display_index, &dispmode) == 0);
      if (!dispmode_valid)
        ERROR_LOG("SDL_GetDesktopDisplayMode() failed: {}", SDL_GetError());
    }
  }

  WindowInfo wi;
  wi.surface_width = static_cast<u16>(window_px_width);
  wi.surface_height = static_cast<u16>(window_px_height);
  wi.surface_scale = s_state.sdl_window_scale;
  wi.surface_prerotation = s_state.force_prerotation;

  // set display refresh rate if available
  if (dispmode_valid)
  {
    INFO_LOG("Display mode refresh rate: {} hz", dispmode.refresh_rate);
    wi.surface_refresh_rate = static_cast<float>(dispmode.refresh_rate);
  }

  // SDL's opengl window flag tends to make a mess of pixel formats...
  if (!(SDL_GetWindowFlags(win) & (SDL_WINDOW_OPENGL | SDL_WINDOW_VULKAN)))
  {
    switch (swi.subsystem)
    {
#ifdef SDL_VIDEO_DRIVER_WINDOWS
      case SDL_SYSWM_WINDOWS:
        wi.type = WindowInfo::Type::Win32;
        wi.window_handle = swi.info.win.window;
        return wi;
#endif

#ifdef SDL_VIDEO_DRIVER_X11
      case SDL_SYSWM_X11:
        wi.type = WindowInfo::Type::Xlib;
        wi.display_connection = swi.info.x11.display;
        wi.window_handle = (void*)swi.info.x11.window;
        return wi;
#endif

#ifdef SDL_VIDEO_DRIVER_WAYLAND
      case SDL_SYSWM_WAYLAND:
        wi.type = WindowInfo::Type::Wayland;
        wi.display_connection = swi.info.wl.display;
        wi.window_handle = swi.info.wl.surface;
        return wi;
#endif

#ifdef SDL_VIDEO_DRIVER_COCOA
      case SDL_SYSWM_COCOA:
        wi.type = WindowInfo::Type::MacOS;
        wi.window_handle = swi.info.cocoa.window;
        return wi;
#endif

      default:
        ERROR_LOG("Unhandled subsystem {}", static_cast<int>(swi.subsystem));
        break;
    }
  }

  // nothing handled, fall back to SDL abstraction
  wi.type = WindowInfo::Type::SDL;
  wi.window_handle = win;
  return wi;
}

std::optional<WindowInfo> Host::AcquireRenderWindow(RenderAPI render_api, bool fullscreen, bool exclusive_fullscreen,
                                                    Error* error)
{
  using namespace MiniHost;

  std::optional<WindowInfo> wi;

  MiniHost::RunOnUIThread([render_api, fullscreen, error, &wi]() {
    s32 window_x, window_y, window_width, window_height;
    if (!MiniHost::GetSavedPlatformWindowGeometry(&window_x, &window_y, &window_width, &window_height))
    {
      window_x = SDL_WINDOWPOS_UNDEFINED;
      window_y = SDL_WINDOWPOS_UNDEFINED;
      window_width = DEFAULT_WINDOW_WIDTH;
      window_height = DEFAULT_WINDOW_HEIGHT;
    }

    int flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS | SDL_WINDOW_SHOWN |
                SDL_WINDOW_ALLOW_HIGHDPI;
    if (render_api == RenderAPI::OpenGL || render_api == RenderAPI::OpenGLES)
      flags |= SDL_WINDOW_OPENGL;
    else if (render_api == RenderAPI::Vulkan)
      flags |= SDL_WINDOW_VULKAN;
    if (fullscreen)
      flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    s_state.sdl_window = SDL_CreateWindow(GetWindowTitle(System::GetGameTitle()).c_str(), window_x, window_y,
                                          window_width, window_height, flags);
    if (s_state.sdl_window)
    {
      wi = TranslateSDLWindowInfo(s_state.sdl_window, error);
      if (wi.has_value())
      {
        s_state.fullscreen.store(fullscreen, std::memory_order_release);
      }
      else
      {
        SDL_DestroyWindow(s_state.sdl_window);
        s_state.sdl_window = nullptr;
      }
    }
    else
    {
      Error::SetStringFmt(error, "SDL_CreateWindow() failed: {}", SDL_GetError());
    }

    s_state.platform_window_updated.Post();
  });

  s_state.platform_window_updated.Wait();

  // reload input sources, since it might use the window handle
  {
    auto lock = Host::GetSettingsLock();
    InputManager::ReloadSources(*Host::GetSettingsInterface(), lock);
  }

  return wi;
}

void Host::ReleaseRenderWindow()
{
  using namespace MiniHost;

  if (!s_state.sdl_window)
    return;

  MiniHost::RunOnUIThread([]() {
    if (!s_state.fullscreen.load(std::memory_order_acquire))
    {
      int window_x = SDL_WINDOWPOS_UNDEFINED, window_y = SDL_WINDOWPOS_UNDEFINED;
      int window_width = DEFAULT_WINDOW_WIDTH, window_height = DEFAULT_WINDOW_HEIGHT;
      SDL_GetWindowPosition(s_state.sdl_window, &window_x, &window_y);
      SDL_GetWindowSize(s_state.sdl_window, &window_width, &window_height);
      MiniHost::SavePlatformWindowGeometry(window_x, window_y, window_width, window_height);
    }
    else
    {
      s_state.fullscreen.store(false, std::memory_order_release);
    }

    SDL_DestroyWindow(s_state.sdl_window);
    s_state.sdl_window = nullptr;

    s_state.platform_window_updated.Post();
  });

  s_state.platform_window_updated.Wait();
}

bool Host::IsFullscreen()
{
  using namespace MiniHost;

  return s_state.fullscreen.load(std::memory_order_acquire);
}

void Host::SetFullscreen(bool enabled)
{
  using namespace MiniHost;

  if (!s_state.sdl_window || s_state.fullscreen.load(std::memory_order_acquire) == enabled)
    return;

  if (SDL_SetWindowFullscreen(s_state.sdl_window, enabled ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) != 0)
  {
    ERROR_LOG("SDL_SetWindowFullscreen() failed: {}", SDL_GetError());
    return;
  }

  s_state.fullscreen.store(enabled, std::memory_order_release);
}

void Host::BeginTextInput()
{
  SDL_StartTextInput();
}

void Host::EndTextInput()
{
  // we want to keep getting text events, SDL_StopTextInput() apparently inhibits that
}

bool Host::CreateAuxiliaryRenderWindow(s32 x, s32 y, u32 width, u32 height, std::string_view title,
                                       std::string_view icon_name, AuxiliaryRenderWindowUserData userdata,
                                       AuxiliaryRenderWindowHandle* handle, WindowInfo* wi, Error* error)
{
  // not here, but could be...
  Error::SetStringView(error, "Not supported.");
  return false;
}

void Host::DestroyAuxiliaryRenderWindow(AuxiliaryRenderWindowHandle handle, s32* pos_x /* = nullptr */,
                                        s32* pos_y /* = nullptr */, u32* width /* = nullptr */,
                                        u32* height /* = nullptr */)
{
  // noop
}

bool MiniHost::GetSavedPlatformWindowGeometry(s32* x, s32* y, s32* width, s32* height)
{
  auto lock = Host::GetSettingsLock();

  bool result = s_state.base_settings_interface->GetIntValue("SimpleHost", "WindowX", x);
  result = result && s_state.base_settings_interface->GetIntValue("SimpleHost", "WindowY", y);
  result = result && s_state.base_settings_interface->GetIntValue("SimpleHost", "WindowWidth", width);
  result = result && s_state.base_settings_interface->GetIntValue("SimpleHost", "WindowHeight", height);
  return result;
}

void MiniHost::SavePlatformWindowGeometry(s32 x, s32 y, s32 width, s32 height)
{
  if (Host::IsFullscreen())
    return;

  auto lock = Host::GetSettingsLock();
  s_state.base_settings_interface->SetIntValue("SimpleHost", "WindowX", x);
  s_state.base_settings_interface->SetIntValue("SimpleHost", "WindowY", y);
  s_state.base_settings_interface->SetIntValue("SimpleHost", "WindowWidth", width);
  s_state.base_settings_interface->SetIntValue("SimpleHost", "WindowHeight", height);
  s_state.base_settings_interface->Save();
}

void MiniHost::UIThreadMainLoop()
{
  while (s_state.ui_thread_running)
  {
    SDL_Event ev;
    if (!SDL_WaitEvent(&ev))
      continue;

    ProcessSDLEvent(&ev);
  }
}

void MiniHost::ProcessSDLEvent(const SDL_Event* ev)
{
  switch (ev->type)
  {
    case SDL_WINDOWEVENT:
    {
      switch (ev->window.event)
      {
        case SDL_WINDOWEVENT_SIZE_CHANGED:
        {
          int window_width = ev->window.data1, window_height = ev->window.data2;
          SDL_GetWindowSizeInPixels(s_state.sdl_window, &window_width, &window_height);
          Host::RunOnCPUThread([window_width, window_height, window_scale = s_state.sdl_window_scale]() {
            GPUThread::ResizeDisplayWindow(window_width, window_height, window_scale);
          });
        }
        break;

        case SDL_WINDOWEVENT_DISPLAY_CHANGED:
        {
          const int new_display = ev->window.data1;
          float ddpi, hdpi, vdpi;
          if (SDL_GetDisplayDPI(new_display, &ddpi, &hdpi, &vdpi) == 0)
          {
            if (const float new_scale = std::max(ddpi / DEFAULT_WINDOW_DPI, 0.5f);
                new_scale != s_state.sdl_window_scale)
            {
              s_state.sdl_window_scale = new_scale;

              int window_width = 1, window_height = 1;
              SDL_GetWindowSizeInPixels(s_state.sdl_window, &window_width, &window_height);
              Host::RunOnCPUThread([window_width, window_height, window_scale = s_state.sdl_window_scale]() {
                GPUThread::ResizeDisplayWindow(window_width, window_height, window_scale);
              });
            }
          }
        }
        break;

        case SDL_WINDOWEVENT_CLOSE:
        {
          Host::RunOnCPUThread([]() { Host::RequestExitApplication(false); });
        }
        break;

        case SDL_WINDOWEVENT_FOCUS_GAINED:
        {
          Host::RunOnCPUThread([]() {
            if (!System::IsValid() || !s_state.was_paused_by_focus_loss)
              return;

            System::PauseSystem(false);
            s_state.was_paused_by_focus_loss = false;
          });
        }
        break;

        case SDL_WINDOWEVENT_FOCUS_LOST:
        {
          Host::RunOnCPUThread([]() {
            if (!System::IsRunning() || !g_settings.pause_on_focus_loss)
              return;

            s_state.was_paused_by_focus_loss = true;
            System::PauseSystem(true);
          });
        }
        break;

        default:
          break;
      }
    }
    break;

    case SDL_KEYDOWN:
    case SDL_KEYUP:
    {
      Host::RunOnCPUThread([key_code = static_cast<u32>(ev->key.keysym.sym), pressed = (ev->type == SDL_KEYDOWN)]() {
        InputManager::InvokeEvents(InputManager::MakeHostKeyboardKey(key_code), pressed ? 1.0f : 0.0f,
                                   GenericInputBinding::Unknown);
      });
    }
    break;

    case SDL_TEXTINPUT:
    {
      if (ImGuiManager::WantsTextInput())
        Host::RunOnCPUThread([text = std::string(ev->text.text)]() { ImGuiManager::AddTextInput(std::move(text)); });
    }
    break;

    case SDL_MOUSEMOTION:
    {
      Host::RunOnCPUThread([x = static_cast<float>(ev->motion.x), y = static_cast<float>(ev->motion.y)]() {
        InputManager::UpdatePointerAbsolutePosition(0, x, y);
        ImGuiManager::UpdateMousePosition(x, y);
      });
    }
    break;

    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    {
      const bool pressed = (ev->type == SDL_MOUSEBUTTONDOWN);
      if (ev->button.button > 0)
      {
        Host::RunOnCPUThread([button = ev->button.button - 1, pressed]() {
          InputManager::InvokeEvents(InputManager::MakePointerButtonKey(0, button), pressed ? 1.0f : 0.0f,
                                     GenericInputBinding::Unknown);
        });
      }
    }
    break;

    case SDL_MOUSEWHEEL:
    {
      Host::RunOnCPUThread([x = ev->wheel.preciseX, y = ev->wheel.preciseY]() {
        if (x != 0.0f)
          InputManager::UpdatePointerRelativeDelta(0, InputPointerAxis::WheelX, x);
        if (y != 0.0f)
          InputManager::UpdatePointerRelativeDelta(0, InputPointerAxis::WheelY, y);
      });
    }
    break;

    case SDL_QUIT:
    {
      Host::RunOnCPUThread([]() { Host::RequestExitApplication(false); });
    }
    break;

    default:
    {
      if (ev->type == s_state.func_event_id)
      {
        std::function<void()>* pfunc = reinterpret_cast<std::function<void()>*>(ev->user.data1);
        if (pfunc)
        {
          (*pfunc)();
          delete pfunc;
        }
      }
      else if (SDLInputSource::IsHandledInputEvent(ev))
      {
        Host::RunOnCPUThread([event_copy = *ev]() {
          SDLInputSource* is =
            static_cast<SDLInputSource*>(InputManager::GetInputSourceInterface(InputSourceType::SDL));
          if (is)
            is->ProcessSDLEvent(&event_copy);
        });
      }
    }
    break;
  }
}

void MiniHost::RunOnUIThread(std::function<void()> func)
{
  std::function<void()>* pfunc = new std::function<void()>(std::move(func));

  SDL_Event ev;
  ev.user = {};
  ev.type = s_state.func_event_id;
  ev.user.data1 = pfunc;
  SDL_PushEvent(&ev);
}

void MiniHost::ProcessCPUThreadPlatformMessages()
{
  // This is lame. On Win32, we need to pump messages, even though *we* don't have any windows
  // on the CPU thread, because SDL creates a hidden window for raw input for some game controllers.
  // If we don't do this, we don't get any controller events.
#ifdef _WIN32
  MSG msg;
  while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
  {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
#endif
}

void MiniHost::ProcessCPUThreadEvents(bool block)
{
  std::unique_lock lock(s_state.cpu_thread_events_mutex);

  for (;;)
  {
    if (s_state.cpu_thread_events.empty())
    {
      if (!block || !s_state.cpu_thread_running.load(std::memory_order_acquire))
        return;

      // we still need to keep polling the controllers when we're paused
      do
      {
        ProcessCPUThreadPlatformMessages();
        InputManager::PollSources();
      } while (!s_state.cpu_thread_event_posted.wait_for(lock, CPU_THREAD_POLL_INTERVAL,
                                                         []() { return !s_state.cpu_thread_events.empty(); }));
    }

    // return after processing all events if we had one
    block = false;

    auto event = std::move(s_state.cpu_thread_events.front());
    s_state.cpu_thread_events.pop_front();
    lock.unlock();
    event.first();
    lock.lock();

    if (event.second)
    {
      s_state.blocking_cpu_events_pending--;
      s_state.cpu_thread_event_done.notify_one();
    }
  }
}

void MiniHost::StartCPUThread()
{
  s_state.cpu_thread_running.store(true, std::memory_order_release);
  s_state.cpu_thread.Start(CPUThreadEntryPoint);
}

void MiniHost::StopCPUThread()
{
  if (!s_state.cpu_thread.Joinable())
    return;

  {
    std::unique_lock lock(s_state.cpu_thread_events_mutex);
    s_state.cpu_thread_running.store(false, std::memory_order_release);
    s_state.cpu_thread_event_posted.notify_one();
  }

  s_state.cpu_thread.Join();
}

void MiniHost::CPUThreadEntryPoint()
{
  Threading::SetNameOfCurrentThread("CPU Thread");

  // input source setup must happen on emu thread
  Error error;
  if (!System::CPUThreadInitialize(&error))
  {
    Host::ReportFatalError("CPU Thread Initialization Failed", error.GetDescription());
    return;
  }

  // start up GPU thread
  s_state.gpu_thread.Start(&GPUThreadEntryPoint);

  // start the fullscreen UI and get it going
  if (GPUThread::StartFullscreenUI(s_state.start_fullscreen_ui_fullscreen, &error))
  {
    WarnAboutInterface();

    // kick a game list refresh if we're not in batch mode
    if (!s_state.batch_mode)
      Host::RefreshGameListAsync(false);

    CPUThreadMainLoop();

    Host::CancelGameListRefresh();
  }
  else
  {
    Host::ReportFatalError("Error", fmt::format("Failed to start fullscreen UI: {}", error.GetDescription()));
  }

  // finish any events off (e.g. shutdown system with save)
  ProcessCPUThreadEvents(false);

  if (System::IsValid())
    System::ShutdownSystem(false);

  GPUThread::StopFullscreenUI();
  GPUThread::Internal::RequestShutdown();
  s_state.gpu_thread.Join();

  System::CPUThreadShutdown();

  // Tell the UI thread to shut down.
  RunOnUIThread([]() { s_state.ui_thread_running = false; });
}

void MiniHost::CPUThreadMainLoop()
{
  while (s_state.cpu_thread_running.load(std::memory_order_acquire))
  {
    if (System::IsRunning())
    {
      System::Execute();
      continue;
    }
    else if (!GPUThread::IsUsingThread() && GPUThread::IsRunningIdle())
    {
      ProcessCPUThreadEvents(false);
      if (!GPUThread::IsUsingThread() && GPUThread::IsRunningIdle())
        GPUThread::Internal::DoRunIdle();
    }

    ProcessCPUThreadEvents(true);
  }
}

void MiniHost::GPUThreadEntryPoint()
{
  Threading::SetNameOfCurrentThread("CPU Thread");
  GPUThread::Internal::GPUThreadEntryPoint();
}

void Host::OnSystemStarting()
{
  MiniHost::s_state.was_paused_by_focus_loss = false;
}

void Host::OnSystemStarted()
{
}

void Host::OnSystemPaused()
{
}

void Host::OnSystemResumed()
{
}

void Host::OnSystemDestroyed()
{
}

void Host::OnGPUThreadRunIdleChanged(bool is_active)
{
}

void Host::FrameDoneOnGPUThread(GPUBackend* gpu_backend, u32 frame_number)
{
}

void Host::OnPerformanceCountersUpdated(const GPUBackend* gpu_backend)
{
  // noop
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
  // noop
}

void Host::OnAchievementsLoginSuccess(const char* username, u32 points, u32 sc_points, u32 unread_messages)
{
  // noop
}

void Host::OnAchievementsRefreshed()
{
  // noop
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
  // noop
}

void Host::OnCoverDownloaderOpenRequested()
{
  // noop
}

void Host::SetMouseMode(bool relative, bool hide_cursor)
{
  // noop
}

void Host::OnMediaCaptureStarted()
{
  // noop
}

void Host::OnMediaCaptureStopped()
{
  // noop
}

void Host::PumpMessagesOnCPUThread()
{
  MiniHost::ProcessCPUThreadEvents(false);
}

std::string MiniHost::GetWindowTitle(const std::string& game_title)
{
#if defined(_DEBUGFAST)
  static constexpr std::string_view suffix = " [DebugFast]";
#elif defined(_DEBUG)
  static constexpr std::string_view suffix = " [Debug]";
#else
  static constexpr std::string_view suffix = std::string_view();
#endif

  if (System::IsShutdown() || game_title.empty())
    return fmt::format("DuckStation {}{}", g_scm_tag_str, suffix);
  else
    return fmt::format("{}{}", game_title, suffix);
}

void MiniHost::WarnAboutInterface()
{
  const char* message = "This is the \"mini\" interface for DuckStation, and is missing many features.\n"
                        "       We recommend using the Qt interface instead, which you can download\n"
                        "       from https://www.duckstation.org/.";
  Host::AddIconOSDWarning("MiniWarning", ICON_EMOJI_WARNING, message, Host::OSD_INFO_DURATION);
}

void Host::OnGameChanged(const std::string& disc_path, const std::string& game_serial, const std::string& game_name)
{
  using namespace MiniHost;

  VERBOSE_LOG("Host::OnGameChanged(\"{}\", \"{}\", \"{}\")", disc_path, game_serial, game_name);
  if (s_state.sdl_window)
    SDL_SetWindowTitle(s_state.sdl_window, GetWindowTitle(game_name).c_str());
}

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
  using namespace MiniHost;

  std::unique_lock lock(s_state.cpu_thread_events_mutex);
  s_state.cpu_thread_events.emplace_back(std::move(function), block);
  s_state.blocking_cpu_events_pending += BoolToUInt32(block);
  s_state.cpu_thread_event_posted.notify_one();
  if (block)
    s_state.cpu_thread_event_done.wait(lock, []() { return s_state.blocking_cpu_events_pending == 0; });
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
  using namespace MiniHost;

  std::unique_lock lock(s_state.state_mutex);

  while (s_state.game_list_refresh_progress)
  {
    lock.unlock();
    CancelGameListRefresh();
    lock.lock();
  }

  s_state.game_list_refresh_progress = new FullscreenUI::BackgroundProgressCallback("glrefresh");
  System::QueueAsyncTask([invalidate_cache]() {
    GameList::Refresh(invalidate_cache, false, s_state.game_list_refresh_progress);

    std::unique_lock lock(s_state.state_mutex);
    delete s_state.game_list_refresh_progress;
  });
}

void Host::CancelGameListRefresh()
{
  using namespace MiniHost;

  {
    std::unique_lock lock(s_state.state_mutex);
    if (!s_state.game_list_refresh_progress)
      return;

    s_state.game_list_refresh_progress->SetCancelled();
  }

  System::WaitForAllAsyncTasks();
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
  return MiniHost::TranslateSDLWindowInfo(MiniHost::s_state.sdl_window, nullptr);
}

void Host::RequestExitApplication(bool allow_confirm)
{
  if (System::IsValid())
  {
    Host::RunOnCPUThread([]() { System::ShutdownSystem(g_settings.save_state_on_exit); });
  }

  // clear the running flag, this'll break out of the main CPU loop once the VM is shutdown.
  MiniHost::s_state.cpu_thread_running.store(false, std::memory_order_release);
}

void Host::RequestExitBigPicture()
{
  // sorry dude
}

void Host::RequestSystemShutdown(bool allow_confirm, bool save_state)
{
  // TODO: Confirm
  if (System::IsValid())
  {
    Host::RunOnCPUThread([save_state]() { System::ShutdownSystem(save_state); });
  }
}

void Host::ReportFatalError(std::string_view title, std::string_view message)
{
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, TinyString(title).c_str(), SmallString(message).c_str(), nullptr);
}

void Host::ReportErrorAsync(std::string_view title, std::string_view message)
{
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, TinyString(title).c_str(), SmallString(message).c_str(), nullptr);
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
  using namespace MiniHost;

  if (!s_state.sdl_window || s_state.fullscreen.load(std::memory_order_acquire))
    return;

  SDL_SetWindowSize(s_state.sdl_window, width, height);
}

void Host::OpenURL(std::string_view url)
{
  if (SDL_OpenURL(SmallString(url).c_str()) != 0)
    ERROR_LOG("SDL_OpenURL({}) failed: {}", url, SDL_GetError());
}

std::string Host::GetClipboardText()
{
  std::string ret;

  char* text = SDL_GetClipboardText();
  if (text)
  {
    ret = text;
    SDL_free(text);
  }

  return ret;
}

bool Host::CopyTextToClipboard(std::string_view text)
{
  if (SDL_SetClipboardText(SmallString(text).c_str()) != 0)
  {
    ERROR_LOG("SDL_SetClipboardText({}) failed: {}", text, SDL_GetError());
    return false;
  }

  return true;
}

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(std::string_view str)
{
  return SDLKeyNames::GetKeyCodeForName(str);
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code)
{
  const char* converted = SDLKeyNames::GetKeyName(code);
  return converted ? std::optional<std::string>(converted) : std::nullopt;
}

const char* InputManager::ConvertHostKeyboardCodeToIcon(u32 code)
{
  return nullptr;
}

bool Host::ConfirmMessage(std::string_view title, std::string_view message)
{
  const SmallString title_copy(title);
  const SmallString message_copy(message);

  static constexpr SDL_MessageBoxButtonData bd[2] = {
    {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Yes"},
    {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 2, "No"},
  };
  const SDL_MessageBoxData md = {SDL_MESSAGEBOX_INFORMATION,
                                 nullptr,
                                 title_copy.c_str(),
                                 message_copy.c_str(),
                                 static_cast<int>(std::size(bd)),
                                 bd,
                                 nullptr};

  int buttonid = -1;
  SDL_ShowMessageBox(&md, &buttonid);
  return (buttonid == 1);
}

void Host::OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
                                     FileSelectorFilters filters /* = FileSelectorFilters() */,
                                     std::string_view initial_directory /* = std::string_view() */)
{
  // TODO: Use SDL FileDialog API
  callback(std::string());
}

bool Host::ShouldPreferHostFileSelector()
{
  return false;
}

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()

static void SignalHandler(int signal)
{
  // First try the normal (graceful) shutdown/exit.
  static bool graceful_shutdown_attempted = false;
  if (!graceful_shutdown_attempted)
  {
    std::fprintf(stderr, "Received CTRL+C, attempting graceful shutdown. Press CTRL+C again to force.\n");
    graceful_shutdown_attempted = true;
    Host::RequestExitApplication(false);
    return;
  }

  std::signal(signal, SIG_DFL);

  // MacOS is missing std::quick_exit() despite it being C++11...
#ifndef __APPLE__
  std::quick_exit(1);
#else
  _Exit(1);
#endif
}

void MiniHost::HookSignals()
{
  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);
}

void MiniHost::InitializeEarlyConsole()
{
  const bool was_console_enabled = Log::IsConsoleOutputEnabled();
  if (!was_console_enabled)
    Log::SetConsoleOutputParams(true);
}

void MiniHost::PrintCommandLineVersion()
{
  InitializeEarlyConsole();

  std::fprintf(stderr, "DuckStation Version %s (%s)\n", g_scm_tag_str, g_scm_branch_str);
  std::fprintf(stderr, "https://github.com/stenzek/duckstation\n");
  std::fprintf(stderr, "\n");
}

void MiniHost::PrintCommandLineHelp(const char* progname)
{
  InitializeEarlyConsole();

  PrintCommandLineVersion();
  std::fprintf(stderr, "Usage: %s [parameters] [--] [boot filename]\n", progname);
  std::fprintf(stderr, "\n");
  std::fprintf(stderr, "  -help: Displays this information and exits.\n");
  std::fprintf(stderr, "  -version: Displays version information and exits.\n");
  std::fprintf(stderr, "  -batch: Enables batch mode (exits after powering off).\n");
  std::fprintf(stderr, "  -fastboot: Force fast boot for provided filename.\n");
  std::fprintf(stderr, "  -slowboot: Force slow boot for provided filename.\n");
  std::fprintf(stderr, "  -bios: Boot into the BIOS shell.\n");
  std::fprintf(stderr, "  -resume: Load resume save state. If a boot filename is provided,\n"
                       "    that game's resume state will be loaded, otherwise the most\n"
                       "    recent resume save state will be loaded.\n");
  std::fprintf(stderr, "  -state <index>: Loads specified save state by index. If a boot\n"
                       "    filename is provided, a per-game state will be loaded, otherwise\n"
                       "    a global state will be loaded.\n");
  std::fprintf(stderr, "  -statefile <filename>: Loads state from the specified filename.\n"
                       "    No boot filename is required with this option.\n");
  std::fprintf(stderr, "  -exe <filename>: Boot the specified exe instead of loading from disc.\n");
  std::fprintf(stderr, "  -fullscreen: Enters fullscreen mode immediately after starting.\n");
  std::fprintf(stderr, "  -nofullscreen: Prevents fullscreen mode from triggering if enabled.\n");
  std::fprintf(stderr, "  -portable: Forces \"portable mode\", data in same directory.\n");
  std::fprintf(stderr, "  -settings <filename>: Loads a custom settings configuration from the\n"
                       "    specified filename. Default settings applied if file not found.\n");
  std::fprintf(stderr, "  -earlyconsole: Creates console as early as possible, for logging.\n");
  std::fprintf(stderr, "  -prerotation <degrees>: Prerotates output by 90/180/270 degrees.\n");
  std::fprintf(stderr, "  --: Signals that no more arguments will follow and the remaining\n"
                       "    parameters make up the filename. Use when the filename contains\n"
                       "    spaces or starts with a dash.\n");
  std::fprintf(stderr, "\n");
}

std::optional<SystemBootParameters>& AutoBoot(std::optional<SystemBootParameters>& autoboot)
{
  if (!autoboot)
    autoboot.emplace();

  return autoboot;
}

bool MiniHost::ParseCommandLineParametersAndInitializeConfig(int argc, char* argv[],
                                                             std::optional<SystemBootParameters>& autoboot)
{
  std::optional<s32> state_index;
  std::string settings_filename;
  bool starting_bios = false;

  bool no_more_args = false;

  for (int i = 1; i < argc; i++)
  {
    if (!no_more_args)
    {
#define CHECK_ARG(str) (std::strcmp(argv[i], (str)) == 0)
#define CHECK_ARG_PARAM(str) (std::strcmp(argv[i], (str)) == 0 && ((i + 1) < argc))

      if (CHECK_ARG("-help"))
      {
        PrintCommandLineHelp(argv[0]);
        return false;
      }
      else if (CHECK_ARG("-version"))
      {
        PrintCommandLineVersion();
        return false;
      }
      else if (CHECK_ARG("-batch"))
      {
        INFO_LOG("Command Line: Using batch mode.");
        s_state.batch_mode = true;
        continue;
      }
      else if (CHECK_ARG("-bios"))
      {
        INFO_LOG("Command Line: Starting BIOS.");
        AutoBoot(autoboot);
        starting_bios = true;
        continue;
      }
      else if (CHECK_ARG("-fastboot"))
      {
        INFO_LOG("Command Line: Forcing fast boot.");
        AutoBoot(autoboot)->override_fast_boot = true;
        continue;
      }
      else if (CHECK_ARG("-slowboot"))
      {
        INFO_LOG("Command Line: Forcing slow boot.");
        AutoBoot(autoboot)->override_fast_boot = false;
        continue;
      }
      else if (CHECK_ARG("-resume"))
      {
        state_index = -1;
        INFO_LOG("Command Line: Loading resume state.");
        continue;
      }
      else if (CHECK_ARG_PARAM("-state"))
      {
        state_index = StringUtil::FromChars<s32>(argv[++i]);
        if (!state_index.has_value())
        {
          ERROR_LOG("Invalid state index");
          return false;
        }

        INFO_LOG("Command Line: Loading state index: {}", state_index.value());
        continue;
      }
      else if (CHECK_ARG_PARAM("-statefile"))
      {
        AutoBoot(autoboot)->save_state = argv[++i];
        INFO_LOG("Command Line: Loading state file: '{}'", autoboot->save_state);
        continue;
      }
      else if (CHECK_ARG_PARAM("-exe"))
      {
        AutoBoot(autoboot)->override_exe = argv[++i];
        INFO_LOG("Command Line: Overriding EXE file: '{}'", autoboot->override_exe);
        continue;
      }
      else if (CHECK_ARG("-fullscreen"))
      {
        INFO_LOG("Command Line: Using fullscreen.");
        AutoBoot(autoboot)->override_fullscreen = true;
        s_state.start_fullscreen_ui_fullscreen = true;
        continue;
      }
      else if (CHECK_ARG("-nofullscreen"))
      {
        INFO_LOG("Command Line: Not using fullscreen.");
        AutoBoot(autoboot)->override_fullscreen = false;
        continue;
      }
      else if (CHECK_ARG("-portable"))
      {
        INFO_LOG("Command Line: Using portable mode.");
        EmuFolders::DataRoot = EmuFolders::AppRoot;
        continue;
      }
      else if (CHECK_ARG_PARAM("-settings"))
      {
        settings_filename = argv[++i];
        INFO_LOG("Command Line: Overriding settings filename: {}", settings_filename);
        continue;
      }
      else if (CHECK_ARG("-earlyconsole"))
      {
        InitializeEarlyConsole();
        continue;
      }
      else if (CHECK_ARG_PARAM("-prerotation"))
      {
        const char* prerotation_str = argv[++i];
        if (std::strcmp(prerotation_str, "0") == 0 || StringUtil::EqualNoCase(prerotation_str, "identity"))
        {
          INFO_LOG("Command Line: Forcing surface pre-rotation to identity.");
          s_state.force_prerotation = WindowInfo::PreRotation::Identity;
        }
        else if (std::strcmp(prerotation_str, "90") == 0)
        {
          INFO_LOG("Command Line: Forcing surface pre-rotation to 90 degrees clockwise.");
          s_state.force_prerotation = WindowInfo::PreRotation::Rotate90Clockwise;
        }
        else if (std::strcmp(prerotation_str, "180") == 0)
        {
          INFO_LOG("Command Line: Forcing surface pre-rotation to 180 degrees clockwise.");
          s_state.force_prerotation = WindowInfo::PreRotation::Rotate180Clockwise;
        }
        else if (std::strcmp(prerotation_str, "270") == 0)
        {
          INFO_LOG("Command Line: Forcing surface pre-rotation to 270 degrees clockwise.");
          s_state.force_prerotation = WindowInfo::PreRotation::Rotate270Clockwise;
        }
        else
        {
          ERROR_LOG("Invalid prerotation value: {}", prerotation_str);
          return false;
        }

        continue;
      }
      else if (CHECK_ARG("--"))
      {
        no_more_args = true;
        continue;
      }
      else if (argv[i][0] == '-')
      {
        Host::ReportFatalError("Error", fmt::format("Unknown parameter: {}", argv[i]));
        return false;
      }

#undef CHECK_ARG
#undef CHECK_ARG_PARAM
    }

    if (autoboot && !autoboot->filename.empty())
      autoboot->filename += ' ';
    AutoBoot(autoboot)->filename += argv[i];
  }

  // To do anything useful, we need the config initialized.
  if (!InitializeConfig(std::move(settings_filename)))
  {
    // NOTE: No point translating this, because no config means the language won't be loaded anyway.
    Host::ReportFatalError("Error", "Failed to initialize config.");
    return EXIT_FAILURE;
  }

  // Check the file we're starting actually exists.

  if (autoboot && !autoboot->filename.empty() && !FileSystem::FileExists(autoboot->filename.c_str()))
  {
    Host::ReportFatalError("Error", fmt::format("File '{}' does not exist.", autoboot->filename));
    return false;
  }

  if (state_index.has_value())
  {
    AutoBoot(autoboot);

    if (autoboot->filename.empty())
    {
      // loading global state, -1 means resume the last game
      if (state_index.value() < 0)
        autoboot->save_state = System::GetMostRecentResumeSaveStatePath();
      else
        autoboot->save_state = System::GetGlobalSaveStateFileName(state_index.value());
    }
    else
    {
      // loading game state
      const std::string game_serial(GameDatabase::GetSerialForPath(autoboot->filename.c_str()));
      autoboot->save_state = System::GetGameSaveStateFileName(game_serial, state_index.value());
    }

    if (autoboot->save_state.empty() || !FileSystem::FileExists(autoboot->save_state.c_str()))
    {
      Host::ReportFatalError("Error", "The specified save state does not exist.");
      return false;
    }
  }

  // check autoboot parameters, if we set something like fullscreen without a bios
  // or disc, we don't want to actually start.
  if (autoboot && autoboot->filename.empty() && autoboot->save_state.empty() && !starting_bios)
    autoboot.reset();

  // if we don't have autoboot, we definitely don't want batch mode (because that'll skip
  // scanning the game list).
  if (s_state.batch_mode)
  {
    if (!autoboot)
    {
      Host::ReportFatalError("Error", "Cannot use batch mode, because no boot filename was specified.");
      return false;
    }

    // if using batch mode, immediately refresh the game list so the data is available
    GameList::Refresh(false, true);
  }

  return true;
}

#include <SDL_main.h>

int main(int argc, char* argv[])
{
  using namespace MiniHost;

  CrashHandler::Install(&Bus::CleanupMemoryMap);

  if (!PerformEarlyHardwareChecks())
    return EXIT_FAILURE;

  if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
  {
    Host::ReportFatalError("Error", TinyString::from_format("SDL_InitSubSystem() failed: {}", SDL_GetError()));
    return EXIT_FAILURE;
  }

  s_state.func_event_id = SDL_RegisterEvents(1);
  if (s_state.func_event_id == static_cast<u32>(-1))
  {
    Host::ReportFatalError("Error", TinyString::from_format("SDL_RegisterEvents() failed: {}", SDL_GetError()));
    return EXIT_FAILURE;
  }

  if (!EarlyProcessStartup())
    return EXIT_FAILURE;

  std::optional<SystemBootParameters> autoboot;
  if (!ParseCommandLineParametersAndInitializeConfig(argc, argv, autoboot))
    return EXIT_FAILURE;

  // the rest of initialization happens on the CPU thread.
  HookSignals();

  // prevent input source polling on CPU thread...
  SDLInputSource::ALLOW_EVENT_POLLING = false;
  s_state.ui_thread_running = true;
  StartCPUThread();

  // process autoboot early, that way we can set the fullscreen flag
  if (autoboot)
  {
    s_state.start_fullscreen_ui_fullscreen =
      s_state.start_fullscreen_ui_fullscreen || autoboot->override_fullscreen.value_or(false);
    Host::RunOnCPUThread([params = std::move(autoboot.value())]() mutable {
      Error error;
      if (!System::BootSystem(std::move(params), &error))
        Host::ReportErrorAsync("Failed to boot system", error.GetDescription());
    });
  }

  UIThreadMainLoop();

  StopCPUThread();

  System::ProcessShutdown();

  // Ensure log is flushed.
  Log::SetFileOutputParams(false, nullptr);

  s_state.base_settings_interface.reset();

  SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);

  return EXIT_SUCCESS;
}
