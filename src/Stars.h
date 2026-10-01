// MoonInfo: the stars behind the Moon.
//
// The stars the Moon can pass in front of (those within 7.5 degrees of the
// ecliptic), from the Tycho-2 catalogue: see tools/make_stars.py, which
// makes data/moon_stars.bin.
//
// Copyright 2026 Steve Ferrell.  Tycho-2: Hog et al. (2000), ESA.

#ifndef MOONINFO_STARS_H
#define MOONINFO_STARS_H

#include <cstdint>
#include <string>
#include <vector>

#include "MoonRender.h"

namespace mooninfo
{
  class StarCatalog
  {
    public:
      // Read moon_stars.bin.  Returns false if it can't be read.
#ifdef _WIN32
      bool load(const std::wstring & path);
#endif
      bool load(const std::string & path);
      bool ok() const { return ! stars_.empty(); }
      std::size_t count() const { return stars_.size(); }

      struct Star
      {
        float x, y, z;         // its direction, J2000 equatorial (a unit vector)
        float magnitude;       // V
        float colour;          // B-V
      };

      // The stars within a radius (degrees) of a direction.
      void within(const Vec3 & direction, double radius, std::vector<const Star *> & found) const;

    private:
      bool read(std::istream & in);
      std::vector<Star> stars_;            // sorted by right ascension
      std::vector<std::uint32_t> first_;   // the first star of each degree of right ascension (361 entries)
  };

  // Add the stars around the Moon to a picture renderMoon() drew (the same
  // size, geometry and angle), outside the Moon's disc.
  void drawStars(const StarCatalog & catalog, const MoonGeometry & geometry, int size, double angle,
                 std::vector<std::uint32_t> & pixels);
}

#endif
