// MoonInfo: prints the calculations for a time and place, to check them
// against the website.
//
//   mooninfo-test <unix seconds> <latitude> <longitude> [elevation]

#include <cstdio>
#include <cstdlib>
#include <string>

#include "Location.h"
#include "MoonCalc.h"

int main(int argc, char * argv[])
{
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
  std::printf("azimuth %.6f\naltitude %.6f\nparallactic %.6f\nra %.6f\ndec %.6f\nframe %d\n",
              m.azimuth, m.altitude, m.parallactic, m.ra, m.dec, m.frame);
  return 0;
}
