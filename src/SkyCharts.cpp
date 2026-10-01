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
    COLORREF blend(COLORREF a, COLORREF b, double t)
    {
      auto mix = [t](int x, int y) { return int(x + (y - x) * t + 0.5); };
      return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
    }

    // The sky's colour for the Sun's altitude: day, the three twilights
    // (civil, nautical and astronomical, each 6 degrees), then night.
    COLORREF skyColour(double sunAltitude, const ChartColours & c)
    {
      double t = sunAltitude > -0.833 ? 0 : sunAltitude > -6 ? 0.4 : sunAltitude > -12 ? 0.65
                 : sunAltitude > -18 ? 0.85 : 1;
      return blend(c.day, c.night, t);
    }

    // A value of the track at a time, between its samples.
    SkyPosition positionAt(const DayTrack & track, const std::vector<SkyPosition> & values, double when)
    {
      SkyPosition p;
      if (values.empty() || track.times.size() != values.size())
        return p;
      if (when <= track.times.front()) return values.front();
      if (when >= track.times.back())  return values.back();
      std::size_t i = std::size_t(std::upper_bound(track.times.begin(), track.times.end(), when) - track.times.begin());
      double t0 = track.times[i - 1], t1 = track.times[i], f = t1 > t0 ? (when - t0) / (t1 - t0) : 0;
      double a0 = values[i - 1].azimuth, a1 = values[i].azimuth;
      if (a1 - a0 > 180) a0 += 360;
      if (a0 - a1 > 180) a1 += 360;
      p.azimuth = std::fmod(a0 + (a1 - a0) * f, 360.0);
      p.altitude = values[i - 1].altitude + (values[i].altitude - values[i - 1].altitude) * f;
      return p;
    }

    // sky: the day whose daylight, twilight and night shade the part above
    // the horizon (the altitude chart), or NULL.
    Plot drawFrame(Gdiplus::Graphics & g, const RECT & area, const ChartColours & c, Gdiplus::Font & font,
                   float textHeight, double scale, const DayTrack * sky = NULL)
    {
      Gdiplus::SolidBrush background(colour(c.background));
      g.FillRectangle(&background, Gdiplus::Rect(area.left, area.top, area.right - area.left, area.bottom - area.top));

      Plot p;
      p.left = float(area.left) + textHeight * 3.2f;
      p.right = float(area.right) - float(10 * scale);
      p.top = float(area.top) + textHeight * 0.6f;
      p.bottom = float(area.bottom) - textHeight * 1.5f;

      if (sky && sky->sun.size() > 1 && sky->sun.size() == sky->times.size()) {
        g.SetSmoothingMode(Gdiplus::SmoothingModeNone);   // (no seams between the strips)
        double span = sky->end - sky->start;
        for (std::size_t i = 0; i + 1 < sky->times.size(); ++i) {
          float x0 = p.x((sky->times[i] - sky->start) / span), x1 = p.x((sky->times[i + 1] - sky->start) / span);
          Gdiplus::SolidBrush strip(colour(skyColour((sky->sun[i].altitude + sky->sun[i + 1].altitude) / 2, c)));
          g.FillRectangle(&strip, Gdiplus::RectF(x0, p.top, x1 - x0 + 1, p.y(0) - p.top));
        }
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
      }

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

    // The Sun: a small disc with a ring.
    void drawSun(Gdiplus::Graphics & g, float x, float y, const ChartColours & c, double scale)
    {
      float r = float(4 * scale);
      Gdiplus::SolidBrush fill(colour(c.sun));
      Gdiplus::Pen ring(colour(c.background), float(1 * scale));
      g.FillEllipse(&fill, x - r, y - r, 2 * r, 2 * r);
      g.DrawEllipse(&ring, x - r, y - r, 2 * r, 2 * r);
    }

    // "Moon" and "Sun" with their lines, at the plot's top right corner.
    void drawLegend(Gdiplus::Graphics & g, const Plot & p, const ChartColours & c, Gdiplus::Font & font,
                    float textHeight, double scale)
    {
      Gdiplus::SolidBrush text(colour(c.text));
      Gdiplus::Pen moon(colour(c.curve), float(2.2 * scale)), sun(colour(c.sun), float(1.6 * scale));
      sun.SetDashStyle(Gdiplus::DashStyleDash);
      Gdiplus::RectF moonBox, sunBox;
      g.MeasureString(L"Moon", -1, &font, Gdiplus::PointF(0, 0), &moonBox);
      g.MeasureString(L"Sun", -1, &font, Gdiplus::PointF(0, 0), &sunBox);
      float line = float(18 * scale), gap = float(4 * scale), y = p.top + float(3 * scale);
      float x = p.right - float(6 * scale) - sunBox.Width;
      g.DrawString(L"Sun", -1, &font, Gdiplus::PointF(x, y), &text);
      g.DrawLine(&sun, x - gap - line, y + textHeight / 2, x - gap, y + textHeight / 2);
      x -= gap + line + float(8 * scale) + moonBox.Width;
      g.DrawString(L"Moon", -1, &font, Gdiplus::PointF(x, y), &text);
      g.DrawLine(&moon, x - gap - line, y + textHeight / 2, x - gap, y + textHeight / 2);
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
      track.sun.push_back(sunPosition(when, observer));
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
               RGB(200, 205, 215), RGB(130, 180, 255), RGB(255, 214, 102),
               RGB(255, 170, 60), RGB(52, 78, 122), RGB(18, 20, 28), RGB(236, 232, 218), RGB(58, 60, 70) };
    return { RGB(255, 255, 255), RGB(243, 239, 231), RGB(205, 208, 216), RGB(110, 115, 130),
             RGB(70, 75, 90), RGB(35, 95, 190), RGB(215, 70, 40),
             RGB(225, 140, 20), RGB(255, 255, 255), RGB(188, 198, 222), RGB(250, 246, 232), RGB(86, 90, 104) };
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
    Plot p = drawFrame(g, area, c, painter.font, painter.textHeight, scale, &track);
    static const wchar_t * const hours[] = { L"0h", L"", L"6h", L"", L"12h", L"", L"18h", L"", L"24h" };
    drawColumns(g, p, c, painter.font, painter.textHeight, scale, 8, hours);
    drawHorizonLine(g, p, c, scale);
    if (track.times.size() < 2)
      return;

    double span = track.end - track.start;
    // The Sun's altitude, behind the Moon's.
    if (track.sun.size() == track.times.size()) {
      std::vector<Gdiplus::PointF> sunPoints;
      for (std::size_t i = 0; i < track.times.size(); ++i)
        sunPoints.emplace_back(p.x((track.times[i] - track.start) / span), p.y(track.sun[i].altitude));
      Gdiplus::Pen sunCurve(colour(c.sun), float(1.6 * scale));
      sunCurve.SetDashStyle(Gdiplus::DashStyleDash);
      g.DrawLines(&sunCurve, sunPoints.data(), int(sunPoints.size()));
      if (showNow && now >= track.start && now <= track.end)
        drawSun(g, p.x((now - track.start) / span), p.y(positionAt(track, track.sun, now).altitude), c, scale);
      drawLegend(g, p, c, painter.font, painter.textHeight, scale);
    }
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

  void drawHorizonChart(HDC dc, const RECT & area, const DayTrack & track, bool showNow, double now,
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

    // A path, broken where it crosses east (from one edge to the other).
    auto drawPath = [&](const std::vector<SkyPosition> & positions, Gdiplus::Pen & pen) {
      std::vector<Gdiplus::PointF> points;
      double last = -1;
      for (std::size_t i = 0; i < positions.size(); ++i) {
        double f = azimuthFraction(positions[i].azimuth);
        if (last >= 0 && std::fabs(f - last) > 0.5) {
          if (points.size() > 1)
            g.DrawLines(&pen, points.data(), int(points.size()));
          points.clear();
        }
        points.emplace_back(p.x(f), p.y(positions[i].altitude));
        last = f;
      }
      if (points.size() > 1)
        g.DrawLines(&pen, points.data(), int(points.size()));
    };
    if (track.sun.size() == track.times.size()) {
      Gdiplus::Pen sunCurve(colour(c.sun), float(1.6 * scale));
      sunCurve.SetDashStyle(Gdiplus::DashStyleDash);
      drawPath(track.sun, sunCurve);
      if (showNow && now >= track.start && now <= track.end) {
        SkyPosition sun = positionAt(track, track.sun, now);
        drawSun(g, p.x(azimuthFraction(sun.azimuth)), p.y(sun.altitude), c, scale);
      }
      drawLegend(g, p, c, painter.font, painter.textHeight, scale);
    }
    Gdiplus::Pen curve(colour(c.curve), float(2.2 * scale));
    curve.SetLineJoin(Gdiplus::LineJoinRound);
    drawPath(track.positions, curve);
    for (const DayTrack::Event & e : track.events)
      drawEvent(g, p, e, p.x(azimuthFraction(e.position.azimuth)), p.x(azimuthFraction(e.below.azimuth)), c,
                painter.font, painter.textHeight, scale);
    if (showNow)
      drawDot(g, p.x(azimuthFraction(current.azimuth)), p.y(current.altitude), c, scale);
  }

  //---------------------------------------------------------------------------
  // The sky dome

  void drawSkyDome(HDC dc, const RECT & area, const DayTrack & track, bool showNow, double now,
                   const SkyPosition & current, const ChartColours & c, HFONT hfont, double scale)
  {
    Painter painter(dc, hfont);
    Gdiplus::Graphics & g = painter.g;
    const float th = painter.textHeight;
    Gdiplus::SolidBrush background(colour(c.background));
    g.FillRectangle(&background, Gdiplus::Rect(area.left, area.top, area.right - area.left, area.bottom - area.top));

    float cx = (area.left + area.right) / 2.0f, cy = (area.top + area.bottom) / 2.0f;
    float R = std::min(area.right - area.left, area.bottom - area.top) / 2.0f - th * 1.4f;
    if (R < 10)
      return;
    // Looking up: north at the top, east at the left.
    auto at = [&](const SkyPosition & s) {
      double r = R * (90 - s.altitude) / 90, a = s.azimuth * 3.14159265358979323846 / 180;
      return Gdiplus::PointF(cx - float(r * std::sin(a)), cy - float(r * std::cos(a)));
    };

    bool haveSun = track.sun.size() == track.times.size() && track.times.size() > 1;
    SkyPosition sunNow = haveSun ? positionAt(track, track.sun, now) : SkyPosition();
    Gdiplus::SolidBrush sky(colour(haveSun && showNow ? skyColour(sunNow.altitude, c) : c.night));
    g.FillEllipse(&sky, cx - R, cy - R, 2 * R, 2 * R);

    Gdiplus::Pen grid(colour(c.grid), float(1 * scale));
    for (int alt = 30; alt < 90; alt += 30) {
      float r = R * (90 - alt) / 90.0f;
      g.DrawEllipse(&grid, cx - r, cy - r, 2 * r, 2 * r);
    }
    g.DrawLine(&grid, cx - R, cy, cx + R, cy);
    g.DrawLine(&grid, cx, cy - R, cx, cy + R);
    Gdiplus::Pen horizon(colour(c.horizon), float(1.5 * scale));
    g.DrawEllipse(&horizon, cx - R, cy - R, 2 * R, 2 * R);

    Gdiplus::SolidBrush text(colour(c.text));
    Gdiplus::StringFormat centre;
    centre.SetAlignment(Gdiplus::StringAlignmentCenter);
    centre.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    const wchar_t * names[4] = { L"N", L"E", L"S", L"W" };
    for (int i = 0; i < 4; ++i) {
      SkyPosition s;
      s.azimuth = 90 * i;
      s.altitude = 0;
      Gdiplus::PointF q = at(s);
      float dx = (q.X - cx) / R, dy = (q.Y - cy) / R;
      g.DrawString(names[i], -1, &painter.font,
                   Gdiplus::RectF(q.X + dx * th * 0.75f - th, q.Y + dy * th * 0.75f - th, 2 * th, 2 * th), &centre, &text);
    }
    // The altitude circles' labels, along the line to the south-west.
    for (int alt = 30; alt < 90; alt += 30) {
      SkyPosition s;
      s.azimuth = 225;
      s.altitude = alt;
      Gdiplus::PointF q = at(s);
      std::wstring label = std::to_wstring(alt) + wchar_t(0x00B0);
      g.DrawString(label.c_str(), -1, &painter.font, Gdiplus::PointF(q.X + float(2 * scale), q.Y - th), &text);
    }
    if (track.times.size() < 2)
      return;

    // The paths, while above the horizon.
    auto drawPath = [&](const std::vector<SkyPosition> & positions, Gdiplus::Pen & pen) {
      std::vector<Gdiplus::PointF> points;
      for (std::size_t i = 0; i <= positions.size(); ++i) {
        if (i < positions.size() && positions[i].altitude >= 0) {
          points.push_back(at(positions[i]));
          continue;
        }
        if (points.size() > 1)
          g.DrawLines(&pen, points.data(), int(points.size()));
        points.clear();
      }
    };
    if (haveSun) {
      Gdiplus::Pen sunCurve(colour(c.sun), float(1.6 * scale));
      sunCurve.SetDashStyle(Gdiplus::DashStyleDash);
      drawPath(track.sun, sunCurve);
    }
    Gdiplus::Pen curve(colour(c.curve), float(2.2 * scale));
    curve.SetLineJoin(Gdiplus::LineJoinRound);
    drawPath(track.positions, curve);

    // The moonrise and moonset, on the horizon, with their times inside it.
    for (const DayTrack::Event & e : track.events) {
      SkyPosition onHorizon = e.position;
      onHorizon.altitude = 0;
      Gdiplus::PointF q = at(onHorizon);
      float r = float(3.5 * scale);
      Gdiplus::Pen ring(colour(c.text), float(1.5 * scale));
      g.FillEllipse(&background, q.X - r, q.Y - r, 2 * r, 2 * r);
      g.DrawEllipse(&ring, q.X - r, q.Y - r, 2 * r, 2 * r);
      std::time_t when = static_cast<std::time_t>(std::floor(e.time + 0.5));
      std::tm local;
      if (localtime_s(&local, &when) != 0)
        continue;
      wchar_t clock[16];
      std::wcsftime(clock, 16, L"%H:%M", &local);
      std::wstring label = std::wstring(1, wchar_t(e.rise ? 0x2191 : 0x2193)) + L" " + clock;
      Gdiplus::RectF box;
      g.MeasureString(label.c_str(), -1, &painter.font, Gdiplus::PointF(0, 0), &box);
      float dx = (cx - q.X) / R, dy = (cy - q.Y) / R;   // toward the middle
      float lx = q.X + dx * (box.Width / 2 + float(8 * scale)) - box.Width / 2;
      float ly = q.Y + dy * (box.Height / 2 + float(8 * scale)) - box.Height / 2;
      g.DrawString(label.c_str(), -1, &painter.font, Gdiplus::PointF(lx, ly), &text);
    }

    if (showNow && haveSun && sunNow.altitude >= 0) {
      Gdiplus::PointF q = at(sunNow);
      drawSun(g, q.X, q.Y, c, scale);
    }
    if (showNow && current.altitude >= 0) {
      Gdiplus::PointF q = at(current);
      drawDot(g, q.X, q.Y, c, scale);
    }
    else if (showNow) {
      Gdiplus::StringFormat bottom;
      bottom.SetAlignment(Gdiplus::StringAlignmentCenter);
      g.DrawString(L"The Moon is below the horizon", -1, &painter.font,
                   Gdiplus::RectF(cx - R, cy + R * 0.42f, 2 * R, 1.5f * th), &bottom, &text);
    }
  }

  //---------------------------------------------------------------------------
  // The calendar

  namespace
  {
    std::time_t localTime(int year, int month, int day, int hour)
    {
      std::tm t = {};
      t.tm_year = year - 1900;
      t.tm_mon = month - 1;
      t.tm_mday = day;
      t.tm_hour = hour;
      t.tm_isdst = -1;
      return std::mktime(&t);
    }

    // The calendar's layout: the month's name, the weekdays, then six rows
    // of seven days.
    struct CalendarLayout
    {
      float left, top, cellW, cellH, header, weekdays;
      CalendarLayout(const RECT & area, double scale)
      {
        left = float(area.left);
        top = float(area.top);
        header = float(30 * scale);
        weekdays = float(22 * scale);
        cellW = (area.right - area.left) / 7.0f;
        cellH = (area.bottom - area.top - header - weekdays) / 6.0f;
      }
      Gdiplus::RectF cell(int index) const   // 0 - 41
      {
        return Gdiplus::RectF(left + (index % 7) * cellW, top + header + weekdays + (index / 7) * cellH, cellW, cellH);
      }
    };

    // A little Moon at a phase angle: the lit part's edge is the limb on
    // one side and the terminator, an ellipse, on the other.
    void drawLittleMoon(Gdiplus::Graphics & g, float cx, float cy, float r, double phase, bool mirrored,
                        const ChartColours & c)
    {
      Gdiplus::SolidBrush dark(colour(c.moonDark)), lit(colour(c.moonLit));
      g.FillEllipse(&dark, cx - r, cy - r, 2 * r, 2 * r);
      phase = std::fmod(std::fmod(phase, 360.0) + 360.0, 360.0);
      double side = phase < 180 ? 1 : -1;   // waxing: lit on the right (seen from the north)
      if (mirrored)
        side = -side;
      double t = std::cos(phase * 3.14159265358979323846 / 180);
      std::vector<Gdiplus::PointF> edge;
      const int steps = 24;
      for (int i = 0; i <= steps; ++i) {
        double a = (-90 + 180.0 * i / steps) * 3.14159265358979323846 / 180;
        edge.emplace_back(cx + float(side * r * std::cos(a)), cy + float(r * std::sin(a)));
      }
      for (int i = steps; i >= 0; --i) {
        double a = (-90 + 180.0 * i / steps) * 3.14159265358979323846 / 180;
        edge.emplace_back(cx + float(side * r * std::cos(a) * t), cy + float(r * std::sin(a)));
      }
      g.FillPolygon(&lit, edge.data(), int(edge.size()));
      Gdiplus::Pen outline(colour(c.grid), 1.0f);   // (so a full Moon shows on a white background)
      g.DrawEllipse(&outline, cx - r, cy - r, 2 * r, 2 * r);
    }
  }

  CalendarMonth calendarMonth(int year, int month)
  {
    CalendarMonth m;
    m.year = year;
    m.month = month;
    std::time_t first = localTime(year, month, 1, 12);
    std::tm local;
    localtime_s(&local, &first);
    m.firstWeekday = local.tm_wday;
    static const int lengths[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    m.days = lengths[month - 1];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
      m.days = 29;
    m.phase.resize(m.days);
    m.quarter.assign(m.days, -1);
    for (int d = 1; d <= m.days; ++d)
      m.phase[d - 1] = phaseAt(double(localTime(year, month, d, 12)));
    double start = double(localTime(year, month, 1, 0)), end = double(localTime(year, month, m.days, 0)) + 86400;
    for (const MoonQuarter & q : quartersBetween(start, end)) {
      std::tm when;
      if (localtime_s(&when, &q.time) == 0 && when.tm_mon == month - 1 && when.tm_mday >= 1 && when.tm_mday <= m.days)
        m.quarter[when.tm_mday - 1] = q.quarter;
    }
    return m;
  }

  void drawCalendar(HDC dc, const RECT & area, const CalendarMonth & month, bool mirrored,
                    const ChartColours & c, HFONT hfont, double scale)
  {
    Painter painter(dc, hfont);
    Gdiplus::Graphics & g = painter.g;
    Gdiplus::SolidBrush background(colour(c.background));
    g.FillRectangle(&background, Gdiplus::Rect(area.left, area.top, area.right - area.left, area.bottom - area.top));
    if (month.days == 0)
      return;
    CalendarLayout lay(area, scale);
    Gdiplus::SolidBrush text(colour(c.text)), strong(colour(c.dot));
    Gdiplus::StringFormat centre;
    centre.SetAlignment(Gdiplus::StringAlignmentCenter);
    centre.SetLineAlignment(Gdiplus::StringAlignmentCenter);

    // "October 2026", with an arrow at each side for the months before and after.
    static const wchar_t * const names[12] = { L"January", L"February", L"March", L"April", L"May", L"June", L"July",
                                               L"August", L"September", L"October", L"November", L"December" };
    std::wstring title = std::wstring(names[month.month - 1]) + L" " + std::to_wstring(month.year);
    float width = float(area.right - area.left);
    g.DrawString(title.c_str(), -1, &painter.font, Gdiplus::RectF(lay.left, lay.top, width, lay.header), &centre, &text);
    const wchar_t previous[2] = { wchar_t(0x25C0), 0 }, next[2] = { wchar_t(0x25B6), 0 };
    g.DrawString(previous, -1, &painter.font, Gdiplus::RectF(lay.left, lay.top, lay.cellW, lay.header), &centre, &text);
    g.DrawString(next, -1, &painter.font, Gdiplus::RectF(lay.left + width - lay.cellW, lay.top, lay.cellW, lay.header),
                 &centre, &text);

    static const wchar_t * const weekdays[7] = { L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat" };
    for (int i = 0; i < 7; ++i)
      g.DrawString(weekdays[i], -1, &painter.font,
                   Gdiplus::RectF(lay.left + i * lay.cellW, lay.top + lay.header, lay.cellW, lay.weekdays), &centre, &text);

    Gdiplus::Pen grid(colour(c.grid), float(1 * scale));
    Gdiplus::Pen selected(colour(c.curve), float(2 * scale)), marked(colour(c.dot), float(1.6 * scale));
    Gdiplus::StringFormat corner;
    for (int d = 1; d <= month.days; ++d) {
      Gdiplus::RectF cell = lay.cell(month.firstWeekday + d - 1);
      g.DrawRectangle(&grid, cell);
      std::wstring number = std::to_wstring(d);
      g.DrawString(number.c_str(), -1, &painter.font, Gdiplus::PointF(cell.X + float(2 * scale), cell.Y + float(1 * scale)),
                   d == month.today ? &strong : &text);
      float r = std::min(cell.Width, cell.Height) * 0.27f;
      float mx = cell.X + cell.Width * 0.58f, my = cell.Y + cell.Height * 0.58f;
      drawLittleMoon(g, mx, my, r, month.phase[d - 1], mirrored, c);
      if (month.quarter[d - 1] >= 0)   // a new moon, quarter or full moon falls on this day
        g.DrawEllipse(&marked, mx - r - float(2.5 * scale), my - r - float(2.5 * scale),
                      2 * r + float(5 * scale), 2 * r + float(5 * scale));
    }
    if (month.selected >= 1 && month.selected <= month.days) {
      Gdiplus::RectF cell = lay.cell(month.firstWeekday + month.selected - 1);
      cell.Inflate(float(-1 * scale), float(-1 * scale));
      g.DrawRectangle(&selected, cell);
    }
  }

  int calendarHit(const RECT & area, const CalendarMonth & month, POINT point, double scale)
  {
    CalendarLayout lay(area, scale);
    if (point.y < lay.top + lay.header) {
      if (point.x < lay.left + lay.cellW)
        return calendarPrevious;
      if (point.x >= area.right - lay.cellW)
        return calendarNext;
      return 0;
    }
    for (int d = 1; d <= month.days; ++d) {
      Gdiplus::RectF cell = lay.cell(month.firstWeekday + d - 1);
      if (point.x >= cell.X && point.x < cell.X + cell.Width && point.y >= cell.Y && point.y < cell.Y + cell.Height)
        return d;
    }
    return 0;
  }
}
