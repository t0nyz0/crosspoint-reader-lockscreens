# CrossPoint Reader — Lock Screens Fork

> **This is a community fork of [crosspoint-reader/crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader)**, the excellent open-source e-reader firmware for Xteink X3/X4 devices. All credit for the core reader engine goes to the original CrossPoint team and contributors — this fork tracks their `develop` branch and adds one thing on top: **Lock Screens**, described below. Everything else works exactly like upstream CrossPoint.
>
> Not affiliated with the CrossPoint project, Xteink, or any device manufacturer.

**Now running on:** ESP32C3-based Xteink [X4](https://www.xteink.com/products/xteink-x4) and [X3](https://www.xteink.com/products/xteink-x3).

![CrossPoint Reader running on Xteink device](./docs/images/cover.jpg)

## What this fork adds: Lock Screens

A new **"Lock Screens"** folder on the home menu holds always-on, periodically-refreshing e-ink dashboards, inspired by TRMNL-style displays. Pick one, and it arms a timed deep sleep: the display stays visible using essentially zero power, waking briefly on its own schedule to refresh, then sleeping again.

- **GitHub Repo** — your GitHub contribution heatmap, current streak/longest streak, most-in-a-day, and average — pulled from your public profile, no token needed.
- **Weather** — current conditions (temp, feels-like, humidity, wind, rain chance) plus a 5-day forecast for a US ZIP code, via [Open-Meteo](https://open-meteo.com) (free, no API key or account).
- **Tempest** — live local readings from a [WeatherFlow Tempest](https://weatherflow.com/tempest-weather-system/) weather station: temperature with feels-like, wind speed/gust/lull with a compass dial, humidity, pressure with a 3-hour rising/falling/steady trend, dew point, UV index, illuminance, solar radiation, and lightning distance. Reads the station's local UDP broadcast directly — **no cloud account, API token, or internet dependency** for the station data itself.

Each dashboard has its own refresh interval (Settings → System → *GitHub/Weather/Tempest* Refresh Interval), and a failed refresh never blanks the display — it just keeps showing the last known reading and quietly retries next cycle.

Home → **Lock Screens** → pick one → enter your GitHub username / ZIP code / (optional) station label the first time. WiFi credentials are shared with the rest of CrossPoint (the same saved networks you already use for File Transfer, etc.).

---

## Quick flash

The easiest path — no computer tools required:

1. Download `firmware.bin` from the [latest release](https://github.com/t0nyz0/crosspoint-reader-lockscreens/releases/latest).
2. Copy `firmware.bin` to the root of your device's SD card.
3. Power the device off, then hold **Up** + **Power** while turning it back on to enter Recovery Mode.
4. Select `firmware.bin` from the on-screen list and confirm.

Prefer USB? See [Install firmware](#install-firmware) below for the web installer and command-line (`esptool`) methods — same steps as upstream CrossPoint, just point them at this fork's `firmware.bin` instead.

---

## What can CrossPoint do?

- **Reader engine**: EPUB 2/3 rendering with embedded-style option, image handling, hyphenation, kerning, chapter navigation, footnotes, bookmarks, go-to-percent, auto page turn, orientation control, focus reading, KOReader progress sync and more.

- **Various formats**: native handling for `.epub`, `.xtc/.xtch`, `.txt`, and `.bmp`.

- **Screenshots.**

- **Custom fonts**: install your favorite fonts on the SD card.

- **Tilt page turn (X3 only)**.

- **Library workflow**: folder browser, hidden-file toggle, long-press delete, recent books, SD-cache management.

- **Wireless workflows**:

  - File transfer web UI
  - EPUB Optimizer
  - Web settings UI/API (edit many device settings from browser)
  - WebSocket fast uploads
  - WebDAV handler
  - AP mode (hotspot) and STA mode (join existing Wi-Fi), both with QR helpers
  - Calibre wireless connect flow
  - OPDS browser with saved servers (up to 8), search, pagination, and direct download
  - OTA update checks and installs from GitHub releases

- **Customization**: multiple themes (Classic, Lyra, Lyra Extended, RoundedRaff), sleep screen modes, front/side button remapping, status bar controls, power-button behavior, refresh cadence, and more.

- **Localization**: 24 UI languages and counting. RTL support.

---

## USB-locked devices (Xteink Unlocker)

Some Xteink units purchased from third-party stores (e.g. AliExpress) ship with USB flashing locked from the factory.
If your device is locked, you will need to use the **Xteink Unlocker** tool available at
https://crosspointreader.com/#unlock-tool before you can flash any custom firmware, including this fork.

**You do not need this tool if you bought your device directly from xteink.com.** Those units are not locked.

**Not sure if your device is locked?** Power it on, connect the USB-C cable, and try flashing via the web flasher first (see
[Install firmware](#install-firmware) below). If the browser's serial device picker does not show your device, try a different
USB port or browser before assuming the device is locked. Only reach for the unlocker if the device still doesn't appear.

> ### ⚠️ WARNING: READ THIS BEFORE USING THE UNLOCKER ⚠️
>
> **The only officially supported firmwares in the unlock tool are CrossPoint and CrossInk.** This fork is not one of the
> officially supported options, so use the SD-card Recovery Mode method above if your device is locked, rather than
> attempting to flash this fork through the unlocker.
>
> Flashing any unsupported firmware on a USB-locked device may **permanently brick the device** or leave it **permanently
> stuck on that firmware with no recovery path**.

## Install firmware

### SD card (recommended for this fork)

See [Quick flash](#quick-flash) above.

### Web installer

1. Connect your device to your computer via USB-C and wake/unlock the device.
2. Go to https://crosspointreader.com/#flash-tools, select your device (X3 or X4), click **"Custom .bin"**, and upload
   `firmware.bin` from this fork's [releases page](https://github.com/t0nyz0/crosspoint-reader-lockscreens/releases).

### Revert to official CrossPoint

To go back to official upstream CrossPoint at any time, flash the latest official firmware using
https://crosspointreader.com/#flash-tools, or use its Recovery Mode `firmware.bin` from the
[upstream releases page](https://github.com/crosspoint-reader/crosspoint-reader/releases). Your books, settings, and SD
card contents are unaffected either way.

### Command line

1. Install [`esptool`](https://github.com/espressif/esptool):

```bash
pip install esptool
```

2. Download `firmware.bin` from this fork's [releases page](https://github.com/t0nyz0/crosspoint-reader-lockscreens/releases).
3. Connect your device via USB-C.
4. Find the device port. On Linux, run `dmesg` after connecting. On macOS:

```bash
log stream --predicate 'subsystem == "com.apple.iokit"' --info
```

5. Flash:

```bash
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 /path/to/firmware.bin
```

Adjust `/dev/ttyACM0` to match your system.

### Manual

See [Development quick start](#development-quick-start) below.

---

## Custom SD-card fonts

Convert your own TTF/OTF files into `.cpfont` files that load from the SD card. No firmware reflash is needed.

1. Go to https://crosspointreader.com/fonts and open the "SD-card font builder" form.
2. Upload up to four styles (regular, bold, italic, bold-italic), set the family name, point sizes, and Unicode range.
3. Download the generated `.cpfont` files.
4. Copy them to your SD card under `/fonts/YourFont/` (or `/.fonts/YourFont/` to hide the folder).
5. Select the font on the device from the font settings.

---

## Documentation

- [User Guide](./USER_GUIDE.md)
- [Web server usage](./docs/webserver.md)
- [Web server endpoints](./docs/webserver-endpoints.md)
- [Project scope](./SCOPE.md)
- [Contributing docs](./docs/contributing/README.md)

---

## Development quick start

### Prerequisites

- [pioarduino](https://github.com/pioarduino/pioarduino) or VS Code + pioarduino plugin
- Python 3.8+
- `clang-format` 21
- USB-C cable supporting data transfer

### Setup

```bash
git clone --recursive https://github.com/t0nyz0/crosspoint-reader-lockscreens
cd crosspoint-reader-lockscreens

# if cloned without --recursive:
git submodule update --init --recursive
```

### Build / flash / monitor

```bash
pio run --target upload
```

### Contributor pre-PR checks

```bash
./bin/clang-format-fix
pio check -e default
pio run -e default
```

---

## Internals

CrossPoint Reader is pretty aggressive about caching data down to the SD card to minimise RAM usage. The ESP32-C3 only has ~380KB of usable RAM, so we have to be careful. A lot of the decisions made in the design of the firmware were based on this constraint.

### Data caching

The first time chapters of a book are loaded, they are cached to the SD card. Subsequent loads are served from the
cache. This cache directory exists at `.crosspoint` on the SD card. The structure is as follows:

```text
.crosspoint/
├── epub_<hash>/         # one directory per book, named by content hash
│   ├── progress.bin     # reading position (chapter, page, etc.)
│   ├── cover.bmp        # generated cover image
│   ├── book.bin         # metadata: title, author, spine, TOC
│   ├── css_rules.cache  # parsed CSS rule cache
│   ├── img_*            # rendered image cache files
│   └── sections/        # per-chapter layout cache
│       ├── 0.bin
│       ├── 1.bin
│       └── ...
├── settings.json        # device settings
├── state.json           # resume/runtime state
└── recent.json          # recent books list
```

Removing `/.crosspoint` clears all cached metadata and forces a full regeneration on next open. Book deletes, overwrites, and moves done through the firmware or web UI clear or re-key matching caches; manual SD-card edits may leave stale cache directories behind.

For more details on the internal file structures, see the [file formats document](./docs/file-formats.md).

---

## Relationship to upstream

This fork tracks upstream CrossPoint's `develop` branch and merges upstream changes in periodically. Bugs and features
unrelated to Lock Screens should be reported/requested upstream at
[crosspoint-reader/crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) — this fork exists only to
carry the Lock Screens feature on top. Issues specific to Lock Screens (GitHub/Weather/Tempest dashboards) are welcome
here.

CrossPoint Reader is **not affiliated with Xteink or any device manufacturer**.

Huge shoutout to the CrossPoint maintainers and to [diy-esp32-epub-reader](https://github.com/atomic14/diy-esp32-epub-reader), which inspired the original project.
