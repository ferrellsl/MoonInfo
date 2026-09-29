// MoonInfo: finding the computer's location.
//
// Windows location (Windows.Devices.Geolocation, through C++/WinRT), then an
// internet address lookup (https://ipinfo.io/json, through WinHTTP).  Kept
// apart from the GUI: GraphApp's headers and the Windows Runtime's don't
// mix.
//
// Copyright 2026 Steve Ferrell.

#include "Location.h"

#include <cstdlib>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Geolocation.h>

using namespace std::chrono_literals;
#endif

namespace mooninfo
{
  namespace
  {
#ifdef _WIN32
    // Windows' location service.  Returns false (with the reason) if it's
    // turned off for desktop apps, or finds nothing within 15 seconds.
    bool windowsLocation(Location & result, std::string & problem)
    {
      namespace geo = winrt::Windows::Devices::Geolocation;
      try {
        auto access = geo::Geolocator::RequestAccessAsync().get();
        if (access != geo::GeolocationAccessStatus::Allowed) {
          problem = "Windows location is turned off for desktop apps";
          return false;
        }
        geo::Geolocator locator;
        auto request = locator.GetGeopositionAsync();
        if (request.wait_for(15s) != winrt::Windows::Foundation::AsyncStatus::Completed) {
          request.Cancel();
          problem = "Windows location didn't answer";
          return false;
        }
        auto point = request.GetResults().Coordinate().Point();
        auto position = point.Position();
        result.latitude = position.Latitude;
        result.longitude = position.Longitude;
        // An altitude is given only by some sources (e.g. GPS); Wi-Fi
        // positions report 0 with no reference system.
        if (point.AltitudeReferenceSystem() != geo::AltitudeReferenceSystem::Unspecified && position.Altitude != 0) {
          result.hasElevation = true;
          result.elevation = position.Altitude;
        }
        result.found = true;
        result.source = "Windows location";
        return true;
      }
      catch (winrt::hresult_error const & e) {
        problem = "Windows location: " + winrt::to_string(e.message());
        return false;
      }
    }

    // A value from ipinfo.io's JSON, e.g. "loc": "41.55,-72.65".
    std::string jsonValue(const std::string & json, const char * key)
    {
      std::string quoted = std::string("\"") + key + "\"";
      std::size_t k = json.find(quoted);
      if (k == std::string::npos)
        return std::string();
      std::size_t open = json.find('"', json.find(':', k + quoted.size()) + 1);
      std::size_t close = open == std::string::npos ? open : json.find('"', open + 1);
      if (close == std::string::npos)
        return std::string();
      return json.substr(open + 1, close - open - 1);
    }

    bool internetLocation(Location & result, std::string & problem)
    {
      std::string body;
      bool ok = false;
      HINTERNET session = WinHttpOpen(L"MoonInfo/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
      HINTERNET connection = NULL, request = NULL;
      if (session) {
        WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000);
        connection = WinHttpConnect(session, L"ipinfo.io", INTERNET_DEFAULT_HTTPS_PORT, 0);
      }
      if (connection)
        request = WinHttpOpenRequest(connection, L"GET", L"/json", NULL, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
      if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
          && WinHttpReceiveResponse(request, NULL)) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        char buffer[4096];
        DWORD read = 0;
        while (WinHttpReadData(request, buffer, sizeof(buffer), &read) && read > 0 && body.size() < 65536)
          body.append(buffer, read);
        ok = status == 200;
      }
      if (request)    WinHttpCloseHandle(request);
      if (connection) WinHttpCloseHandle(connection);
      if (session)    WinHttpCloseHandle(session);

      std::string loc = ok ? jsonValue(body, "loc") : std::string();
      std::size_t comma = loc.find(',');
      if (comma == std::string::npos) {
        problem = "couldn't reach the internet location service";
        return false;
      }
      result.latitude = std::atof(loc.substr(0, comma).c_str());
      result.longitude = std::atof(loc.substr(comma + 1).c_str());
      result.found = true;
      std::string city = jsonValue(body, "city"), region = jsonValue(body, "region");
      result.source = "your internet address (approximate";
      if (! city.empty())
        result.source += ": " + city + (region.empty() ? std::string() : ", " + region);
      result.source += ")";
      return true;
    }
#endif
  }

  Location findLocation()
  {
    Location result;
#ifdef _WIN32
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    std::string windowsProblem, internetProblem;
    if (! windowsLocation(result, windowsProblem) && ! internetLocation(result, internetProblem))
      result.problem = windowsProblem + ", and " + internetProblem;
    winrt::uninit_apartment();
#else
    result.problem = "finding the location isn't available on this system";
#endif
    return result;
  }

#ifdef _WIN32
  Location findWindowsLocation()
  {
    Location result;
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    windowsLocation(result, result.problem);
    winrt::uninit_apartment();
    return result;
  }

  Location findInternetLocation()
  {
    Location result;
    internetLocation(result, result.problem);
    return result;
  }
#endif
}
