// MoonInfo: drawing the Moon as it looks at a given time and place.
//
// The picture is rendered from NASA's LRO color and LOLA elevation maps:
// the disc at its apparent size, turned by its libration and axis tilt, lit
// from the Sun's direction (Lommel-Seeliger, as the lunar surface reflects),
// with the terrain's slopes shading the relief near the terminator.
//
// Copyright 2026 Steve Ferrell.

#ifndef MOONINFO_MOONRENDER_H
#define MOONINFO_MOONRENDER_H

#include <cstdint>
#include <string>
#include <vector>

#include "MoonCalc.h"

namespace mooninfo
{
  struct Vec3 { double x = 0, y = 0, z = 0; };

  // Where things are, as unit vectors in J2000 equatorial coordinates.
  struct MoonGeometry
  {
    Vec3 toMoon;                 // from the observer to the Moon
    Vec3 toSun;                  // from the Moon to the Sun
    Vec3 pole, prime, east90;    // the Moon's axes: north pole, 0 and 90 degrees east longitude
    double diameter = 0;         // apparent diameter, degrees
    double subEarthLon = 0, subEarthLat = 0;   // the centre of the disc, selenographic degrees
  };

  // The Moon's geometry at a time (UTC seconds) as seen from a place, or from
  // the Earth's centre if topocentric is false.
  MoonGeometry moonGeometry(double unixSeconds, const Observer & where, bool topocentric = true);

  // NASA's maps, longitude -180..180 (0 in the middle), latitude 90..-90.
  struct MoonMaps
  {
    int width = 0, height = 0;
    std::vector<std::uint32_t> color;     // BGRA, sRGB
    std::vector<std::uint16_t> elevation; // half-metres above 1727.4 km (LOLA "uint" encoding)
    bool ok() const { return width > 0 && ! color.empty(); }
  };

  // The Moon, size x size pixels (BGRA, top row first; black outside the
  // disc), north up turned clockwise by angle degrees (the parallactic angle
  // shows it as seen from the observer's location; 0: north up).
  void renderMoon(const MoonMaps & maps, const MoonGeometry & geometry, int size, double angle,
                  std::vector<std::uint32_t> & pixels);

#ifdef _WIN32
  // Read moon_color.jpg and moon_height.png (Windows Imaging Component; COM
  // must be initialized).  Returns false if they can't be read.
  bool loadMoonMaps(const std::wstring & folder, MoonMaps & maps);
#endif
}

#endif
