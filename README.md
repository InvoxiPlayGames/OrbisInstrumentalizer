# OrbisInstrumentalizer

A PlayStation 4 plugin to enable the use of cross-platform instruments on Guitar Hero Live and Rock Band 4.

Please make sure the controllers are connected *before* starting the game, to avoid any potential issues.

## Current Device Support

### Guitar Hero Live
* PS3/Wii U wireless dongle
* Xbox 360 wireless dongle

To use 2 dongles at once, you must have 2 profiles signed in on your PS4. (use the Switch User dialog to do this)

### Rock Band 4
* Xbox 360 wireless adapter (guitars and drums, up to 4 instruments per adapter)
* Xbox 360 wired instruments (guitars and drums)
* Wii wired instruments and wireless dongles (untested, certain instruments may not be detected)

Xbox 360 drums (Rock Band and Guitar Hero) support pads, Pro Drums cymbals, both kick pedals and velocity. Xbox 360 guitars (Rock Band and Guitar Hero) support the whammy, tilt, the solo frets and the pickup switch. The player LEDs follow the game's player numbers.

#### Xbox 360 wireless adapter

When the game starts, the plugin asks each wireless adapter what's linked to it, and tells the game it's a guitar or drums accordingly. **Turn your instrument on and let it link before starting the game.**

If an instrument links later, or a different kind of instrument takes over a slot, the plugin unplugs that slot from the game and plugs it back in as the right instrument, no need to touch the adapter.

## How to install

This plugin has only been tested on firmware 9.00, but any firmware supported by the game version you're using and GoldHEN 2.3.0+ should work.

**!! Rock Band 4 support has only been tested with version 02.21 (in-game: 2.3.7) of the game !!** Other versions are checked at startup: if the game's USB functions can't be found, the plugin shows a notification and does nothing.

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

## Settings

On the first run the plugin creates `/data/GoldHEN/OrbisInstrumentalizer.ini`, with the tilt, whammy and drum velocity thresholds explained inside. Edit it over FTP, then close and reopen the game to apply. The plugin also writes `/data/GoldHEN/OrbisInstrumentalizer.log`, which helps when reporting an issue.

## TODO

In no particular order,

* [GHL] Fix whammy and tilt reporting on 360 guitars.
* [GHL] Device hotplugging support.
* [GHL] Xbox One guitar dongle support.
* [RB4] Fill in all possible Wii instrument product IDs.
* [RB4] Fix sceUsbd itself not being able to be hooked.
* [ALL] (LOL NO) Support iOS/Wiimote guitars, either via OS or USB bluetooth dongle.

## Building

Ensure you have the [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) and [GoldHEN Plugin SDK](https://github.com/GoldHEN/GoldHEN_Plugins_SDK) installed, with the `OO_PS4_TOOLCHAIN` and `GOLDHEN_SDK` environment variables set to their respective directories. Then just type `make` in the OrbisInstrumentalizer project directory.

Alternatively, with Docker, build without installing anything (the toolchain and SDK are downloaded into the cache folder on the first run):

```sh
docker run --rm -v "$PWD:/work/OrbisInstrumentalizer" -v "$HOME/.cache/ps4:/cache" \
    ubuntu:24.04 bash /work/OrbisInstrumentalizer/tools/build-docker.sh
```

## License

OrbisInstrumentalizer is licensed under the GNU Lesser General Public License version 2.1, or any later version at your choice.
