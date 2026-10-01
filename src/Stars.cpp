// MoonInfo: the stars behind the Moon.
//
// Copyright 2026 Steve Ferrell.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <istream>

#include "Stars.h"

namespace mooninfo
{
  namespace
  {
    const double pi = 3.14159265358979323846, toRad = pi / 180;

    Vec3 vec(double x, double y, double z) { Vec3 v; v.x = x; v.y = y; v.z = z; return v; }
    Vec3 operator + (Vec3 a, Vec3 b) { return vec(a.x + b.x, a.y + b.y, a.z + b.z); }
    Vec3 operator - (Vec3 a, Vec3 b) { return vec(a.x - b.x, a.y - b.y, a.z - b.z); }
    Vec3 operator * (Vec3 a, double k) { return vec(a.x * k, a.y * k, a.z * k); }
    double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    Vec3 cross(Vec3 a, Vec3 b) { return vec(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
    Vec3 unit(Vec3 a) { double n = std::sqrt(dot(a, a)); return n > 0 ? a * (1 / n) : a; }
  }

#ifdef _WIN32
  bool StarCatalog::load(const std::wstring & path)
  {
    std::ifstream in(path, std::ios::binary);   // (a wide path: Microsoft's extension)
    return read(in);
  }
#endif

  bool StarCatalog::load(const std::string & path)
  {
    std::ifstream in(path, std::ios::binary);
    return read(in);
  }

  bool StarCatalog::read(std::istream & in)
  {
    stars_.clear();
    first_.clear();
    char magic[8];
    unsigned char countBytes[4];
    if (! in.read(magic, 8) || std::memcmp(magic, "MOONSTAR", 8) != 0 || ! in.read((char *) countBytes, 4))
      return false;
    std::uint32_t count = countBytes[0] | (countBytes[1] << 8) | (countBytes[2] << 16) | (std::uint32_t(countBytes[3]) << 24);
    if (count == 0 || count > 5000000)
      return false;
    std::vector<unsigned char> data(std::size_t(count) * 12);
    if (! in.read((char *) data.data(), (std::streamsize) data.size()))
      return false;

    stars_.resize(count);
    first_.assign(361, count);
    for (std::uint32_t i = 0; i < count; ++i) {
      const unsigned char * r = &data[std::size_t(i) * 12];
      std::uint32_t raBits = r[0] | (r[1] << 8) | (r[2] << 16) | (std::uint32_t(r[3]) << 24);
      std::int32_t decBits = (std::int32_t) (r[4] | (r[5] << 8) | (r[6] << 16) | (std::uint32_t(r[7]) << 24));
      std::int16_t magBits = (std::int16_t) (r[8] | (r[9] << 8));
      double ra = raBits / 4294967296.0 * 360, dec = decBits / 2147483647.0 * 90;
      Star & s = stars_[i];
      s.x = float(std::cos(dec * toRad) * std::cos(ra * toRad));
      s.y = float(std::cos(dec * toRad) * std::sin(ra * toRad));
      s.z = float(std::sin(dec * toRad));
      s.magnitude = magBits / 1000.0f;
      s.colour = (signed char) r[10] / 50.0f;
      int degree = std::min(359, int(ra));
      if (first_[degree] == count)
        first_[degree] = i;
    }
    for (int d = 359; d >= 0; --d)        // (a degree with no stars starts where the next does)
      if (first_[d] == count)
        first_[d] = first_[d + 1];
    return true;
  }

  void StarCatalog::within(const Vec3 & direction, double radius, std::vector<const Star *> & found) const
  {
    found.clear();
    if (stars_.empty())
      return;
    double dec = std::asin(std::max(-1.0, std::min(1.0, direction.z))) / toRad;
    double ra = std::atan2(direction.y, direction.x) / toRad;
    if (ra < 0)
      ra += 360;
    // The degrees of right ascension the circle spans (it's wider near the poles).
    double halfWidth = radius / std::max(0.2, std::cos((std::fabs(dec) + radius) * toRad));
    int from = int(std::floor(ra - halfWidth)), to = int(std::floor(ra + halfWidth));
    const double limit = std::cos(radius * toRad);
    for (int d = from; d <= to; ++d) {
      int degree = ((d % 360) + 360) % 360;
      for (std::uint32_t i = first_[degree]; i < first_[degree + 1]; ++i) {
        const Star & s = stars_[i];
        if (s.x * direction.x + s.y * direction.y + s.z * direction.z >= limit)
          found.push_back(&s);
      }
    }
  }

  void drawStars(const StarCatalog & catalog, const MoonGeometry & g, int size, double angle,
                 std::vector<std::uint32_t> & pixels)
  {
    if (! catalog.ok() || size <= 0 || pixels.size() != std::size_t(size) * size || g.diameter <= 0)
      return;
    // The same view as renderMoon(): the Moon's disc has this radius, and
    // that many pixels to a radian of sky.
    const Vec3 d = g.toMoon, z = vec(0, 0, 1);
    Vec3 up = unit(z - d * dot(z, d));
    Vec3 right = unit(cross(z, d)) * -1;
    double a = angle * toRad, c = std::cos(a), s = std::sin(a);
    Vec3 right2 = right * c + up * s, up2 = up * c - right * s;
    const double radius = size / 2.0 * 0.96 * std::min(1.0, g.diameter / 0.57);
    const double pixelsPerRadian = radius / std::sin(g.diameter / 2 * toRad);
    const double centre = size / 2.0;

    std::vector<const StarCatalog::Star *> stars;
    catalog.within(d, std::atan(size * 0.75 / pixelsPerRadian) / toRad, stars);
    const double base = std::max(0.75, size / 900.0);   // a star's size, pixels
    for (const StarCatalog::Star * star : stars) {
      Vec3 v = vec(star->x, star->y, star->z);
      double depth = dot(v, d);
      if (depth <= 0)
        continue;
      double x = centre + dot(v, right2) / depth * pixelsPerRadian;
      double y = centre - dot(v, up2) / depth * pixelsPerRadian;
      double fromCentre = std::sqrt((x - centre) * (x - centre) + (y - centre) * (y - centre));
      double sigma = base * (1 + 0.32 * std::max(0.0, 8.0 - star->magnitude));
      if (fromCentre < radius + 1.5 * sigma)
        continue;   // behind the Moon (or just emerging)

      // Brighter stars are brighter and bigger; the colour follows B-V
      // (blue-white, white, yellow, orange).
      double peak = std::min(1.0, std::max(0.14, 0.95 * std::pow(10.0, -0.4 * 0.62 * (star->magnitude - 8.0))));
      double t = std::min(1.0, std::max(0.0, (star->colour + 0.3) / 1.9));
      double tint[3] = { 1.0 - 0.55 * t, 0.86 + 0.14 * (1 - std::fabs(t - 0.45) * 1.2), 0.72 + 0.28 * std::min(1.0, t * 2.2) };   // B, G, R
      int reach = int(std::ceil(3 * sigma));
      for (int py = int(y) - reach; py <= int(y) + reach; ++py)
        for (int px = int(x) - reach; px <= int(x) + reach; ++px) {
          if (px < 0 || py < 0 || px >= size || py >= size)
            continue;
          double dx = px + 0.5 - x, dy = py + 0.5 - y;
          double light = peak * std::exp(-(dx * dx + dy * dy) / (2 * sigma * sigma));
          if (light < 0.01)
            continue;
          // (Not over the Moon's limb.)
          double qx = px + 0.5 - centre, qy = py + 0.5 - centre;
          if (qx * qx + qy * qy < radius * radius)
            continue;
          std::uint32_t & q = pixels[std::size_t(py) * size + px];
          std::uint32_t out = 0xFF000000u;
          for (int ch = 0; ch < 3; ++ch) {
            int value = int((q >> (8 * ch)) & 0xFF) + int(255 * light * std::min(1.0, tint[ch]) + 0.5);
            out |= (std::uint32_t) std::min(255, value) << (8 * ch);
          }
          q = out;
        }
    }
  }
}
