// MoonInfo: charts of the Moon's path through a day.
//
// Copyright 2026 Steve Ferrell.

#include <algorithm>
#include <cmath>
#include <ctime>
#include <string>

#include "SkyCharts.h"

// GDI+ uses min and max, which NOMINMAX leaves out.
#include <objidl.h>
namespace Gdiplus
{
  using std::min;
  using std::max;
}
#include <gdiplus.h>

namespace mooninfo
{
  namespace
  {
    ULONG_PTR gdiplusToken = 0;

    Gdiplus::Color colour(COLORREF c, BYTE alpha = 255)
    {
      return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
    }

    // The plot inside a chart: room for the labels on the left and below.
    struct Plot
    {
      float left, top, right, bottom;
      float x(double fraction) const { return left + float(fraction) * (right - left); }
      float y(double altitude) const { return top + float((90 - altitude) / 180) * (bottom - top); }
    };

    // The background, the ground (below the horizon), the altitude grid
    // and its labels, common to both charts.
    Plot drawFrame(Gdiplus::Graphics & g, const RECT & area, const ChartColours & c, Gdiplus::Font & font,
                   float textHeight, double scale)
    {
      Gdiplus::SolidBrush background(colour(c.background));
      g.FillRectangle(&background, Gdiplus::Rect(area.left, area.top, area.right - area.left, area.bottom - area.top));

      Plot p;
      p.left = float(area.left) + textHeight * 3.2f;
      p.right = float(area.right) - float(10 * scale);
      p.top = float(area.top) + textHeight * 0.6f;
      p.bottom = float(area.bottom) - textHeight * 1.5f;

      Gdiplus::SolidBrush ground(colour(c.ground));
      g.FillRectangle(&ground, Gdiplus::RectF(p.left, p.y(0), p.right - p.left, p.bottom - p.y(0)));

      Gdiplus::Pen grid(colour(c.grid), float(1 * scale));
      Gdiplus::SolidBrush text(colour(c.text));
      Gdiplus::StringFormat right;
      right.SetAlignment(Gdiplus::StringAlignmentFar);
      right.SetLineAlignment(Gdiplus::StringAlignmentCenter);
      right.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
      for (int alt = -90; alt <= 90; alt += 30) {
        float y = p.y(alt);
        if (alt != 0)
          g.DrawLine(&grid, p.left, y, p.right, y);
        std::wstring label = std::to_wstring(alt) + wchar_t(0x00B0);   // degree sign
        g.DrawString(label.c_str(), -1, &font, Gdiplus::RectF(float(area.left), y - textHeight, p.left - float(5 * scale),
                     2 * textHeight), &right, &text);
      }
      return p;
    }

    // Vertical grid lines, labeled below the plot.
    void drawColumns(Gdiplus::Graphics & g, const Plot & p, const ChartColours & c, Gdiplus::Font & font,
                     float textHeight, double scale, int count, const wchar_t * const * labels)
    {
      Gdiplus::Pen grid(colour(c.grid), float(1 * scale));
      Gdiplus::SolidBrush text(colour(c.text));
      // Centered under their lines, except at the ends, which are aligned
      // with the plot's edges (clear of the altitude labels and the edge).
      Gdiplus::StringFormat centre, left, right;
      centre.SetAlignment(Gdiplus::StringAlignmentCenter);
      left.SetAlignment(Gdiplus::StringAlignmentNear);
      right.SetAlignment(Gdiplus::StringAlignmentFar);
      for (int i = 0; i <= count; ++i) {
        float x = p.x(double(i) / count);
        if (i > 0 && i < count)
          g.DrawLine(&grid, x, p.top, x, p.bottom);
        if (! labels[i] || ! *labels[i])
          continue;
        float y = p.bottom + float(3 * scale), w = 6 * textHeight, h = 1.4f * textHeight;
        if (i == 0)
          g.DrawString(labels[i], -1, &font, Gdiplus::RectF(x, y, w, h), &left, &text);
        else if (i == count)
          g.DrawString(labels[i], -1, &font, Gdiplus::RectF(x - w, y, w, h), &right, &text);
        else
          g.DrawString(labels[i], -1, &font, Gdiplus::RectF(x - w / 2, y, w, h), &centre, &text);
      }
    }

    // The horizon (0 degrees) and the plot's border, over the grid.
    void drawHorizonLine(Gdiplus::Graphics & g, const Plot & p, const ChartColours & c, double scale)
    {
      Gdiplus::Pen horizon(colour(c.horizon), float(1.5 * scale));
      g.DrawLine(&horizon, p.left, p.y(0), p.right, p.y(0));
      Gdiplus::Pen border(colour(c.grid), float(1 * scale));
      g.DrawRectangle(&border, p.left, p.top, p.right - p.left, p.bottom - p.top);
    }

    void drawDot(Gdiplus::Graphics & g, float x, float y, const ChartColours & c, double scale)
    {
      float r = float(5 * scale);
      Gdiplus::SolidBrush fill(colour(c.dot));
      Gdiplus::Pen ring(colour(c.background), float(1.5 * scale));
      g.FillEllipse(&fill, x - r, y - r, 2 * r, 2 * r);
      g.DrawEllipse(&ring, x - r, y - r, 2 * r, 2 * r);
    }

    // A moonrise or moonset: a mark on the curve and its time, beside it in
    // the shaded part, on the side away from the curve below the horizon
    // (belowX: where the curve is just below the horizon).
    void drawEvent(Gdiplus::Graphics & g, const Plot & p, const DayTrack::Event & e, float x, float belowX,
                   const ChartColours & c, Gdiplus::Font & font, float textHeight, double scale)
    {
      float y = p.y(e.position.altitude), r = float(3.5 * scale);
      Gdiplus::SolidBrush fill(colour(c.background));
      Gdiplus::Pen ring(colour(c.text), float(1.5 * scale));
      g.FillEllipse(&fill, x - r, y - r, 2 * r, 2 * r);
      g.DrawEllipse(&ring, x - r, y - r, 2 * r, 2 * r);

      // An up or down arrow and the local time, "HH:MM".
      std::time_t when = static_cast<std::time_t>(std::floor(e.time + 0.5));
      std::tm local;
      if (localtime_s(&local, &when) != 0)
        return;
      wchar_t clock[16];
      std::wcsftime(clock, 16, L"%H:%M", &local);
      std::wstring label = std::wstring(1, wchar_t(e.rise ? 0x2191 : 0x2193)) + L" " + clock;

      Gdiplus::RectF box;
      g.MeasureString(label.c_str(), -1, &font, Gdiplus::PointF(0, 0), &box);
      float gap = float(5 * scale);
      bool toRight = belowX <= x, flipped = false;
      if (toRight && x + gap + box.Width > p.right)        { toRight = false; flipped = true; }
      else if (! toRight && x - gap - box.Width < p.left)  { toRight = true;  flipped = true; }
      float left = toRight ? x + gap : x - gap - box.Width;
      // With no room on that side it goes on the other, above the horizon
      // (where the curve below the horizon isn't).
      float top = flipped ? p.y(0) - box.Height - float(1 * scale) : p.y(0) + float(2 * scale);
      Gdiplus::SolidBrush text(colour(c.text));
      g.DrawString(label.c_str(), -1, &font, Gdiplus::PointF(left, top), &text);
    }

    // The horizon chart's x: east at the left, through south, west and
    // north, to east again at the right.
    double azimuthFraction(double azimuth)
    {
      double a = std::fmod(azimuth - 90 + 720, 360.0);
      return a / 360;
    }

    struct Painter
    {
      Gdiplus::Graphics g;
      Gdiplus::Font font;
      float textHeight;
      Painter(HDC dc, HFONT f) : g(dc), font(dc, f)
      {
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
        textHeight = font.GetHeight(&g);
      }
    };
  }

  //---------------------------------------------------------------------------

  DayTrack dayTrack(double start, double end, const Observer & observer)
  {
    DayTrack track;
    track.start = start;
    track.end = end;
    const double step = 300;   // five minutes
    for (double t = start; t < end + step / 2; t += step) {
      double when = std::min(t, end);
      track.times.push_back(when);
      track.positions.push_back(skyPosition(when, observer));
    }
    for (const HorizonEvent & e : horizonEvents(start, end, observer)) {
      DayTrack::Event event;
      event.time = e.time;
      event.rise = e.rise;
      event.position = skyPosition(e.time, observer);
      event.below = skyPosition(e.time + (e.rise ? -600 : 600), observer);
      track.events.push_back(event);
    }
    return track;
  }

  ChartColours chartColours(bool dark)
  {
    if (dark)
      return { RGB(18, 20, 28), RGB(30, 32, 40), RGB(78, 84, 102), RGB(165, 170, 185),
               RGB(200, 205, 215), RGB(130, 180, 255), RGB(255, 214, 102) };
    return { RGB(255, 255, 255), RGB(243, 239, 231), RGB(205, 208, 216), RGB(110, 115, 130),
             RGB(70, 75, 90), RGB(35, 95, 190), RGB(215, 70, 40) };
  }

  void startCharts()
  {
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&gdiplusToken, &input, NULL);
  }

  void stopCharts()
  {
    if (gdiplusToken)
      Gdiplus::GdiplusShutdown(gdiplusToken);
    gdiplusToken = 0;
  }

  void drawAltitudeChart(HDC dc, const RECT & area, const DayTrack & track, bool showNow, double now,
                         const SkyPosition & current, const ChartColours & c, HFONT hfont, double scale)
  {
    Painter painter(dc, hfont);
    Gdiplus::Graphics & g = painter.g;
    Plot p = drawFrame(g, area, c, painter.font, painter.textHeight, scale);
    static const wchar_t * const hours[] = { L"0h", L"", L"6h", L"", L"12h", L"", L"18h", L"", L"24h" };
    drawColumns(g, p, c, painter.font, painter.textHeight, scale, 8, hours);
    drawHorizonLine(g, p, c, scale);
    if (track.times.size() < 2)
      return;

    double span = track.end - track.start;
    std::vector<Gdiplus::PointF> points;
    for (std::size_t i = 0; i < track.times.size(); ++i)
      points.emplace_back(p.x((track.times[i] - track.start) / span), p.y(track.positions[i].altitude));
    Gdiplus::Pen curve(colour(c.curve), float(2.2 * scale));
    curve.SetLineJoin(Gdiplus::LineJoinRound);
    g.DrawLines(&curve, points.data(), int(points.size()));
    for (const DayTrack::Event & e : track.events) {
      float x = p.x((e.time - track.start) / span);
      drawEvent(g, p, e, x, e.rise ? x - 1 : x + 1, c, painter.font, painter.textHeight, scale);
    }
    if (showNow && now >= track.start && now <= track.end)
      drawDot(g, p.x((now - track.start) / span), p.y(current.altitude), c, scale);
  }

  void drawHorizonChart(HDC dc, const RECT & area, const DayTrack & track, bool showNow,
                        const SkyPosition & current, const ChartColours & c, HFONT hfont, double scale)
  {
    Painter painter(dc, hfont);
    Gdiplus::Graphics & g = painter.g;
    Plot p = drawFrame(g, area, c, painter.font, painter.textHeight, scale);
    static const wchar_t * const directions[] = { L"E", L"SE", L"S", L"SW", L"W", L"NW", L"N", L"NE", L"E" };
    drawColumns(g, p, c, painter.font, painter.textHeight, scale, 8, directions);
    drawHorizonLine(g, p, c, scale);
    if (track.times.size() < 2)
      return;

    // The path, broken where it crosses east (from one edge to the other).
    Gdiplus::Pen curve(colour(c.curve), float(2.2 * scale));
    curve.SetLineJoin(Gdiplus::LineJoinRound);
    std::vector<Gdiplus::PointF> points;
    double last = -1;
    for (std::size_t i = 0; i < track.positions.size(); ++i) {
      double f = azimuthFraction(track.positions[i].azimuth);
      if (last >= 0 && std::fabs(f - last) > 0.5) {
        if (points.size() > 1)
          g.DrawLines(&curve, points.data(), int(points.size()));
        points.clear();
      }
      points.emplace_back(p.x(f), p.y(track.positions[i].altitude));
      last = f;
    }
    if (points.size() > 1)
      g.DrawLines(&curve, points.data(), int(points.size()));
    for (const DayTrack::Event & e : track.events)
      drawEvent(g, p, e, p.x(azimuthFraction(e.position.azimuth)), p.x(azimuthFraction(e.below.azimuth)), c,
                painter.font, painter.textHeight, scale);
    if (showNow)
      drawDot(g, p.x(azimuthFraction(current.azimuth)), p.y(current.altitude), c, scale);
  }
}
