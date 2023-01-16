# OrbisInstrumentalizer

A PlayStation 4 plugin to enable the use of cross-platform instruments on Guitar Hero Live and Rock Band 4.

Please make sure the controllers are connected *before* starting the game, to avoid any potential issues.

## Current Device Support

### Guitar Hero Live
* PS3/Wii U wireless dongle
* Xbox 360 wireless dongle

To use 2 dongles at once, you must have 2 profiles signed in on your PS4. (use the Switch User dialog to do this)

### Rock Band 4
* Xbox 360 wireless adapter (guitars only, only supports 1 controller per dongle)
* Xbox 360 wired instruments (untested, drums may not be mapped properly)
* Wii wired instruments and wireless dongles (untested, certain instruments may not be detected)

## How to install

This plugin has only been tested on firmware 9.00, but any firmware supported by the game version you're using and GoldHEN 2.3.0+ should work.

**!! Rock Band 4 support only works with version 02.21 (in-game: 2.3.7) of the game !!**

Go to the [GitHub releases page](https://github.com/InvoxiPlayGames/OrbisInstrumentalizer/releases) and download the latest OrbisInstrumentalizer.prx.

Copy the PRX to `/data/GoldHEN/plugins` then edit `/data/GoldHEN/plugins.ini` (create this file if it doesn't exist) and add the following lines:

```ini
; Guitar Hero Live (PAL)
[CUSA02410]
/data/GoldHEN/plugins/OrbisInstrumentalizer.prx
; Guitar Hero Live (USA)
[CUSA02188]
/data/GoldHEN/plugins/OrbisInstrumentalizer.prx
; Rock Band 4 (PAL)
[CUSA02901]
/data/GoldHEN/plugins/OrbisInstrumentalizer.prx
; Rock Band 4 (USA)
[CUSA02084]
/data/GoldHEN/plugins/OrbisInstrumentalizer.prx
```

If you run into any issues, [report them on the issue tracker](https://github.com/InvoxiPlayGames/OrbisInstrumentalizer/issues).

## TODO

In no particular order,

* [ALL] Fix whammy and tilt reporting on 360 guitars.
* [GHL] Device hotplugging support.
* [GHL] Xbox One guitar dongle support.
* [RB4] 360 wireless adapter instrument detection.
* [RB4] Ensure mapping of buttons is correct.
* [RB4] Set player numbers on 360 controllers. (stops eternal flashing)
* [RB4] Fill in all possible Wii instrument product IDs.
* [RB4] Fix sceUsbd itself not being able to be hooked.
* [RB4] (Maybe) Support multiple instruments with 360 wireless adapter.
* [ALL] (LOL NO) Support iOS/Wiimote guitars, either via OS or USB bluetooth dongle.
* If a new (compatible) plastic instrument game is ever made, support that as well.

## Building

Ensure you have the [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) and [GoldHEN Plugin SDK](https://github.com/GoldHEN/GoldHEN_Plugins_SDK) installed, with the `OO_PS4_TOOLCHAIN` and `GOLDHEN_SDK` environment variables set to their respective directories. Then just type `make` in the OrbisInstrumentalizer project directory.

## License

OrbisInstrumentalizer is licensed under the GNU Lesser General Public License version 2.1, or any later version at your choice.
