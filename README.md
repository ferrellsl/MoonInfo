# MoonInfo

The Moon's phase, illumination, rise and set, the next quarters and its
position in the sky, with a picture of the Moon as it looks from your
location. A Windows version of the Moon Info web page.

![MoonInfo in dark mode](docs/mooninfo-dark.png)

## Download

Get **MoonInfo-1.0-win64.zip** from the
[Releases page](../../releases) and unzip it anywhere:

    MoonInfo.exe   the program (no installation; no DLLs needed)
    images\        the Moon pictures (keep this folder beside MoonInfo.exe)

Windows 10 or 11, 64-bit.

## Using it

- **Date and time:** with *Automatic* checked, the computer's clock,
  updated every second. Unchecked, type a local date and time,
  `YYYY-MM-DD HH:MM:SS`.
- **Location:** on the first run MoonInfo finds your location, and *Use My
  Location* finds it again. It asks Windows' location service first (if it's
  turned on for desktop apps in Settings > Privacy > Location), then, if
  that's off or doesn't answer, looks up the approximate location of your
  internet address at [ipinfo.io](https://ipinfo.io). Or type the latitude
  and longitude (degrees, north and east positive) and the elevation in
  metres.
- **Picture:** *As seen from my location* turns the picture by the Moon's
  parallactic angle (the angle between celestial north and straight up at
  the Moon), so it's tilted as it appears in your sky, roughly upside down in
  the southern hemisphere. Unchecked, it's shown north up.
- **Dark mode:** light text on a dark window, with a dark title bar. On the
  first run it follows Windows' app theme.

Times are in the computer's time zone. Settings are kept in
`%APPDATA%\MoonInfo\settings.ini`.

## Building

Visual Studio 2019 (x64) and CMake:

    cmake -S . -B build -G "Visual Studio 16 2019" -A x64
    cmake --build build --config Release

It needs [GraphApp](http://enchantia.com/graphapp/) (`GRAPHAPP_ROOT`,
default `C:/GraphApp`) and the Windows 10 SDK 10.0.19041 (for C++/WinRT's
`Windows.Devices.Geolocation`). The images aren't in this repository: copy
the `images` folder from the release zip into the project folder, and the
build copies it beside `MoonInfo.exe`.

`build/Release/mooninfo-test.exe` prints the calculations for a time and
place (`mooninfo-test <unix seconds> <latitude> <longitude>`), or tests
finding the location (`mooninfo-test location`).

## Support and contact

MoonInfo is free, open-source software provided as is: no support, no
warranty. You're welcome to fork it and change it.

To get in touch (questions, ideas, or just to say hello), post in
[Discussions](https://github.com/ferrellsl/MoonInfo/discussions).

## Licence and credits

MoonInfo: MIT licence, (c) 2026 Steve Ferrell ([LICENSE](LICENSE)).

- Calculations: [Astronomy Engine](https://github.com/cosinekitty/astronomy),
  (c) 2019-2023 Don Cross (MIT licence).
- Moon images: NASA's Scientific Visualization Studio.
- Window and controls: GraphApp, (c) L. Patrick.

See [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt) for their licences.
