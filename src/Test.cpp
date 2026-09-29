// MoonInfo: prints the calculations for a time and place, to check them
// against the website.
//
//   mooninfo-test <unix seconds> <latitude> <longitude> [elevation]

#include <cstdio>
#include <cstdlib>
#include <string>

#include <cstdint>
#include <vector>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

#include "Location.h"
#include "MoonCalc.h"
#include "MoonRender.h"

// A BGRA picture as a 24-bit BMP.
static bool writeBmp(const char * path, const std::vector<std::uint32_t> & pixels, int size)
{
  FILE * f = std::fopen(path, "wb");
  if (! f)
    return false;
  int row = (size * 3 + 3) & ~3, bytes = row * size;
  unsigned char header[54] = { 'B', 'M' };
  auto put32 = [&](int at, std::uint32_t v) { for (int i = 0; i < 4; ++i) header[at + i] = (unsigned char) (v >> (8 * i)); };
  put32(2, 54 + bytes); put32(10, 54); put32(14, 40); put32(18, size); put32(22, (std::uint32_t) -size);
  header[26] = 1; header[28] = 24; put32(34, bytes);
  std::fwrite(header, 1, 54, f);
  std::vector<unsigned char> line(row, 0);
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      std::uint32_t p = pixels[std::size_t(y) * size + x];
      line[x * 3] = (unsigned char) p; line[x * 3 + 1] = (unsigned char) (p >> 8); line[x * 3 + 2] = (unsigned char) (p >> 16);
    }
    std::fwrite(line.data(), 1, row, f);
  }
  std::fclose(f);
  return true;
}

int main(int argc, char * argv[])
{
  // mooninfo-test render <unix seconds> <out.bmp> <size> [latitude longitude angle]:
  // the Moon as seen from the Earth's centre, north up (or from a place,
  // turned by angle degrees), drawn from moon_color.jpg and moon_height.png
  // beside the program.
  if (argc >= 5 && std::string(argv[1]) == "render") {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    wchar_t exe[MAX_PATH];
    std::wstring folder(exe, GetModuleFileNameW(NULL, exe, MAX_PATH));
    folder = folder.substr(0, folder.find_last_of(L"\\/") + 1);
    mooninfo::MoonMaps maps;
    if (! mooninfo::loadMoonMaps(folder, maps)) {
      std::fprintf(stderr, "can't read moon_color.jpg / moon_height.png\n");
      return 1;
    }
    mooninfo::Observer where;
    bool topocentric = argc >= 8;
    double angle = 0;
    if (topocentric) {
      where.latitude = std::atof(argv[5]);
      where.longitude = std::atof(argv[6]);
      angle = std::atof(argv[7]);
    }
    mooninfo::MoonGeometry g = mooninfo::moonGeometry(std::atof(argv[2]), where, topocentric);
    std::printf("diameter %.4f deg, sub-Earth lon %.3f lat %.3f\n", g.diameter, g.subEarthLon, g.subEarthLat);
    std::vector<std::uint32_t> pixels;
    int size = std::atoi(argv[4]);
    DWORD t0 = GetTickCount();
    mooninfo::renderMoon(maps, g, size, angle, pixels);
    std::printf("rendered %dx%d in %lu ms\n", size, size, (unsigned long) (GetTickCount() - t0));
    return writeBmp(argv[3], pixels, size) ? 0 : 1;
  }

  // mooninfo-test location: both ways of finding the location, separately.
  if (argc == 2 && std::string(argv[1]) == "location") {
    mooninfo::Location w = mooninfo::findWindowsLocation(), i = mooninfo::findInternetLocation();
    std::printf("Windows location:  %s %.4f %.4f %s\n", w.found ? "found" : "not found", w.latitude, w.longitude,
                w.found ? w.source.c_str() : w.problem.c_str());
    std::printf("Internet location: %s %.4f %.4f %s\n", i.found ? "found" : "not found", i.latitude, i.longitude,
                i.found ? i.source.c_str() : i.problem.c_str());
    return 0;
  }
  if (argc < 4) {
    std::fprintf(stderr, "usage: mooninfo-test <unix seconds> <latitude> <longitude> [elevation]\n");
    return 2;
  }
  mooninfo::Observer where;
  where.latitude = std::atof(argv[2]);
  where.longitude = std::atof(argv[3]);
  where.elevation = argc > 4 ? std::atof(argv[4]) : 0;
  mooninfo::MoonInfo m = mooninfo::calculate(std::atof(argv[1]), where);
  std::printf("phase %.6f\nillumination %.6f\nrise %lld\nset %lld\n", m.phase, m.illumination,
              (long long) m.rise, (long long) m.set);
  for (int i = 0; i < 4; ++i)
    std::printf("quarter %d %lld\n", m.quarters[i].quarter, (long long) m.quarters[i].time);
  std::printf("azimuth %.6f\naltitude %.6f\nparallactic %.6f\nra %.6f\ndec %.6f\nframe %d\ndistance %.3f\n",
              m.azimuth, m.altitude, m.parallactic, m.ra, m.dec, m.frame, m.distance);
  return 0;
}
