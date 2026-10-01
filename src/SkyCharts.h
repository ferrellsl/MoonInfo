// MoonInfo: charts of the Moon's path through a day - its altitude against
// the time, and against the direction (azimuth) - with its position now.
//
// Copyright 2026 Steve Ferrell.

#ifndef MOONINFO_SKYCHARTS_H
#define MOONINFO_SKYCHARTS_H

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vector>

#include "MoonCalc.h"

namespace mooninfo
{
  // The Moon's position through one local day.
  struct DayTrack
  {
    double start = 0, end = 0;               // UTC seconds: local midnight to the next
    std::vector<double> times;               // UTC seconds
    std::vector<SkyPosition> positions;
    std::vector<SkyPosition> sun;            // the Sun's, at the same times

    // The day's moonrises and moonsets, marked on the charts.
    struct Event
    {
      double time = 0;
      bool rise = false;
      SkyPosition position;                  // where the Moon is then
      SkyPosition below;                     // and ten minutes earlier (rise) or later (set), below the horizon
    };
    std::vector<Event> events;
  };

  DayTrack dayTrack(double start, double end, const Observer & observer);

  struct ChartColours
  {
    COLORREF background, ground, grid, horizon, text, curve, dot;
    COLORREF sun;              // the Sun's path and mark
    COLORREF day, night;       // the sky by day and by night (twilight is between them)
    COLORREF moonLit, moonDark;   // the calendar's little Moons
  };

  ChartColours chartColours(bool dark);

  // GDI+, for the charts' smooth lines: started once, stopped at the end.
  void startCharts();
  void stopCharts();

  // Draw a chart filling the rectangle.  showNow: whether to mark the
  // position now (now, current); the track may be empty (no data yet).
  void drawAltitudeChart(HDC dc, const RECT & area, const DayTrack & track, bool showNow, double now,
                         const SkyPosition & current, const ChartColours & colours, HFONT font, double scale);
  void drawHorizonChart(HDC dc, const RECT & area, const DayTrack & track, bool showNow, double now,
                        const SkyPosition & current, const ChartColours & colours, HFONT font, double scale);

  // The sky as a circle, looking up: the horizon around the edge (north at
  // the top, east at the left), the zenith in the middle, with the Moon's
  // and the Sun's paths while they're up.
  void drawSkyDome(HDC dc, const RECT & area, const DayTrack & track, bool showNow, double now,
                   const SkyPosition & current, const ChartColours & colours, HFONT font, double scale);

  // A month of Moons, for the calendar.
  struct CalendarMonth
  {
    int year = 0, month = 0;                 // month 1 - 12
    int days = 0, firstWeekday = 0;          // 0: the 1st is a Sunday
    std::vector<double> phase;               // each day's phase angle at local noon
    std::vector<int> quarter;                // each day's principal phase (0 - 3), or -1
    int selected = 0, today = 0;             // days of this month (0: not in it)
  };

  CalendarMonth calendarMonth(int year, int month);

  // mirrored: the Moons as the southern hemisphere sees them (lit on the
  // left while waxing).
  void drawCalendar(HDC dc, const RECT & area, const CalendarMonth & month, bool mirrored,
                    const ChartColours & colours, HFONT font, double scale);

  // What's at a point in the calendar: a day (1 - 31), calendarPrevious or
  // calendarNext (the arrows beside the month's name), or 0.
  const int calendarPrevious = -1, calendarNext = -2;
  int calendarHit(const RECT & area, const CalendarMonth & month, POINT point, double scale);
}

#endif
