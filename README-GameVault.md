# Dolphin for Game Vault

This is the Xbox (UWP) Dolphin from [XboxEmulationHub/dolphin](https://github.com/XboxEmulationHub/dolphin), built for
[Game Vault](https://github.com/skitzellez/xbox-game-vault) on Xbox Series X|S and Xbox One in Dev Mode. Dolphin is
licensed under the GNU GPL v2 or later (see `COPYING`); these changes are under the same licence.

Changes on top of XboxEmulationHub/dolphin master (branch `gamevault`):

- **Games started from a link no longer crash.** `Source/DolphinWinRT/main.cpp`: `Run()` waits for the activation
  (up to 5 s) before choosing between the menu and a game. Before, a link's game often arrived while the menu was
  already running, and booting it shut down the video backend the menu still drew with.
- **The video backend stays up for a game started without the menu.** `Source/Core/Core/Core.cpp` (`EmuThread`): the
  guard that shuts video down at the end of emulation was declared inside an `if` block, so it ran right after video
  started; the first draw then crashed (null `g_vertex_manager` in `VertexManagerBase::Flush`).
- Own package identity (`GamerDad.DolphinForGameVault`) and link `gamevault-dolphin:`, so it sits next to the
  original Dolphin; trace lines ("Game Vault trace") in the Core log.
- Build fixes: the SDL submodule points at XboxEmulationHub/SDL3-uwp (where its commit lives); lunasvg (its commit is
  gone from every remote, and nothing in the code uses it) is left out.

Build: Visual Studio 2022 with the UWP workload, Windows SDK 10.0.26100. Copy `Data\Sys` to
`Source\DolphinWinRT\Sys`, then `nuget restore Source/dolphin-emu.sln` and
`msbuild Source/dolphin-emu.sln /t:DolphinWinRT /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143`.

GameCube and Wii are trademarks of Nintendo. Dolphin is not affiliated with Nintendo. No game, BIOS or key is included.
