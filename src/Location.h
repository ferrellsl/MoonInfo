// MoonInfo: finding the computer's location.
//
// Copyright 2026 Steve Ferrell.

#ifndef MOONINFO_LOCATION_H
#define MOONINFO_LOCATION_H

#include <string>

namespace mooninfo
{
  struct Location
  {
    bool found = false;
    double latitude = 0, longitude = 0;
    bool hasElevation = false;
    double elevation = 0;   // metres
    std::string source;     // how it was found, for the status line
    std::string problem;    // why it wasn't, if it wasn't
  };

  // Windows' location service first (the one Edge and Maps use; it follows
  // the user's privacy settings), then, if that's off or unavailable, the
  // approximate location of the computer's internet address (ipinfo.io).
  // Blocks for up to about 25 seconds: call it on a worker thread.
  Location findLocation();

  // The two ways separately (for testing).
  Location findWindowsLocation();
  Location findInternetLocation();
}

#endif
