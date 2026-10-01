# MoonInfo

The Moon's phase, illumination, distance, rise and set, the next new and
full moons, quarters and eclipses, and its position in the sky, updated every
second, with charts of its path through the day, a calendar of its phases,
and a picture of the Moon drawn as it looks at that moment from your
location, among the stars it's passing. A Windows version of the Moon Info web
page.

![MoonInfo in dark mode](docs/mooninfo-dark.png)

## Download

Get **MoonInfo-2.3-win64.zip** from the
[Releases page](../../releases) and unzip it anywhere:

    MoonInfo.exe   the program (no installation; no DLLs needed)
    data\          NASA's Moon maps and the stars (keep this folder beside MoonInfo.exe)

Windows 10 or 11, 64-bit. On Linux or macOS it runs under
[Wine](https://www.winehq.org/) (which has no Windows location service, so
it finds the location from your internet address).

## Using it

- **Date and time:** with *Automatic* checked, the computer's clock: every
  value is recalculated each second. Unchecked, type a local date and time,
  `YYYY-MM-DD HH:MM:SS`.
- **Time of day, < Day, Day >, Play day, Play month:** drag the slider to
  another time on the day shown, step a day back or forward, or run the time
  forward (a day in about six seconds, a month in about fifteen) and watch
  the Moon move, turn and change phase. The same button stops it; check
  *Automatic* to return to now.
- **Location:** on the first run MoonInfo finds your location, and *Use My
  Location* finds it again. It asks Windows' location service first (if it's
  turned on for desktop apps in Settings > Privacy > Location), then, if
  that's off or doesn't answer, looks up the approximate location of your
  internet address at [ipinfo.io](https://ipinfo.io). Or type the latitude
  and longitude (degrees, north and east positive) and the elevation.
- **Phase name:** New Moon, Waxing Crescent, First Quarter, Waxing Gibbous,
  Full Moon, Waning Gibbous, Third Quarter or Waning Crescent (the four
  principal phases within about half a day of the exact time).
- **Distance:** from the Earth's center to the Moon's.
- **Age:** days since the last new moon. **Libration:** how far the Moon is
  turned, east-west and north-south, from facing us squarely. **Perigee and
  apogee:** when the Moon is next nearest and farthest. A full moon closer
  than 367,600 km (228,400 miles) is marked as a supermoon.
- **Eclipses:** the next lunar eclipse (the time of its peak, its kind, and
  whether the Moon is above your horizon then), and the next solar eclipse
  visible from your location, with how much of the Sun is covered.
- **Charts:** at the bottom of the data column, the Moon's path through the
  day (midnight to midnight): its altitude by the hour, and its altitude by
  direction (east at the left, through south, west and north). The shaded
  part is below the horizon, and the dot is the Moon now. The small circles
  mark that day's moonrise (up arrow) and moonset (down arrow), with their
  times. (The Moonrise and Moonset rows show the next ones from now, which
  may be tomorrow's.) The dashed line is the Sun, and the sky in the first
  chart is colored by the daylight: day, twilight and night. The round
  chart is the sky looking up: the horizon around the edge (north at the
  top, east at the left), straight up in the middle.
- **Calendar:** a little Moon for each day of the month, ringed on the days
  of the new moon, quarters and full moon. Click a day to show it; the
  arrows show the months before and after.
- **Picture:** drawn from NASA's Lunar Reconnaissance Orbiter maps (the LROC
  color mosaic and LOLA elevations) for the date, time and place: the exact
  phase, lit from the Sun's direction, with the relief shaded near the
  terminator; the Moon's libration (the slight wobble that turns different
  edge features toward us) and its tilt; its apparent size, which changes
  with its distance; and earthshine faintly lighting the dark side. It's
  exact for any date, past or future.
- **View > Moon as seen from my location** turns the picture by the Moon's
  parallactic angle (the angle between celestial north and straight up at
  the Moon), so it's tilted as it appears in your sky, roughly upside down in
  the southern hemisphere. Unchecked, it's shown north up.
- **View > Names of the seas and craters** labels the picture (more names
  as it gets bigger).
- **View > Stars behind the Moon:** the stars the Moon is passing, where they
  really are, to about magnitude 12 (far more than its glare lets you see).
  The picture is only a little wider than the Moon, so there are usually
  just a few; with *Play day* you can watch them disappear behind it.
- **View > Miles and feet:** the distance in miles and the elevation in feet
  (unchecked: km and meters).
- **View > Dark mode:** light text on a dark window, with a dark title bar. On the
  first run it follows Windows' app theme.
- The picture grows and shrinks with the window; as you resize it, the
  window keeps the picture filling its right side. If the window is too short
  for all the data, scroll it with the scroll bar or the mouse wheel.

Times are in the computer's time zone. Settings are kept in
`%APPDATA%\MoonInfo\settings.ini`.

## Building

Visual Studio 2019 (x64) and CMake:

    cmake -S . -B build -G "Visual Studio 16 2019" -A x64
    cmake --build build --config Release

Plain Win32, with the Windows 10 SDK 10.0.19041 (for C++/WinRT's
`Windows.Devices.Geolocation`); no other libraries. NASA's maps aren't in
this repository: copy the `data` folder from the release zip into the
project folder, and the build copies it beside `MoonInfo.exe`. (The maps were
made from the CGI Moon Kit's `lroc_color_16bit_srgb_4k.tif`, as an 8-bit
JPEG, and `ldem_16_uint.tif`, resampled to 4096 x 2048 as a 16-bit PNG.)

`build/Release/mooninfo-test.exe` prints the calculations for a time and
place (`mooninfo-test <unix seconds> <latitude> <longitude>`), or tests
finding the location (`mooninfo-test location`), or draws the Moon to a BMP
file (`mooninfo-test render <unix seconds> <out.bmp> <size>`).

## Support and contact

MoonInfo is free, open-source software provided as is: no support, no
warranty. You're welcome to fork it and change it.

To get in touch (questions, ideas, or just to say hello), post in
[Discussions](https://github.com/ferrellsl/MoonInfo/discussions).

## License and credits

MoonInfo: MIT license, (c) 2026 Steve Ferrell ([LICENSE](LICENSE)).

- Calculations: [Astronomy Engine](https://github.com/cosinekitty/astronomy),
  (c) 2019-2023 Don Cross (MIT license).
- Stars: the [Tycho-2 catalogue](https://cdsarc.cds.unistra.fr/viz-bin/cat/I/259)
  (Hog et al. 2000), from ESA's Hipparcos mission, by way of VizieR (CDS,
  Strasbourg); `tools/make_stars.py` makes `data/moon_stars.bin` from it.
- Moon maps: NASA's Scientific Visualization Studio
  ([CGI Moon Kit](https://svs.gsfc.nasa.gov/4720)), from the Lunar
  Reconnaissance Orbiter's camera (LROC) and laser altimeter (LOLA) teams.

See [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt) for details.
