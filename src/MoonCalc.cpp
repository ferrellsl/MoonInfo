// MoonInfo: the Moon's phase, rise and set, quarters and position.
//
// The same calculations as the Moon Info website (index.htm), with Astronomy
// Engine's C version in place of astro.js.
//
// Copyright 2026 Steve Ferrell.

#include <algorithm>
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

  const char * phaseName(double phase)
  {
    const double window = 6.0;   // degrees: the Moon moves ~12.2 degrees a day relative to the Sun
    phase = std::fmod(std::fmod(phase, 360.0) + 360.0, 360.0);
    if (phase < window || phase > 360 - window) return "New Moon";
    if (std::fabs(phase - 90) < window)         return "First Quarter";
    if (std::fabs(phase - 180) < window)        return "Full Moon";
    if (std::fabs(phase - 270) < window)        return "Third Quarter";
    if (phase < 90)                             return "Waxing Crescent";
    if (phase < 180)                            return "Waxing Gibbous";
    if (phase < 270)                            return "Waning Gibbous";
    return "Waning Crescent";
  }

  namespace
  {
    SkyPosition positionOf(astro_body_t body, double unixSeconds, const Observer & where);
  }

  SkyPosition skyPosition(double unixSeconds, const Observer & where)
  {
    return positionOf(BODY_MOON, unixSeconds, where);
  }

  SkyPosition sunPosition(double unixSeconds, const Observer & where)
  {
    return positionOf(BODY_SUN, unixSeconds, where);
  }

  Eclipses nextEclipses(double unixSeconds, const Observer & where)
  {
    Eclipses e;
    astro_time_t time = timeFromUnix(unixSeconds);
    astro_lunar_eclipse_t lunar = Astronomy_SearchLunarEclipse(time);
    if (lunar.status == ASTRO_SUCCESS) {
      e.lunarFound = true;
      e.lunarPeak = unixFromTime(lunar.peak);
      e.lunarKind = lunar.kind == ECLIPSE_TOTAL ? "total" : lunar.kind == ECLIPSE_PARTIAL ? "partial" : "penumbral";
      e.lunarVisible = skyPosition(double(e.lunarPeak), where).altitude > 0;
    }

    astro_observer_t observer = Astronomy_MakeObserver(where.latitude, where.longitude, where.elevation);
    astro_local_solar_eclipse_t solar = Astronomy_SearchLocalSolarEclipse(time, observer);
    for (int i = 0; i < 20 && solar.status == ASTRO_SUCCESS; ++i) {
      if (solar.peak.altitude > 0 || solar.partial_begin.altitude > 0 || solar.partial_end.altitude > 0) {
        e.solarFound = true;
        e.solarPeak = unixFromTime(solar.peak.time);
        e.solarKind = solar.kind == ECLIPSE_TOTAL ? "total" : solar.kind == ECLIPSE_ANNULAR ? "annular" : "partial";
        e.solarObscuration = solar.obscuration;
        e.solarPeakSun = solar.peak.altitude > 0 ? 0 : solar.partial_end.altitude > 0 ? 1 : 2;
        break;
      }
      solar = Astronomy_NextLocalSolarEclipse(solar.peak.time, observer);   // (the Sun is down for that one)
    }
    return e;
  }

  std::vector<MoonQuarter> quartersBetween(double start, double end)
  {
    std::vector<MoonQuarter> found;
    astro_moon_quarter_t mq = Astronomy_SearchMoonQuarter(timeFromUnix(start));
    while (mq.status == ASTRO_SUCCESS && found.size() < 16) {
      double when = mq.time.ut * 86400.0 + j2000Unix;
      if (when > end)
        break;
      MoonQuarter q;
      q.quarter = mq.quarter;
      q.time = unixFromTime(mq.time);
      found.push_back(q);
      mq = Astronomy_NextMoonQuarter(mq);
    }
    return found;
  }

  double phaseAt(double unixSeconds)
  {
    return Astronomy_MoonPhase(timeFromUnix(unixSeconds)).angle;
  }

  namespace
  {
  SkyPosition positionOf(astro_body_t body, double unixSeconds, const Observer & where)
  {
    astro_time_t time = timeFromUnix(unixSeconds);
    astro_observer_t observer = Astronomy_MakeObserver(where.latitude, where.longitude, where.elevation);
    astro_equatorial_t equDate = Astronomy_Equator(body, &time, observer, EQUATOR_OF_DATE, ABERRATION);
    astro_horizon_t hor = Astronomy_Horizon(&time, observer, equDate.ra, equDate.dec, REFRACTION_NORMAL);
    SkyPosition p;
    p.azimuth = hor.azimuth;
    p.altitude = hor.altitude;
    return p;
  }
  }

  std::vector<HorizonEvent> horizonEvents(double start, double end, const Observer & where)
  {
    std::vector<HorizonEvent> events;
    astro_observer_t observer = Astronomy_MakeObserver(where.latitude, where.longitude, where.elevation);
    for (int pass = 0; pass < 2; ++pass) {
      astro_direction_t direction = pass == 0 ? DIRECTION_RISE : DIRECTION_SET;
      double from = start;
      while (from < end && events.size() < 8) {
        astro_search_result_t found = Astronomy_SearchRiseSet(BODY_MOON, observer, direction, timeFromUnix(from),
                                                              (end - from) / 86400.0);
        if (found.status != ASTRO_SUCCESS)
          break;
        double when = found.time.ut * 86400.0 + j2000Unix;
        if (when > end)
          break;
        HorizonEvent e;
        e.time = when;
        e.rise = pass == 0;
        events.push_back(e);
        from = when + 600;   // (the next one is hours later)
      }
    }
    std::sort(events.begin(), events.end(), [](const HorizonEvent & a, const HorizonEvent & b) { return a.time < b.time; });
    return events;
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
    info.distance = Astronomy_VectorLength(Astronomy_GeoMoon(time)) * KM_PER_AU;

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

    // The age: since the last new moon (searching forward from 35 days back).
    astro_time_t from = timeFromUnix(unixSeconds - 35 * 86400.0);
    double lastNew = unixSeconds;
    for (int i = 0; i < 3; ++i) {
      astro_search_result_t found = Astronomy_SearchMoonPhase(0.0, from, 40);
      if (found.status != ASTRO_SUCCESS)
        break;
      double when = found.time.ut * 86400.0 + j2000Unix;
      if (when > unixSeconds)
        break;
      lastNew = when;
      from = Astronomy_AddDays(found.time, 1);
    }
    info.age = (unixSeconds - lastNew) / 86400.0;

    astro_constellation_t con = Astronomy_Constellation(equ2000.ra, equ2000.dec);
    if (con.status == ASTRO_SUCCESS && con.name)
      info.constellation = con.name;

    // The next perigee and apogee (they alternate).
    astro_apsis_t apsis = Astronomy_SearchLunarApsis(time);
    for (int i = 0; i < 2 && apsis.status == ASTRO_SUCCESS; ++i) {
      if (apsis.kind == APSIS_PERICENTER) {
        info.perigee = unixFromTime(apsis.time);
        info.perigeeDistance = apsis.dist_km;
      }
      else {
        info.apogee = unixFromTime(apsis.time);
        info.apogeeDistance = apsis.dist_km;
      }
      apsis = Astronomy_NextLunarApsis(apsis);
    }

    for (int i = 0; i < 4; ++i)
      if (info.quarters[i].quarter == 2) {
        astro_time_t full = timeFromUnix(double(info.quarters[i].time));
        info.nextFullIsSupermoon = Astronomy_VectorLength(Astronomy_GeoMoon(full)) * KM_PER_AU < supermoonKm;
      }

    // The website's frame: two per degree of phase, from frame 502.
    info.frame = static_cast<int>(std::trunc(info.phase * 2 + 502));
    return info;
  }
}
