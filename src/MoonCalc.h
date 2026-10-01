// MoonInfo: the Moon's phase, rise and set, quarters and position, as the
// Moon Info website shows them.
//
// Copyright 2026 Steve Ferrell.  Calculations: Astronomy Engine (C),
// (c) 2019-2023 Don Cross, MIT licence.

#ifndef MOONINFO_MOONCALC_H
#define MOONINFO_MOONCALC_H

#include <ctime>
#include <string>

namespace mooninfo
{
  struct Observer
  {
    double latitude = 30, longitude = -90, elevation = 0;   // degrees, degrees, metres
  };

  struct MoonQuarter
  {
    int quarter = 0;         // 0 new moon, 1 first quarter, 2 full moon, 3 third quarter
    std::time_t time = 0;
  };

  struct MoonInfo
  {
    double phase = 0;          // phase angle, degrees (0 new, 90 first quarter, 180 full, 270 third quarter)
    double illumination = 0;   // fraction of the disc lit, 0 - 1
    bool riseFound = false, setFound = false;
    std::time_t rise = 0, set = 0;   // the next moonrise and moonset
    MoonQuarter quarters[4];         // the next four quarters
    double azimuth = 0, altitude = 0;   // degrees, with refraction
    double parallactic = 0;    // degrees: turning the north-up image clockwise by this shows it as seen from here
    double ra = 0, dec = 0;    // J2000, hours and degrees
    double distance = 0;       // Earth's centre to the Moon's centre, km
    int frame = 0;             // the Moon Info website's image frame for the phase (moon.NNNN.jpg)
  };

  // Everything for the given time (UTC seconds) and place.
  MoonInfo calculate(double unixSeconds, const Observer & observer);

  // Where the Moon is in the sky (degrees; altitude with refraction), as in
  // calculate(), for drawing its path through a day.
  struct SkyPosition
  {
    double azimuth = 0, altitude = 0;
  };
  SkyPosition skyPosition(double unixSeconds, const Observer & observer);

  const char * quarterName(int quarter);

  // "Waxing Crescent", "First Quarter", ... "Waning Crescent": the principal
  // phases within about half a day (6 degrees) of the exact time.
  const char * phaseName(double phase);
}

#endif
