/* Copyright (c) 2026 Skitzelles - part of Game Vault, https://github.com/skitzellez/xbox-game-vault
 * GPL-2.0-or-later, like Dolphin.
 *
 * Racing wheels on the Xbox (Windows.Gaming.Input.RacingWheel). See GameVaultWheel.cpp.
 */
#pragma once

#include <memory>
#include <optional>
#include <string>

#include <winrt/windows.gaming.input.h>

#include "InputCommon/ControllerInterface/CoreDevice.h"

namespace ciface::WGInput::GameVaultWheel
{
// a device that reads this controller as a racing wheel, or null when it is not one
std::shared_ptr<Core::Device> TryCreate(const winrt::Windows::Gaming::Input::RawGameController& raw,
                                        const std::string& display_name);

// for a wheel device made here: whether it reads this controller; nullopt for any other device
std::optional<bool> Matches(const Core::Device* dev,
                            const winrt::Windows::Gaming::Input::RawGameController& raw);
}  // namespace ciface::WGInput::GameVaultWheel
