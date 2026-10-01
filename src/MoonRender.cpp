// MoonInfo: drawing the Moon as it looks at a given time and place.
//
// Copyright 2026 Steve Ferrell.

#include <algorithm>
#include <cstring>
#include <cmath>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>
#endif

extern "C" {
#include "astronomy.h"
}

#include "MoonRender.h"

namespace mooninfo
{
  namespace
  {
    const double pi = 3.14159265358979323846, toRad = pi / 180, toDeg = 180 / pi;
    const double moonRadiusKm = 1737.4;

    Vec3 vec(double x, double y, double z) { Vec3 v; v.x = x; v.y = y; v.z = z; return v; }
    Vec3 vec(astro_vector_t v) { return vec(v.x, v.y, v.z); }
    Vec3 operator + (Vec3 a, Vec3 b) { return vec(a.x + b.x, a.y + b.y, a.z + b.z); }
    Vec3 operator - (Vec3 a, Vec3 b) { return vec(a.x - b.x, a.y - b.y, a.z - b.z); }
    Vec3 operator * (Vec3 a, double k) { return vec(a.x * k, a.y * k, a.z * k); }
    double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    Vec3 cross(Vec3 a, Vec3 b) { return vec(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
    double length(Vec3 a) { return std::sqrt(dot(a, a)); }
    Vec3 unit(Vec3 a) { double n = length(a); return n > 0 ? a * (1 / n) : a; }

    const double j2000Unix = 946728000.0;

    // sRGB <-> linear light
    float linearOf[256];
    unsigned char srgbOf[4096];
    void makeTables()
    {
      static bool made = false;
      if (made)
        return;
      for (int i = 0; i < 256; ++i) {
        double c = i / 255.0;
        linearOf[i] = (float) (c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
      }
      for (int i = 0; i < 4096; ++i) {
        double l = i / 4095.0;
        double c = l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1 / 2.4) - 0.055;
        srgbOf[i] = (unsigned char) std::lround(std::min(1.0, std::max(0.0, c)) * 255);
      }
      made = true;
    }
  }

  MoonGeometry moonGeometry(double unixSeconds, const Observer & where, bool topocentric)
  {
    MoonGeometry g;
    astro_time_t time = Astronomy_TimeFromDays((unixSeconds - j2000Unix) / 86400.0);
    Vec3 moon = vec(Astronomy_GeoMoon(time));                              // AU, from the Earth's centre
    Vec3 sun = vec(Astronomy_GeoVector(BODY_SUN, time, ABERRATION));
    Vec3 observer;
    if (topocentric) {
      astro_observer_t o = Astronomy_MakeObserver(where.latitude, where.longitude, where.elevation);
      observer = vec(Astronomy_ObserverVector(&time, o, EQUATOR_J2000));
    }
    Vec3 look = moon - observer;
    g.toMoon = unit(look);
    g.toSun = unit(sun - moon);
    g.diameter = 2 * std::asin(std::min(1.0, moonRadiusKm / (length(look) * KM_PER_AU))) * toDeg;

    // The Moon's body frame from the IAU rotation model: the pole, and the
    // prime meridian turned by the spin angle W from the node of the Moon's
    // equator on the J2000 equator.
    astro_axis_t axis = Astronomy_RotationAxis(BODY_MOON, &time);
    g.pole = unit(vec(axis.north));
    double ra = axis.ra * 15 * toRad, w = axis.spin * toRad;
    Vec3 node = vec(-std::sin(ra), std::cos(ra), 0);
    g.prime = unit(node * std::cos(w) + cross(g.pole, node) * std::sin(w));
    g.east90 = cross(g.pole, g.prime);

    Vec3 toViewer = g.toMoon * -1;
    g.subEarthLon = std::atan2(dot(toViewer, g.east90), dot(toViewer, g.prime)) * toDeg;
    g.subEarthLat = std::asin(dot(toViewer, g.pole)) * toDeg;
    return g;
  }

  bool projectToPicture(const MoonGeometry & g, int size, double angle, double longitude, double latitude,
                        double & x, double & y)
  {
    // (The same view as renderMoon().)
    const Vec3 d = g.toMoon, z = vec(0, 0, 1);
    Vec3 up = unit(z - d * dot(z, d));
    Vec3 right = unit(cross(z, d)) * -1;
    double a = angle * toRad, c = std::cos(a), s = std::sin(a);
    Vec3 right2 = right * c + up * s, up2 = up * c - right * s;
    const double radius = size / 2.0 * 0.96 * std::min(1.0, g.diameter / 0.57);
    double lon = longitude * toRad, lat = latitude * toRad;
    Vec3 p = g.prime * (std::cos(lat) * std::cos(lon)) + g.east90 * (std::cos(lat) * std::sin(lon))
             + g.pole * std::sin(lat);
    x = size / 2.0 + dot(p, right2) * radius;
    y = size / 2.0 - dot(p, up2) * radius;
    return dot(p, d) < -0.12;   // (not at the very edge, where names would pile up)
  }

  void renderMoon(const MoonMaps & maps, const MoonGeometry & g, int size, double angle,
                  std::vector<std::uint32_t> & pixels)
  {
    makeTables();
    pixels.assign(std::size_t(size) * size, 0xFF000000u);
    if (! maps.ok() || size <= 0)
      return;

    // The screen: north up, east to the left (as the sky is seen), then
    // turned clockwise by angle.
    const Vec3 d = g.toMoon, z = vec(0, 0, 1);
    Vec3 up = unit(z - d * dot(z, d));
    Vec3 right = unit(cross(z, d)) * -1;   // west
    double a = angle * toRad, c = std::cos(a), s = std::sin(a);
    Vec3 right2 = right * c + up * s, up2 = up * c - right * s;

    // The disc at its apparent size: 0.57 degrees (the largest, a close
    // perigee high in the sky) fills 96% of the picture.
    const double radius = size / 2.0 * 0.96 * std::min(1.0, g.diameter / 0.57);
    const double centre = size / 2.0;
    const int W = maps.width, H = maps.height;
    const double metresPerRow = pi * moonRadiusKm * 1000 / H;   // north-south spacing
    const bool relief = ! maps.elevation.empty();

    // Earthshine: sunlight reflected by the Earth dimly lights the night
    // side, most around new Moon (when the Earth, seen from the Moon, is
    // nearly full), slightly blue.
    const double earthLit = (1 - dot(g.toSun, d * -1)) / 2;
    const double earthshine = 0.03 * earthLit;
    const double earthTint[3] = { 1.3, 1.0, 0.78 };   // (blue, green, red: BGRA order)

    auto elevation = [&](int x, int y) {
      x = ((x % W) + W) % W;
      y = std::max(0, std::min(H - 1, y));
      return (maps.elevation[std::size_t(y) * W + x] - 20000.0) * 0.5;   // metres
    };

    auto renderRows = [&](int from, int to) {
      for (int py = from; py < to; ++py)
        for (int px = 0; px < size; ++px) {
          double X = (px + 0.5 - centre) / radius, Y = (centre - (py + 0.5)) / radius;
          double r2 = X * X + Y * Y;
          if (r2 >= 1.0 + 2.0 / radius)
            continue;
          double coverage = std::min(1.0, std::max(0.0, (1 - std::sqrt(r2)) * radius + 0.5));   // (smooth limb)
          double depth = std::sqrt(std::max(0.0, 1 - r2));
          Vec3 p = right2 * X + up2 * Y - d * depth;   // the surface point (unit), from the Moon's centre

          double lon = std::atan2(dot(p, g.east90), dot(p, g.prime));
          double lat = std::asin(std::max(-1.0, std::min(1.0, dot(p, g.pole))));
          double u = (lon / (2 * pi) + 0.5) * W - 0.5, v = (0.5 - lat / pi) * H - 0.5;
          int x0 = (int) std::floor(u), y0 = (int) std::floor(v);
          double fx = u - x0, fy = v - y0;

          // The color (bilinear, in linear light)
          double rgb[3] = { 0, 0, 0 };
          for (int k = 0; k < 4; ++k) {
            int xi = ((x0 + (k & 1)) % W + W) % W, yi = std::max(0, std::min(H - 1, y0 + (k >> 1)));
            double wgt = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
            std::uint32_t q = maps.color[std::size_t(yi) * W + xi];
            rgb[0] += wgt * linearOf[q & 0xFF];
            rgb[1] += wgt * linearOf[(q >> 8) & 0xFF];
            rgb[2] += wgt * linearOf[(q >> 16) & 0xFF];
          }

          // The surface's normal: the sphere's, tilted by the terrain's slopes.
          Vec3 n = p;
          if (relief) {
            int xc = (int) std::lround(u), yc = (int) std::lround(v);
            double metresPerColumn = 2 * pi * moonRadiusKm * 1000 * std::max(0.02, std::cos(lat)) / W;
            double slopeEast = (elevation(xc + 1, yc) - elevation(xc - 1, yc)) / (2 * metresPerColumn);
            double slopeNorth = (elevation(xc, yc - 1) - elevation(xc, yc + 1)) / (2 * metresPerRow);
            Vec3 east = unit(cross(g.pole, p)), north = cross(p, east);
            n = unit(p - east * slopeEast - north * slopeNorth);
          }

          // Lommel-Seeliger: 2 mu0 / (mu0 + mu), mu0 the cosine of the Sun's
          // incidence, mu of the view (1 at the disc's centre at full Moon).
          double mu0 = dot(n, g.toSun), mu = std::max(0.05, dot(n, d * -1));
          double sphereLit = dot(p, g.toSun);
          double light = 0;
          if (mu0 > 0 && sphereLit > -0.03)
            light = 2 * mu0 / (mu0 + mu) * std::min(1.0, (sphereLit + 0.03) / 0.03);   // (no light past the terminator)

          double dark = std::min(1.0, std::max(0.0, -sphereLit / 0.05));   // (earthshine where it's night)
          std::uint32_t out = 0xFF000000u;
          for (int ch = 0; ch < 3; ++ch) {
            double l = std::min(1.0, rgb[ch] * (light + earthshine * dark * earthTint[ch]) * coverage);
            out |= (std::uint32_t) srgbOf[(int) (l * 4095 + 0.5)] << (8 * ch);
          }
          pixels[std::size_t(py) * size + px] = out;
        }
    };

    int threads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t)
      workers.emplace_back(renderRows, size * t / threads, size * (t + 1) / threads);
    for (std::thread & w : workers)
      w.join();
  }

#ifdef _WIN32
  namespace
  {
    // Decode an image file to the given pixel format.
    bool readImage(const std::wstring & path, REFWICPixelFormatGUID format, int bytesPerPixel,
                   std::vector<unsigned char> & data, int & width, int & height)
    {
      IWICImagingFactory * factory = NULL;
      IWICBitmapDecoder * decoder = NULL;
      IWICBitmapFrameDecode * frame = NULL;
      IWICBitmapSource * converted = NULL;
      bool ok = false;
      if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
          SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), NULL, GENERIC_READ,
                                                       WICDecodeMetadataCacheOnDemand, &decoder)) &&
          SUCCEEDED(decoder->GetFrame(0, &frame)) &&
          SUCCEEDED(WICConvertBitmapSource(format, frame, &converted))) {
        UINT w = 0, h = 0;
        converted->GetSize(&w, &h);
        width = (int) w;
        height = (int) h;
        data.assign(std::size_t(w) * h * bytesPerPixel, 0);
        ok = SUCCEEDED(converted->CopyPixels(NULL, w * bytesPerPixel, (UINT) data.size(), data.data()));
      }
      if (converted) converted->Release();
      if (frame)     frame->Release();
      if (decoder)   decoder->Release();
      if (factory)   factory->Release();
      return ok;
    }
  }

  bool loadMoonMaps(const std::wstring & folder, MoonMaps & maps)
  {
    maps = MoonMaps();
    std::vector<unsigned char> data;
    int w = 0, h = 0;
    if (! readImage(folder + L"moon_color.jpg", GUID_WICPixelFormat32bppBGRA, 4, data, w, h))
      return false;
    maps.width = w;
    maps.height = h;
    maps.color.resize(std::size_t(w) * h);
    std::memcpy(maps.color.data(), data.data(), data.size());
    int ew = 0, eh = 0;
    if (readImage(folder + L"moon_height.png", GUID_WICPixelFormat16bppGray, 2, data, ew, eh) && ew == w && eh == h) {
      maps.elevation.resize(std::size_t(w) * h);
      std::memcpy(maps.elevation.data(), data.data(), data.size());
    }
    return true;
  }
#endif
}
