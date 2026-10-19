/* Copyright (c) 2026 Skitzelles - part of Game Vault, https://github.com/skitzellez/xbox-game-vault
 * GPL-2.0-or-later, like Dolphin.
 *
 * Racing wheels on the Xbox. A wheel (Logitech G920/G923 Xbox, Thrustmaster, Fanatec, MOZA ...) is not a gamepad,
 * so WGInput only saw its raw buttons and axes, which no default mapping names. Windows.Gaming.Input.RacingWheel
 * gives the wheel, pedals and buttons in fixed units on every Xbox wheel, so the wheel is offered here as one more
 * "Xbox One Game Controller" with a gamepad's input names, and the Xbox default mappings drive the game with it:
 * steering = left stick X, throttle = right trigger, brake = left trigger, A/B/X/Y, Menu/View and the d-pad as on a
 * pad, gear paddles = bumpers, the rim's two extra buttons = stick clicks. Nothing changes without a wheel.
 */
#include "InputCommon/ControllerInterface/WGInput/GameVaultWheel.h"

#include <algorithm>
#include <mutex>
#include <set>
#include <string_view>

#include <fmt/format.h>

#include "Common/Logging/Log.h"

namespace WGI = winrt::Windows::Gaming::Input;

namespace ciface::WGInput::GameVaultWheel
{
namespace
{
// steering turned this far (degrees, lock to lock) is a full stick; most wheels turn 900, a stick game wants less
constexpr double kStickLockDegrees = 270.0;

class WheelDevice;
// the wheel devices alive now (Dolphin is built without RTTI, so no dynamic_cast)
std::mutex s_wheels_lock;
std::set<const Core::Device*> s_wheels;

class WheelDevice final : public Core::Device
{
public:
  WheelDevice(WGI::RawGameController raw, WGI::RacingWheel wheel)
      : m_raw(std::move(raw)), m_wheel(std::move(wheel))
  {
    try
    {
      m_nav = WGI::UINavigationController::FromGameController(m_raw);
      m_max_angle = m_wheel.MaxWheelAngle();
    }
    catch (winrt::hresult_error)
    {
    }

    AddInput(new Value(&m_steer, -1.0, "Left X-"));
    AddInput(new Value(&m_steer, +1.0, "Left X+"));
    AddInput(new Value(&m_zero, -1.0, "Left Y-"));
    AddInput(new Value(&m_zero, +1.0, "Left Y+"));
    AddInput(new Value(&m_zero, -1.0, "Right X-"));
    AddInput(new Value(&m_zero, +1.0, "Right X+"));
    AddInput(new Value(&m_zero, -1.0, "Right Y-"));
    AddInput(new Value(&m_zero, +1.0, "Right Y+"));
    AddInput(new Value(&m_brake, +1.0, "Trigger L"));
    AddInput(new Value(&m_throttle, +1.0, "Trigger R"));
    AddInput(new Value(&m_clutch, +1.0, "Clutch"));
    AddInput(new Value(&m_handbrake, +1.0, "Handbrake"));
    for (size_t i = 0; i < kButtonCount; ++i)
      AddInput(new Value(&m_buttons[i], +1.0, kButtonNames[i]));

    std::lock_guard lk(s_wheels_lock);
    s_wheels.insert(this);
  }

  ~WheelDevice() override
  {
    std::lock_guard lk(s_wheels_lock);
    s_wheels.erase(this);
  }

  std::string GetName() const override { return "Xbox One Game Controller"; }
  std::string GetSource() const override { return "WGInput"; }
  int GetSortPriority() const override { return 1; }

  const WGI::RawGameController& Raw() const { return m_raw; }
  double MaxAngle() const { return m_max_angle; }

  Core::DeviceRemoval UpdateInput() override
  {
    WGI::RacingWheelReading r{};
    WGI::UINavigationReading nav{};
    try
    {
      r = m_wheel.GetCurrentReading();
      if (m_nav)
        nav = m_nav.GetCurrentReading();
    }
    catch (winrt::hresult_error)
    {
      return Core::DeviceRemoval::Keep;
    }

    const double scale = m_max_angle > kStickLockDegrees ? m_max_angle / kStickLockDegrees : 1.0;
    m_steer = std::clamp(r.Wheel * scale, -1.0, 1.0);
    m_throttle = std::clamp(r.Throttle, 0.0, 1.0);
    m_brake = std::clamp(r.Brake, 0.0, 1.0);
    m_clutch = std::clamp(r.Clutch, 0.0, 1.0);
    m_handbrake = std::clamp(r.Handbrake, 0.0, 1.0);

    const auto has = [&r](WGI::RacingWheelButtons b) { return (r.Buttons & b) == b; };
    const auto req = [&](WGI::RequiredUINavigationButtons b) {
      return m_nav && (nav.RequiredButtons & b) == b;
    };
    const auto opt = [&](WGI::OptionalUINavigationButtons b) {
      return m_nav && (nav.OptionalButtons & b) == b;
    };
    using B = WGI::RacingWheelButtons;
    using R = WGI::RequiredUINavigationButtons;
    using O = WGI::OptionalUINavigationButtons;
    const bool pressed[kButtonCount] = {
        req(R::Accept),                              // Button A
        req(R::Cancel),                              // Button B
        opt(O::Context1),                            // Button X
        opt(O::Context2),                            // Button Y
        req(R::Menu),                                // Menu
        req(R::View),                                // View
        has(B::DPadUp) || req(R::Up),                // Pad N
        has(B::DPadDown) || req(R::Down),            // Pad S
        has(B::DPadLeft) || req(R::Left),            // Pad W
        has(B::DPadRight) || req(R::Right),          // Pad E
        has(B::PreviousGear) || opt(O::PageLeft),    // Bumper L: shift down
        has(B::NextGear) || opt(O::PageRight),       // Bumper R: shift up
        has(B::Button1),                             // Thumb L
        has(B::Button2),                             // Thumb R
    };
    for (size_t i = 0; i < kButtonCount; ++i)
      m_buttons[i] = pressed[i] ? 1.0 : 0.0;

    // reports: the first time anything is pressed, what the wheel sends (for wheels whose buttons land elsewhere)
    if (!m_logged_first_input &&
        (r.Buttons != B::None || r.Throttle > 0.5 || (m_nav && nav.RequiredButtons != R::None)))
    {
      m_logged_first_input = true;
      NOTICE_LOG_FMT(CONTROLLERINTERFACE,
                     "Game Vault: wheel first input: wheel {:.2f} throttle {:.2f} brake {:.2f} clutch {:.2f} "
                     "buttons 0x{:X} nav 0x{:X}/0x{:X}",
                     r.Wheel, r.Throttle, r.Brake, r.Clutch, static_cast<unsigned>(r.Buttons),
                     static_cast<unsigned>(nav.RequiredButtons), static_cast<unsigned>(nav.OptionalButtons));
    }
    return Core::DeviceRemoval::Keep;
  }

private:
  static constexpr size_t kButtonCount = 14;
  static constexpr std::string_view kButtonNames[kButtonCount] = {
      "Button A", "Button B", "Button X", "Button Y", "Menu",     "View",    "Pad N",
      "Pad S",    "Pad W",    "Pad E",    "Bumper L", "Bumper R", "Thumb L", "Thumb R"};

  class Value final : public Input
  {
  public:
    Value(const double* value, double sign, std::string_view name)
        : m_value(*value), m_sign(sign), m_name(name)
    {
    }
    std::string GetName() const override { return std::string(m_name); }
    ControlState GetState() const override { return std::max(0.0, m_value * m_sign); }

  private:
    const double& m_value;
    const double m_sign;
    const std::string_view m_name;
  };

  const WGI::RawGameController m_raw;
  const WGI::RacingWheel m_wheel;
  WGI::UINavigationController m_nav = nullptr;
  double m_max_angle = 0;
  double m_steer = 0, m_throttle = 0, m_brake = 0, m_clutch = 0, m_handbrake = 0;
  const double m_zero = 0;
  double m_buttons[kButtonCount] = {};
  bool m_logged_first_input = false;
};
}  // namespace

std::shared_ptr<Core::Device> TryCreate(const WGI::RawGameController& raw, const std::string& display_name)
{
  WGI::RacingWheel wheel = nullptr;
  try
  {
    wheel = WGI::RacingWheel::FromGameController(raw);
  }
  catch (winrt::hresult_error)
  {
    return nullptr;
  }
  if (!wheel)
    return nullptr;

  auto dev = std::make_shared<WheelDevice>(raw, wheel);
  std::string extra;
  try
  {
    extra = fmt::format(", motor {}, clutch {}, handbrake {}, shifter {}", wheel.WheelMotor() ? "yes" : "no",
                        wheel.HasClutch() ? "yes" : "no", wheel.HasHandbrake() ? "yes" : "no",
                        wheel.HasPatternShifter() ? "yes" : "no");
  }
  catch (winrt::hresult_error)
  {
  }
  u16 vid = 0, pid = 0;
  try
  {
    vid = raw.HardwareVendorId();
    pid = raw.HardwareProductId();
  }
  catch (winrt::hresult_error)
  {
  }
  NOTICE_LOG_FMT(CONTROLLERINTERFACE,
                 "Game Vault: wheel connected: {} ({:04X}:{:04X}), max angle {:.0f}{} - used as Xbox One Game "
                 "Controller (steering = left stick, {:.0f} degrees for a full stick)",
                 display_name.empty() ? "racing wheel" : display_name, vid, pid, dev->MaxAngle(), extra,
                 std::min(dev->MaxAngle() > 0 ? dev->MaxAngle() : kStickLockDegrees, kStickLockDegrees));
  return dev;
}

std::optional<bool> Matches(const Core::Device* dev, const WGI::RawGameController& raw)
{
  std::lock_guard lk(s_wheels_lock);
  const auto it = s_wheels.find(dev);
  if (it == s_wheels.end())
    return std::nullopt;
  return static_cast<const WheelDevice*>(dev)->Raw() == raw;
}
}  // namespace ciface::WGInput::GameVaultWheel
