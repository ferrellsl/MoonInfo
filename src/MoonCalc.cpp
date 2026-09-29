// MoonInfo: the Moon's phase, rise and set, quarters and position.
//
// The same calculations as the Moon Info website (index.htm), with Astronomy
// Engine's C version in place of astro.js.
//
// Copyright 2026 Steve Ferrell.

#include <cmath>

extern "C" {
#include "astronomy.h"
}

#include "MoonCalc.h"

namespace mooninfo
{
  namespace
  {
    const double pi = 3.14159265358979323846;
    const double toRad = pi / 180;

    // Astronomy Engine's time <-> seconds since 1970 (UTC).  Its ut is in
    // days since 2000-01-01 12:00 UTC.
    const double j2000Unix = 946728000.0;

    astro_time_t timeFromUnix(double seconds)
    {
      return Astronomy_TimeFromDays((seconds - j2000Unix) / 86400.0);
    }

    std::time_t unixFromTime(astro_time_t t)
    {
      return static_cast<std::time_t>(std::floor(t.ut * 86400.0 + j2000Unix + 0.5));
    }
  }

  const char * quarterName(int quarter)
  {
    static const char * names[4] = { "New Moon", "First Quarter", "Full Moon", "Third Quarter" };
    return names[quarter & 3];
  }

  MoonInfo calculate(double unixSeconds, const Observer & where)
  {
    MoonInfo info;
    astro_time_t time = timeFromUnix(unixSeconds);
    astro_observer_t observer = Astronomy_MakeObserver(where.latitude, where.longitude, where.elevation);

    astro_angle_result_t phase = Astronomy_MoonPhase(time);
    info.phase = phase.angle;
    astro_illum_t illum = Astronomy_Illumination(BODY_MOON, time);
    info.illumination = illum.phase_fraction;

    // Position: J2000 for RA and Dec (as the website shows), of date for the
    // horizon coordinates.
    astro_equatorial_t equ2000 = Astronomy_Equator(BODY_MOON, &time, observer, EQUATOR_J2000, ABERRATION);
    astro_equatorial_t equDate = Astronomy_Equator(BODY_MOON, &time, observer, EQUATOR_OF_DATE, ABERRATION);
    astro_horizon_t hor = Astronomy_Horizon(&time, observer, equDate.ra, equDate.dec, REFRACTION_NORMAL);
    info.ra = equ2000.ra;
    info.dec = equ2000.dec;
    info.azimuth = hor.azimuth;
    info.altitude = hor.altitude;

    // Parallactic angle: the angle at the Moon between celestial north and
    // the observer's zenith (positive when the Moon is west of the meridian).
    double hourAngle = (Astronomy_SiderealTime(&time) - equDate.ra) * 15 + where.longitude;
    info.parallactic = std::atan2(
      std::sin(hourAngle * toRad),
      std::tan(where.latitude * toRad) * std::cos(equDate.dec * toRad)
        - std::sin(equDate.dec * toRad) * std::cos(hourAngle * toRad)) / toRad;

    astro_search_result_t rise = Astronomy_SearchRiseSet(BODY_MOON, observer, DIRECTION_RISE, time, 300);
    astro_search_result_t set  = Astronomy_SearchRiseSet(BODY_MOON, observer, DIRECTION_SET, time, 300);
    info.riseFound = rise.status == ASTRO_SUCCESS;
    info.setFound = set.status == ASTRO_SUCCESS;
    if (info.riseFound) info.rise = unixFromTime(rise.time);
    if (info.setFound)  info.set = unixFromTime(set.time);

    astro_moon_quarter_t mq = Astronomy_SearchMoonQuarter(time);
    for (int i = 0; i < 4; ++i) {
      if (i > 0)
        mq = Astronomy_NextMoonQuarter(mq);
      info.quarters[i].quarter = mq.quarter;
      info.quarters[i].time = unixFromTime(mq.time);
    }

    // The website's frame: two per degree of phase, from frame 502.
    info.frame = static_cast<int>(std::trunc(info.phase * 2 + 502));
    return info;
  }
}
