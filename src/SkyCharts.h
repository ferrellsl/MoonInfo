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
  };

  DayTrack dayTrack(double start, double end, const Observer & observer);

  struct ChartColours
  {
    COLORREF background, ground, grid, horizon, text, curve, dot;
  };

  ChartColours chartColours(bool dark);

  // GDI+, for the charts' smooth lines: started once, stopped at the end.
  void startCharts();
  void stopCharts();

  // Draw a chart filling the rectangle.  showNow: whether to mark the
  // position now (now, current); the track may be empty (no data yet).
  void drawAltitudeChart(HDC dc, const RECT & area, const DayTrack & track, bool showNow, double now,
                         const SkyPosition & current, const ChartColours & colours, HFONT font, double scale);
  void drawHorizonChart(HDC dc, const RECT & area, const DayTrack & track, bool showNow,
                        const SkyPosition & current, const ChartColours & colours, HFONT font, double scale);
}

#endif
