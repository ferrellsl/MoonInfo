// MoonInfo: the Moon Info website as a Windows program.
//
// Shows the Moon's phase, illumination, distance, rise and set, the next
// quarters, its position, charts of its path through the day, and a picture
// of it, turned to look as it does from the observer's location (or north
// up).  Plain Win32: the data are in
// a scrolling panel on the left, the picture (decoded with WIC) on the
// right.  The calculations are in MoonCalc.cpp and finding the location in
// Location.cpp.  (It also runs under Wine on Linux and macOS.)
//
// Copyright 2026 Steve Ferrell.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Location.h"
#include "MoonCalc.h"
#include "MoonRender.h"
#include "SkyCharts.h"
#include "Stars.h"
#include "config.h"
#include "resource.h"

using namespace mooninfo;

namespace
{
  //--------------------------------------------------------------------------
  // Text, files and times
  //--------------------------------------------------------------------------

  std::wstring widen(const std::string & utf8)
  {
    if (utf8.empty())
      return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int) utf8.size(), NULL, 0);
    std::wstring wide(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int) utf8.size(), &wide[0], n);
    return wide;
  }

  std::string toUtf8(const std::wstring & wide)
  {
    if (wide.empty())
      return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int) wide.size(), NULL, 0, NULL, NULL);
    std::string utf8(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int) wide.size(), &utf8[0], n, NULL, NULL);
    return utf8;
  }

  std::string trim(const std::string & text)
  {
    std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
      return std::string();
    std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
  }

  // A number such as "30", "-90" or "+41.55" (as the website accepts).
  bool parseNumber(const std::string & text, double & value)
  {
    if (text.empty())
      return false;
    char * end = 0;
    value = std::strtod(text.c_str(), &end);
    return end && *end == '\0' && std::isfinite(value);
  }

  // The folder the program is in, with a trailing backslash.
  std::wstring programFolder()
  {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring p(path, n);
    return p.substr(0, p.find_last_of(L"\\/") + 1);
  }

  // %APPDATA%\MoonInfo\settings.ini
  std::wstring settingsFile()
  {
    const wchar_t * appData = _wgetenv(L"APPDATA");
    std::wstring folder;
    if (appData && *appData)
      folder = std::wstring(appData) + L"\\MoonInfo";
    else
      folder = programFolder() + L"settings";
    CreateDirectoryW(folder.c_str(), NULL);
    return folder + L"\\settings.ini";
  }

  // Local time <-> text, "YYYY-MM-DD HH:MM:SS" (the website's format).
  std::string formatLocal(std::time_t t)
  {
    std::tm local;
    if (localtime_s(&local, &t) != 0)
      return "?";
    char text[32];
    std::strftime(text, sizeof text, "%Y-%m-%d %H:%M:%S", &local);
    return text;
  }

  bool parseLocal(const std::string & text, std::time_t & t)
  {
    std::tm local = {};
    int n = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d:%d%n", &local.tm_year, &local.tm_mon, &local.tm_mday,
                    &local.tm_hour, &local.tm_min, &local.tm_sec, &n) != 6 || text[n] != '\0') {
      // Seconds may be left out.
      local = {};
      n = 0;
      if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d%n", &local.tm_year, &local.tm_mon, &local.tm_mday,
                      &local.tm_hour, &local.tm_min, &n) != 5 || text[n] != '\0')
        return false;
    }
    if (local.tm_mon < 1 || local.tm_mon > 12 || local.tm_mday < 1 || local.tm_mday > 31 ||
        local.tm_hour > 23 || local.tm_min > 59 || local.tm_sec > 60 || local.tm_year < 1601)
      return false;
    local.tm_year -= 1900;
    local.tm_mon -= 1;
    local.tm_isdst = -1;   // whichever applies on that date
    t = std::mktime(&local);
    return t != (std::time_t) -1;
  }

  std::string fixed(double value, int decimals)
  {
    char text[32];
    std::snprintf(text, sizeof text, "%.*f", decimals, value);
    return text;
  }

  // 384400 -> "384,400"
  std::string withThousands(double value)
  {
    std::string digits = fixed(value, 0), result;
    std::size_t start = digits[0] == '-' ? 1 : 0;
    for (std::size_t i = 0; i < digits.size(); ++i) {
      if (i > start && (digits.size() - i) % 3 == 0)
        result += ',';
      result += digits[i];
    }
    return result;
  }

  const char * degree = "\xC2\xB0";   // UTF-8
  const double metresPerFoot = 0.3048, kmPerMile = 1.609344;

  //--------------------------------------------------------------------------
  // Settings
  //--------------------------------------------------------------------------

  struct Settings
  {
    std::string latitude = "30", longitude = "-90", elevation = "0";   // elevation in the chosen units
    std::string date;               // when not automatic
    bool automatic = true;
    bool observerView = true;       // turn the picture as seen from here
    bool darkMode = false;          // (first run: as Windows' app theme)
    bool imperial = false;          // elevation in feet, distance in miles (else metres, km)
    bool labels = false;            // name the seas and craters on the picture
    bool stars = true;              // the stars behind the Moon
    int playSpeed = 2;              // 0 - 4: a quarter, a half, 1, 2 and 4 times the usual speed
    bool stored = false;            // read from the settings file
  };

  // Whether Windows' app theme is dark (Settings > Personalization > Colors).
  bool windowsDarkMode()
  {
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &size);
    return light == 0;
  }

  Settings loadSettings()
  {
    Settings s;
    s.darkMode = windowsDarkMode();
    std::ifstream in(settingsFile());
    std::string line;
    while (std::getline(in, line)) {
      std::size_t eq = line.find('=');
      if (eq == std::string::npos)
        continue;
      std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
      double number;
      if      (key == "latitude"  && parseNumber(value, number)) { s.latitude = value;  s.stored = true; }
      else if (key == "longitude" && parseNumber(value, number))   s.longitude = value;
      else if (key == "elevation" && parseNumber(value, number))   s.elevation = value;
      else if (key == "date")                                      s.date = value;
      else if (key == "automatic")                                 s.automatic = value != "0";
      else if (key == "observerView")                              s.observerView = value != "0";
      else if (key == "darkMode")                                  s.darkMode = value != "0";
      else if (key == "imperial")                                  s.imperial = value != "0";
      else if (key == "labels")                                    s.labels = value != "0";
      else if (key == "stars")                                     s.stars = value != "0";
      else if (key == "playSpeed" && parseNumber(value, number))
        s.playSpeed = std::max(0, std::min(4, int(number)));
    }
    return s;
  }

  void saveSettings(const Settings & s)
  {
    std::ofstream out(settingsFile(), std::ios::trunc);
    out << "latitude=" << s.latitude << "\nlongitude=" << s.longitude << "\nelevation=" << s.elevation
        << "\ndate=" << s.date << "\nautomatic=" << (s.automatic ? 1 : 0)
        << "\nobserverView=" << (s.observerView ? 1 : 0) << "\ndarkMode=" << (s.darkMode ? 1 : 0)
        << "\nimperial=" << (s.imperial ? 1 : 0) << "\nlabels=" << (s.labels ? 1 : 0) << "\nstars=" << (s.stars ? 1 : 0)
        << "\nplaySpeed=" << s.playSpeed << "\n";
  }

  //--------------------------------------------------------------------------
  // The window and its controls
  //--------------------------------------------------------------------------

  HINSTANCE instance;
  HWND mainWindow, panel;          // panel: the scrolling data column
  HFONT font;
  double dpiScale = 1.0;
  Settings settings;

  HWND dateEdit, autoCheck, locationButton, locationStatus;
  HWND latEdit, lonEdit, elevEdit, elevLabel;
  HWND problemLabel;
  HWND timeSlider, playDayButton, playMonthButton;   // moving through time
  HWND speedSlider, speedLabel;
  HMENU viewMenu;                  // the View menu's options
  std::map<std::string, HWND> values;   // the results, by name

  enum { ID_AUTO = 100, ID_LOCATION, ID_VIEW, ID_DARK, ID_UNITS, ID_EXIT, ID_HELP, ID_ABOUT,
         ID_LABELS, ID_STARS, ID_PREVIOUS_DAY, ID_NEXT_DAY, ID_PLAY_DAY, ID_PLAY_MONTH };
  const UINT WM_LOCATED = WM_APP + 1;   // the location worker has finished
  const UINT_PTR TIMER_ID = 1, TIMER_PLAY = 2;

  // Layout, in unscaled (96 dpi) pixels.
  const int margin = 12, labelWidth = 150, valueX = margin + labelWidth, valueWidth = 270, rowHeight = 26;
  const int panelWidth = valueX + valueWidth + 8;   // the data column (its scroll bar is added to it)
  const int imageSize = 400;        // the picture at the window's first size
  int pictureSize = 0;              // the picture on screen, pixels: as big as the window allows
  int pictureLeft = 0;              // its left edge (it's centered when there's room to spare)
  int imageX = 0;                   // (set once the scroll bar's width is known)
  int contentHeight = 0;            // the data column's height, unscaled
  int scrollPos = 0;                // in screen pixels

  int S(int n) { return static_cast<int>(n * dpiScale + 0.5); }

  std::wstring windowText(HWND h)
  {
    int n = GetWindowTextLengthW(h);
    std::wstring text(n + 1, L'\0');
    GetWindowTextW(h, &text[0], n + 1);
    text.resize(n);
    return text;
  }

  std::string controlText(HWND h) { return trim(toUtf8(windowText(h))); }

  // Controls are redrawn only when their text changes (no flicker).
  void setText(HWND h, const std::string & text)
  {
    std::wstring wide = widen(text);
    if (windowText(h) != wide)
      SetWindowTextW(h, wide.c_str());
  }

  bool isChecked(HWND h) { return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }
  void check(HWND h, bool on) { SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }

  HWND makeControl(HWND parent, const wchar_t * cls, const std::string & text, DWORD style,
                   int x, int y, int w, int h, int id = 0, DWORD exStyle = 0)
  {
    HWND c = CreateWindowExW(exStyle, cls, widen(text).c_str(), WS_CHILD | WS_VISIBLE | style,
                             S(x), S(y), S(w), S(h), parent, (HMENU) (INT_PTR) id, instance, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM) font, FALSE);
    return c;
  }

  HWND addLabel(HWND parent, int x, int y, int w, const std::string & text)
  {
    return makeControl(parent, L"STATIC", text, SS_LEFT | SS_NOPREFIX, x, y + 5, w, rowHeight - 5);
  }

  HWND addEdit(int y, int w, const std::string & text)
  {
    return makeControl(panel, L"EDIT", text, WS_TABSTOP | ES_AUTOHSCROLL, valueX, y, w, rowHeight - 2,
                       0, WS_EX_CLIENTEDGE);
  }

  void addValue(int y, const char * label, const char * name)
  {
    addLabel(panel, margin, y, labelWidth, label);
    values[name] = addLabel(panel, valueX, y, valueWidth, "");
  }

  //--------------------------------------------------------------------------
  // Dark mode
  //--------------------------------------------------------------------------

  COLORREF windowColour, textColour, editColour;
  HBRUSH windowBrush = NULL, editBrush = NULL;

  // Windows' dark title bar (Windows 10 20H1 and later; 19 before that).
  void setTitleBar(bool dark)
  {
    BOOL value = dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(mainWindow, 20, &value, sizeof value)))
      DwmSetWindowAttribute(mainWindow, 19, &value, sizeof value);
  }

  BOOL CALLBACK themeChild(HWND h, LPARAM dark)
  {
    wchar_t cls[32];
    GetClassNameW(h, cls, 32);
    LONG style = GetWindowLongW(h, GWL_STYLE) & 0x0F;
    if (lstrcmpiW(cls, L"BUTTON") == 0 && (style == BS_AUTOCHECKBOX || style == BS_CHECKBOX))
      // Themed check boxes ignore the text colour: unthemed in dark mode.
      SetWindowTheme(h, dark ? L"" : NULL, dark ? L"" : NULL);
    else if (lstrcmpiW(cls, L"BUTTON") == 0 || lstrcmpiW(cls, L"EDIT") == 0)
      SetWindowTheme(h, dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
    return TRUE;
  }

  void applyTheme(bool dark)
  {
    windowColour = dark ? RGB(28, 28, 30) : GetSysColor(COLOR_WINDOW);
    textColour   = dark ? RGB(230, 230, 230) : GetSysColor(COLOR_WINDOWTEXT);
    editColour   = dark ? RGB(48, 48, 52) : GetSysColor(COLOR_WINDOW);
    if (windowBrush) DeleteObject(windowBrush);
    if (editBrush)   DeleteObject(editBrush);
    windowBrush = CreateSolidBrush(windowColour);
    editBrush = CreateSolidBrush(editColour);
    SetWindowTheme(panel, dark ? L"DarkMode_Explorer" : L"Explorer", NULL);   // its scroll bar
    EnumChildWindows(mainWindow, themeChild, dark ? 1 : 0);
    setTitleBar(dark);
    for (HWND slider : { timeSlider, speedSlider }) {
      if (! slider)
        continue;
      // A slider keeps a picture of its background: resizing it makes a new one.
      RECT r;
      GetWindowRect(slider, &r);
      MapWindowPoints(NULL, panel, (POINT *) &r, 2);
      MoveWindow(slider, r.left, r.top, r.right - r.left + 1, r.bottom - r.top, FALSE);
      MoveWindow(slider, r.left, r.top, r.right - r.left, r.bottom - r.top, FALSE);
    }
    RedrawWindow(mainWindow, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
  }

  // WM_CTLCOLOR* for the main window's and the panel's controls.
  LRESULT controlColours(UINT msg, HDC dc, HWND control)
  {
    bool edit = msg == WM_CTLCOLOREDIT || control == dateEdit || control == latEdit
                || control == lonEdit || control == elevEdit;
    COLORREF text = textColour;
    if (! IsWindowEnabled(control))
      text = settings.darkMode ? RGB(140, 140, 140) : GetSysColor(COLOR_GRAYTEXT);
    SetTextColor(dc, text);
    SetBkColor(dc, edit ? editColour : windowColour);
    return (LRESULT) (edit ? editBrush : windowBrush);
  }

  //--------------------------------------------------------------------------
  // The Moon's picture
  //--------------------------------------------------------------------------

  // The picture is drawn from NASA's maps (MoonRender.cpp), in data\ beside
  // the program.
  MoonMaps maps;
  bool mapsLoaded = false;

  StarCatalog starCatalog;            // (data\moon_stars.bin)
  std::vector<std::uint32_t> shown;   // the Moon as last drawn, BGRA
  std::vector<std::uint32_t> picture; // the picture on screen: the Moon, and the stars behind it
  Vec3 starsAt;                       // where the Moon was among the stars when they were last drawn
  double starsAngle = 0;
  int shownSize = 0;
  MoonGeometry shownGeometry;
  double shownAngle = 0;
  MoonGeometry currentGeometry;       // the latest (redrawn when it changes visibly)
  double currentAngle = 0;
  bool haveGeometry = false;

  RECT pictureRect()
  {
    RECT r = { pictureLeft, S(margin), pictureLeft + pictureSize, S(margin) + pictureSize };
    return r;
  }

  // The seas and the best-known craters (selenographic longitude, east
  // positive, and latitude; degrees).  The craters are named only when the
  // picture is big enough for them.
  struct Feature { const wchar_t * name; double longitude, latitude; bool major; };
  const Feature features[] = {
    { L"Mare Imbrium", -15.6, 32.8, true },         { L"Mare Serenitatis", 17.5, 28.0, true },
    { L"Mare Tranquillitatis", 31.4, 8.5, true },   { L"Mare Crisium", 59.1, 17.0, true },
    { L"Mare Fecunditatis", 51.3, -7.8, true },     { L"Mare Nectaris", 34.6, -15.2, true },
    { L"Mare Nubium", -16.6, -21.3, true },         { L"Mare Humorum", -38.6, -24.4, true },
    { L"Oceanus Procellarum", -57.4, 18.4, true },  { L"Mare Frigoris", 1.4, 56.0, true },
    { L"Mare Vaporum", 3.6, 13.3, false },          { L"Mare Cognitum", -23.1, -10.0, false },
    { L"Tycho", -11.4, -43.3, true },               { L"Copernicus", -20.1, 9.6, true },
    { L"Kepler", -38.0, 8.1, false },               { L"Aristarchus", -47.4, 23.7, false },
    { L"Plato", -9.4, 51.6, false },                { L"Clavius", -14.4, -58.4, false },
    { L"Ptolemaeus", -1.8, -9.2, false },           { L"Langrenus", 61.0, -8.9, false },
    { L"Grimaldi", -68.6, -5.2, false },            { L"Archimedes", -4.0, 29.7, false },
    { L"Eratosthenes", -11.3, 14.5, false },        { L"Theophilus", 26.4, -11.4, false },
    { L"Posidonius", 29.9, 31.8, false },           { L"Gassendi", -39.9, -17.5, false },
    { L"Petavius", 60.4, -25.3, false },            { L"Aristoteles", 17.4, 50.2, false },
    { L"Apollo 11", 23.47, 0.67, false },
  };

  // The names, over the picture in a bitmap of its size.
  void drawFeatureNames(HDC dc)
  {
    bool all = shownSize >= S(520);
    HFONT old = (HFONT) SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    for (const Feature & f : features) {
      double x, y;
      if ((! f.major && ! all) || ! projectToPicture(shownGeometry, shownSize, shownAngle, f.longitude, f.latitude, x, y))
        continue;
      RECT at = { (LONG) x, (LONG) y, (LONG) x, (LONG) y };
      const UINT format = DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP | DT_NOPREFIX;
      for (int dx = -1; dx <= 1; ++dx)        // a dark edge, to read on bright ground
        for (int dy = -1; dy <= 1; ++dy) {
          RECT edge = { at.left + dx, at.top + dy, at.right + dx, at.bottom + dy };
          SetTextColor(dc, RGB(0, 0, 0));
          DrawTextW(dc, f.name, -1, &edge, format);
        }
      SetTextColor(dc, RGB(255, 244, 190));
      DrawTextW(dc, f.name, -1, &at, format);
    }
    SelectObject(dc, old);
  }

  void paintPicture(HDC dc)
  {
    RECT r = pictureRect();
    if (! picture.empty() && shownSize == pictureSize) {
      BITMAPINFO bi = {};
      bi.bmiHeader.biSize = sizeof bi.bmiHeader;
      bi.bmiHeader.biWidth = shownSize;
      bi.bmiHeader.biHeight = -shownSize;   // top row first
      bi.bmiHeader.biPlanes = 1;
      bi.bmiHeader.biBitCount = 32;
      bi.bmiHeader.biCompression = BI_RGB;
      if (! settings.labels) {
        SetDIBitsToDevice(dc, r.left, r.top, shownSize, shownSize, 0, 0, 0, shownSize,
                          picture.data(), &bi, DIB_RGB_COLORS);
        return;
      }
      // With the names: put together in a bitmap first, so they don't flicker.
      HDC memory = CreateCompatibleDC(dc);
      HBITMAP bitmap = CreateCompatibleBitmap(dc, shownSize, shownSize);
      HGDIOBJ old = SelectObject(memory, bitmap);
      SetDIBitsToDevice(memory, 0, 0, shownSize, shownSize, 0, 0, 0, shownSize, picture.data(), &bi, DIB_RGB_COLORS);
      drawFeatureNames(memory);
      BitBlt(dc, r.left, r.top, shownSize, shownSize, memory, 0, 0, SRCCOPY);
      SelectObject(memory, old);
      DeleteObject(bitmap);
      DeleteDC(memory);
      return;
    }
    FillRect(dc, &r, (HBRUSH) GetStockObject(BLACK_BRUSH));
    if (! mapsLoaded) {
      SetTextColor(dc, RGB(200, 200, 200));
      SetBkMode(dc, TRANSPARENT);
      HFONT old = (HFONT) SelectObject(dc, font);
      DrawTextW(dc, L"(The Moon's maps, data/moon_color.jpg and moon_height.png, are missing.)", -1, &r,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      SelectObject(dc, old);
    }
  }

  // Whether the Moon has changed enough since it was drawn to draw it again
  // (it moves smoothly, so every second would be wasted effort).
  bool changedVisibly(const MoonGeometry & a, const MoonGeometry & b, double angleA, double angleB)
  {
    auto apart = [](const Vec3 & u, const Vec3 & v) {   // degrees between two unit vectors
      double c = u.x * v.x + u.y * v.y + u.z * v.z;
      return std::acos(std::max(-1.0, std::min(1.0, c))) * 57.29577951308232;
    };
    return std::fabs(angleA - angleB) > 0.05 || apart(a.toSun, b.toSun) > 0.02 || apart(a.toMoon, b.toMoon) > 0.02
           || std::fabs(a.subEarthLon - b.subEarthLon) > 0.02 || std::fabs(a.subEarthLat - b.subEarthLat) > 0.02
           || std::fabs(a.diameter - b.diameter) > a.diameter * 0.001;
  }

  // Draw the Moon if it (or the picture's size) has changed.
  void showMoon(const MoonGeometry & geometry, double angle)
  {
    currentGeometry = geometry;
    currentAngle = angle;
    haveGeometry = true;
    if (! mapsLoaded)
      return;
    bool moonChanged = shownSize != pictureSize || shown.empty()
                       || changedVisibly(geometry, shownGeometry, angle, shownAngle);
    if (moonChanged) {
      renderMoon(maps, geometry, pictureSize, angle, shown);
      shownSize = pictureSize;
      shownGeometry = geometry;
      shownAngle = angle;
    }

    // The stars slide past much faster than the Moon's face changes: they're
    // drawn again, over the same Moon, when they've moved half a pixel.
    bool withStars = settings.stars && starCatalog.ok();
    double moved = 0;
    if (withStars && ! moonChanged) {
      double c = geometry.toMoon.x * starsAt.x + geometry.toMoon.y * starsAt.y + geometry.toMoon.z * starsAt.z;
      double pixelsPerDegree = pictureSize * 0.96 / 0.57;
      moved = std::acos(std::max(-1.0, std::min(1.0, c))) * 57.29577951308232 * pixelsPerDegree
              + std::fabs(angle - starsAngle) * 0.017453292519943295 * pictureSize / 2;
    }
    if (! moonChanged && ! (withStars && moved > 0.5) && picture.size() == shown.size())
      return;
    picture = shown;
    if (withStars) {
      MoonGeometry view = shownGeometry;   // (the Moon as drawn, where it is among the stars now)
      view.toMoon = geometry.toMoon;
      drawStars(starCatalog, view, pictureSize, angle, picture);
      starsAt = geometry.toMoon;
      starsAngle = angle;
    }
    RECT r = pictureRect();
    InvalidateRect(mainWindow, &r, FALSE);
  }

  // Fit the picture to the window: the largest square beside the data
  // column (centered when the window is wider, e.g. maximized).
  void fitPicture()
  {
    if (! mainWindow)
      return;
    RECT client;
    GetClientRect(mainWindow, &client);
    int width = client.right - S(imageX) - S(margin);
    int height = client.bottom - 2 * S(margin);
    int size = std::max(S(120), std::min(width, height));
    int left = S(imageX) + std::max(0, (width - size) / 2);
    if (size == pictureSize && left == pictureLeft)
      return;
    pictureSize = size;
    pictureLeft = left;
    if (haveGeometry)
      showMoon(currentGeometry, currentAngle);   // (redrawn at the new size)
    InvalidateRect(mainWindow, NULL, TRUE);      // (and clear where it was)
  }

  // The window's inside width that fits the picture exactly, for a given
  // inside height (the picture's height is the window's, less margins).
  int clientWidthFor(int clientHeight) { return clientHeight + S(imageX) - S(margin); }
  int clientHeightFor(int clientWidth) { return clientWidth - S(imageX) + S(margin); }

  //--------------------------------------------------------------------------
  // The charts of the Moon's path through the day (SkyCharts.cpp)
  //--------------------------------------------------------------------------

  void update();
  void setManualTime(std::time_t when);

  HWND altitudeChart, horizonChart, skyDome, calendarView;
  CalendarMonth calendar;           // the month the calendar shows
  int calendarYear = 0, calendarMonthNumber = 0;
  int shownYear = 0, shownMonth = 0, shownDay = 0;   // the date the data are for
  std::time_t shownTime = 0;
  bool calendarMirrored = false;
  std::string calendarKey;          // what it last drew
  DayTrack chartTrack;              // the shown day's path
  std::string chartTrackKey;        // the day and place it was calculated for
  bool chartShowNow = false;
  double chartNow = 0;
  SkyPosition chartCurrent;

  // The day (local midnight to midnight) containing a time.
  void localDay(std::time_t when, std::time_t & start, std::time_t & end)
  {
    std::tm local;
    localtime_s(&local, &when);
    local.tm_hour = local.tm_min = local.tm_sec = 0;
    local.tm_isdst = -1;
    start = std::mktime(&local);
    local.tm_mday += 1;
    local.tm_hour = local.tm_min = local.tm_sec = 0;
    local.tm_isdst = -1;
    end = std::mktime(&local);
  }

  // The calendar's month, with the shown day and today marked; redrawn
  // only when something in it changes.
  void showCalendar()
  {
    if (calendarYear == 0)
      return;
    if (calendar.year != calendarYear || calendar.month != calendarMonthNumber)
      calendar = calendarMonth(calendarYear, calendarMonthNumber);
    calendar.selected = calendarYear == shownYear && calendarMonthNumber == shownMonth ? shownDay : 0;
    std::time_t now = std::time(NULL);
    std::tm today;
    localtime_s(&today, &now);
    calendar.today = today.tm_year + 1900 == calendarYear && today.tm_mon + 1 == calendarMonthNumber ? today.tm_mday : 0;
    std::string key = std::to_string(calendarYear) + "-" + std::to_string(calendarMonthNumber) + "-"
                      + std::to_string(calendar.selected) + "-" + std::to_string(calendar.today)
                      + (calendarMirrored ? "m" : "n") + (settings.darkMode ? "d" : "l");
    if (key != calendarKey) {
      calendarKey = key;
      InvalidateRect(calendarView, NULL, FALSE);
    }
  }

  // A click in the calendar: a day shows that day (at the time of day
  // shown); the arrows show the month before or after.
  void onCalendarClick(HWND hwnd, LPARAM lp)
  {
    RECT r;
    GetClientRect(hwnd, &r);
    POINT point = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    int hit = calendarHit(r, calendar, point, dpiScale);
    if (hit == calendarPrevious || hit == calendarNext) {
      calendarMonthNumber += hit == calendarNext ? 1 : -1;
      if (calendarMonthNumber < 1)  { calendarMonthNumber = 12; calendarYear--; }
      if (calendarMonthNumber > 12) { calendarMonthNumber = 1;  calendarYear++; }
      showCalendar();
    }
    else if (hit >= 1) {
      std::tm local;
      std::time_t base = shownTime ? shownTime : std::time(NULL);
      localtime_s(&local, &base);
      local.tm_year = calendar.year - 1900;
      local.tm_mon = calendar.month - 1;
      local.tm_mday = hit;
      local.tm_isdst = -1;
      setManualTime(std::mktime(&local));
    }
  }

  // The charts for a time and place (recalculating the day's path when the
  // day or the place changes).
  void updateCharts(std::time_t when, const Observer & where, const SkyPosition & now)
  {
    std::time_t start, end;
    localDay(when, start, end);
    std::string key = std::to_string(start) + "|" + fixed(where.latitude, 6) + "|" + fixed(where.longitude, 6)
                      + "|" + fixed(where.elevation, 1);
    if (key != chartTrackKey) {
      chartTrack = dayTrack(double(start), double(end), where);
      chartTrackKey = key;
    }
    chartShowNow = true;
    chartNow = double(when);
    chartCurrent = now;
    InvalidateRect(altitudeChart, NULL, FALSE);
    InvalidateRect(horizonChart, NULL, FALSE);
    InvalidateRect(skyDome, NULL, FALSE);

    // The time-of-day slider, in five-minute steps.
    LPARAM position = (LPARAM) ((when - start) / 300);
    if (SendMessageW(timeSlider, TBM_GETPOS, 0, 0) != position)
      SendMessageW(timeSlider, TBM_SETPOS, TRUE, position);

    // The calendar follows the date into another month.
    std::tm local;
    localtime_s(&local, &when);
    int year = local.tm_year + 1900, month = local.tm_mon + 1;
    if (year != shownYear || month != shownMonth) {
      calendarYear = year;
      calendarMonthNumber = month;
    }
    shownYear = year;
    shownMonth = month;
    shownDay = local.tm_mday;
    shownTime = when;
    calendarMirrored = where.latitude < 0 && settings.observerView;
    showCalendar();
  }

  void clearCharts()
  {
    chartTrack = DayTrack();
    chartTrackKey.clear();
    chartShowNow = false;
    InvalidateRect(altitudeChart, NULL, FALSE);
    InvalidateRect(horizonChart, NULL, FALSE);
    InvalidateRect(skyDome, NULL, FALSE);
  }

  // Painted into a bitmap first, so the once-a-second updates don't flicker.
  void paintChart(HWND hwnd)
  {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT r;
    GetClientRect(hwnd, &r);
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, r.right, r.bottom);
    HGDIOBJ old = SelectObject(memory, bitmap);
    ChartColours colours = chartColours(settings.darkMode);
    if (hwnd == altitudeChart)
      drawAltitudeChart(memory, r, chartTrack, chartShowNow, chartNow, chartCurrent, colours, font, dpiScale);
    else if (hwnd == horizonChart)
      drawHorizonChart(memory, r, chartTrack, chartShowNow, chartNow, chartCurrent, colours, font, dpiScale);
    else if (hwnd == skyDome)
      drawSkyDome(memory, r, chartTrack, chartShowNow, chartNow, chartCurrent, colours, font, dpiScale);
    else
      drawCalendar(memory, r, calendar, calendarMirrored, colours, font, dpiScale);
    BitBlt(dc, 0, 0, r.right, r.bottom, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    EndPaint(hwnd, &ps);
  }

  LRESULT CALLBACK chartProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
  {
    switch (msg) {
      case WM_PAINT:      paintChart(hwnd); return 0;
      case WM_ERASEBKGND: return 1;
      case WM_LBUTTONDOWN:
        if (hwnd == calendarView)
          onCalendarClick(hwnd, lp);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  //--------------------------------------------------------------------------
  // Finding the location (on a worker thread)
  //--------------------------------------------------------------------------

  std::thread         locator;
  std::atomic<bool>   locating(false);
  std::mutex          locationMutex;
  Location            foundLocation;
  bool                locatingAutomatically = false;

  void requestLocation(bool automatic)
  {
    if (locating)
      return;
    if (locator.joinable())
      locator.join();
    locatingAutomatically = automatic;
    locating = true;
    setText(locationStatus, automatic ? "Detecting your location..." : "Locating...");
    EnableWindow(locationButton, FALSE);
    locator = std::thread([]() {
      Location where = findLocation();
      {
        std::lock_guard<std::mutex> lock(locationMutex);
        foundLocation = where;
      }
      PostMessageW(mainWindow, WM_LOCATED, 0, 0);
    });
  }

  // On the GUI thread, once the worker has finished.
  void useFoundLocation()
  {
    Location where;
    {
      std::lock_guard<std::mutex> lock(locationMutex);
      where = foundLocation;
    }
    locating = false;
    EnableWindow(locationButton, TRUE);
    if (! where.found) {
      setText(locationStatus, locatingAutomatically
        ? "Couldn't find your location (" + where.problem + "). Enter it, or click \"Use My Location\"."
        : "Couldn't find your location: " + where.problem + ".");
      return;
    }
    setText(latEdit, fixed(where.latitude, 4));
    setText(lonEdit, fixed(where.longitude, 4));
    if (where.hasElevation)
      setText(elevEdit, fixed(settings.imperial ? where.elevation / metresPerFoot : where.elevation, 0));
    setText(locationStatus, "Location from " + where.source + ".");
  }

  //--------------------------------------------------------------------------
  // Updating
  //--------------------------------------------------------------------------

  void setValue(const char * name, const std::string & text) { setText(values[name], text); }

  std::string lastInputs;   // the inputs of the last calculation
  std::time_t lastTime = 0;
  int playing = 0;          // 0, or ID_PLAY_DAY or ID_PLAY_MONTH while the time is running
  double playTime = 0;

  void update()
  {
    bool automatic = isChecked(autoCheck), observerView = settings.observerView;
    std::time_t now = std::time(NULL), when = now;
    if (automatic && now != lastTime)
      setText(dateEdit, formatLocal(now));
    std::string dateText = controlText(dateEdit);
    std::string latText = controlText(latEdit), lonText = controlText(lonEdit), elevText = controlText(elevEdit);
    std::string inputs = dateText + "|" + latText + "|" + lonText + "|" + elevText + "|"
                         + (automatic ? "a" : "m") + (observerView ? "v" : "n") + (settings.imperial ? "i" : "k");
    if (inputs == lastInputs && (! automatic || now == lastTime))
      return;
    lastInputs = inputs;
    lastTime = now;

    Observer where;
    bool valid = parseNumber(latText, where.latitude) && parseNumber(lonText, where.longitude)
                 && parseNumber(elevText, where.elevation)
                 && where.latitude >= -90 && where.latitude <= 90 && where.longitude >= -180 && where.longitude <= 360;
    if (! automatic)
      valid = valid && parseLocal(dateText, when);
    if (! valid) {
      // Wait until the user corrects the date or the coordinates.
      for (auto & v : values)
        setText(v.second, "");
      setText(problemLabel, "Check the date and time (YYYY-MM-DD HH:MM:SS) and the coordinates.");
      clearCharts();
      return;
    }
    if (settings.imperial)
      where.elevation *= metresPerFoot;

    // Remember the settings.
    Settings now_settings = settings;
    now_settings.latitude = latText;
    now_settings.longitude = lonText;
    now_settings.elevation = elevText;
    now_settings.automatic = automatic;
    now_settings.observerView = observerView;
    if (! automatic)
      now_settings.date = dateText;
    if (! playing &&
        (now_settings.latitude != settings.latitude || now_settings.longitude != settings.longitude ||
         now_settings.elevation != settings.elevation || now_settings.automatic != settings.automatic ||
         now_settings.observerView != settings.observerView || now_settings.date != settings.date)) {
      settings = now_settings;
      saveSettings(settings);
    }

    MoonInfo moon = calculate(double(when), where);
    setText(problemLabel, "");
    setValue("phase", fixed(moon.phase, 3) + degree);
    setValue("phaseName", phaseName(moon.phase));
    setValue("illumination", fixed(moon.illumination * 100, 2) + "%");
    setValue("distance", settings.imperial ? withThousands(moon.distance / kmPerMile) + " miles"
                                           : withThousands(moon.distance) + " km");
    setValue("moonrise", moon.riseFound ? formatLocal(moon.rise) : "none within 300 days");
    setValue("moonset", moon.setFound ? formatLocal(moon.set) : "none within 300 days");
    for (int i = 0; i < 4; ++i) {
      // (The next four quarters always include one new and one full moon.)
      if (moon.quarters[i].quarter == 0) setValue("nextNew", formatLocal(moon.quarters[i].time));
      if (moon.quarters[i].quarter == 2)
        setValue("nextFull", formatLocal(moon.quarters[i].time) + (moon.nextFullIsSupermoon ? "  (supermoon)" : ""));
      setValue(("quarterName" + std::to_string(i)).c_str(), std::string(quarterName(moon.quarters[i].quarter)) + ":");
      setValue(("quarterTime" + std::to_string(i)).c_str(), formatLocal(moon.quarters[i].time));
    }
    setValue("azimuth", fixed(moon.azimuth, 2) + degree);
    setValue("altitude", fixed(moon.altitude, 2) + degree);
    setValue("parallactic", fixed(moon.parallactic, 2) + degree);
    setValue("ra", fixed(moon.ra, 2) + " h");
    setValue("dec", fixed(moon.dec, 2) + degree);
    MoonGeometry geometry = moonGeometry(double(when), where);
    auto distanceText = [](double km) {
      return settings.imperial ? withThousands(km / kmPerMile) + " miles" : withThousands(km) + " km";
    };
    auto signedText = [](double value) { return (value >= 0 ? "+" : "") + fixed(value, 1) + degree; };
    setValue("age", fixed(moon.age, 1) + " days");
    setValue("diameter", fixed(geometry.diameter * 60, 1) + " arcminutes");
    setValue("constellation", moon.constellation);
    setValue("libration", "longitude " + signedText(geometry.subEarthLon) + ", latitude " + signedText(geometry.subEarthLat));
    setValue("perigee", formatLocal(moon.perigee).substr(0, 16) + "  (" + distanceText(moon.perigeeDistance) + ")");
    setValue("apogee", formatLocal(moon.apogee).substr(0, 16) + "  (" + distanceText(moon.apogeeDistance) + ")");
    // The next eclipses: found again when the day or the place changes, or
    // one has passed.
    {
      static std::string key;
      static Eclipses eclipses;
      std::time_t dayStart, dayEnd;
      localDay(when, dayStart, dayEnd);
      std::string now_key = std::to_string(dayStart) + "|" + latText + "|" + lonText + "|" + elevText;
      if (now_key != key || (eclipses.lunarFound && eclipses.lunarPeak < when)
          || (eclipses.solarFound && eclipses.solarPeak < when)) {
        eclipses = nextEclipses(double(when), where);
        key = now_key;
      }
      setValue("lunarEclipse", ! eclipses.lunarFound ? std::string("none found")
               : formatLocal(eclipses.lunarPeak).substr(0, 16) + "  " + eclipses.lunarKind
                 + (eclipses.lunarVisible ? " (Moon up)" : " (Moon down)"));
      setValue("solarEclipse", ! eclipses.solarFound ? std::string("none found")
               : formatLocal(eclipses.solarPeak).substr(0, 16) + "  " + eclipses.solarKind + ", "
                 + (eclipses.solarPeakSun == 0 ? fixed(eclipses.solarObscuration * 100, 0) + "% covered"
                    : eclipses.solarPeakSun == 1 ? std::string("at sunrise") : std::string("at sunset")));
    }
    showMoon(geometry, observerView ? moon.parallactic : 0);
    SkyPosition position;
    position.azimuth = moon.azimuth;
    position.altitude = moon.altitude;
    updateCharts(when, where, position);
  }

  //--------------------------------------------------------------------------
  // Moving through time
  //--------------------------------------------------------------------------

  void stopPlaying()
  {
    if (! playing)
      return;
    KillTimer(mainWindow, TIMER_PLAY);
    playing = 0;
    setText(playDayButton, "Play day");
    setText(playMonthButton, "Play month");
    lastInputs.clear();   // (and remember the date it stopped at)
    update();
  }

  // Show a time other than now: Automatic is unchecked.
  void setManualTime(std::time_t when)
  {
    check(autoCheck, false);
    EnableWindow(dateEdit, TRUE);
    setText(dateEdit, formatLocal(when));
    update();
  }

  void onDayStep(int days)
  {
    stopPlaying();
    std::tm local;
    std::time_t base = shownTime ? shownTime : std::time(NULL);
    localtime_s(&local, &base);
    local.tm_mday += days;
    local.tm_isdst = -1;
    setManualTime(std::mktime(&local));
  }

  // The play speed's slider: how long a day and a month take at each setting.
  const double speedFactors[5] = { 0.25, 0.5, 1, 2, 4 };
  const double daySeconds = 6, monthSeconds = 30;   // at the middle setting

  void showSpeed()
  {
    auto text = [](double seconds) {
      return seconds >= 90 ? fixed(seconds / 60, 0) + " min" : fixed(seconds, seconds < 10 ? 1 : 0) + " s";
    };
    double f = speedFactors[settings.playSpeed];
    setText(speedLabel, "day " + text(daySeconds / f) + ", month " + text(monthSeconds / f));
  }

  void onSpeedSlider()
  {
    int speed = std::max(0, std::min(4, (int) SendMessageW(speedSlider, TBM_GETPOS, 0, 0)));
    if (speed == settings.playSpeed)
      return;
    settings.playSpeed = speed;
    saveSettings(settings);
    showSpeed();
  }

  // Run the time forward: a day in about six seconds, or a month in about
  // thirty, at the middle speed.  The same button stops it.
  void onPlay(int which)
  {
    bool stop = playing == which;
    stopPlaying();
    if (stop)
      return;
    playing = which;
    playTime = double(shownTime ? shownTime : std::time(NULL));
    setText(which == ID_PLAY_DAY ? playDayButton : playMonthButton, "Stop");
    check(autoCheck, false);
    EnableWindow(dateEdit, TRUE);
    SetTimer(mainWindow, TIMER_PLAY, 40, NULL);
  }

  void onPlayTimer()
  {
    if (! playing)
      return;
    // The Moon's time that passes in each frame (25 frames a second).
    const double frames = 25;
    double step = playing == ID_PLAY_DAY ? 86400 / (daySeconds * frames) : 30 * 86400 / (monthSeconds * frames);
    playTime += step * speedFactors[settings.playSpeed];
    setText(dateEdit, formatLocal((std::time_t) playTime));
    update();
  }

  // The slider: the time of day, on the day shown.
  void onTimeSlider()
  {
    stopPlaying();
    std::time_t start, end;
    localDay(shownTime ? shownTime : std::time(NULL), start, end);
    std::time_t when = start + (std::time_t) SendMessageW(timeSlider, TBM_GETPOS, 0, 0) * 300;
    setManualTime(std::min(when, end - 1));
  }

  //--------------------------------------------------------------------------
  // Scrolling the data column
  //--------------------------------------------------------------------------

  int maxScroll()
  {
    RECT r;
    GetClientRect(panel, &r);
    return std::max(0, S(contentHeight) - (int) r.bottom);
  }

  void scrollTo(int pos)
  {
    pos = std::max(0, std::min(pos, maxScroll()));
    if (pos == scrollPos)
      return;
    ScrollWindowEx(panel, 0, scrollPos - pos, NULL, NULL, NULL, NULL, SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    scrollPos = pos;
    SetScrollPos(panel, SB_VERT, scrollPos, TRUE);
    UpdateWindow(panel);
  }

  void setScrollRange()
  {
    RECT r;
    GetClientRect(panel, &r);
    SCROLLINFO si = { sizeof si, SIF_RANGE | SIF_PAGE | SIF_DISABLENOSCROLL };
    si.nMin = 0;
    si.nMax = S(contentHeight) - 1;
    si.nPage = (UINT) std::max<LONG>(1, r.bottom);
    SetScrollInfo(panel, SB_VERT, &si, TRUE);
    scrollTo(scrollPos);   // (within the new range)
    SetScrollPos(panel, SB_VERT, scrollPos, TRUE);
  }

  void onVScroll(WPARAM wp)
  {
    RECT r;
    GetClientRect(panel, &r);
    int pos = scrollPos;
    switch (LOWORD(wp)) {
      case SB_LINEUP:   pos -= S(rowHeight); break;
      case SB_LINEDOWN: pos += S(rowHeight); break;
      case SB_PAGEUP:   pos -= r.bottom; break;
      case SB_PAGEDOWN: pos += r.bottom; break;
      case SB_TOP:      pos = 0; break;
      case SB_BOTTOM:   pos = S(contentHeight); break;
      case SB_THUMBTRACK:
      case SB_THUMBPOSITION: {
        SCROLLINFO si = { sizeof si, SIF_TRACKPOS };
        GetScrollInfo(panel, SB_VERT, &si);
        pos = si.nTrackPos;
        break;
      }
    }
    scrollTo(pos);
  }

  void onWheel(WPARAM wp)
  {
    int notches = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
    scrollTo(scrollPos - 3 * notches * S(rowHeight));
  }

  //--------------------------------------------------------------------------
  // Commands
  //--------------------------------------------------------------------------

  const char * elevationLabel() { return settings.imperial ? "Elevation (ft):" : "Elevation (m):"; }

  void onAutomatic()
  {
    // Unchecked, the field keeps the time shown, for the user to edit.
    stopPlaying();
    EnableWindow(dateEdit, ! isChecked(autoCheck));
    update();
  }

  // Switch between metres and km, and feet and miles; the elevation typed
  // in is converted, so the place stays the same.
  void onUnits()
  {
    bool imperial = ! settings.imperial;
    CheckMenuItem(viewMenu, ID_UNITS, imperial ? MF_CHECKED : MF_UNCHECKED);
    double elevation;
    if (parseNumber(controlText(elevEdit), elevation))
      setText(elevEdit, fixed(imperial ? elevation / metresPerFoot : elevation * metresPerFoot, 0));
    settings.imperial = imperial;
    settings.elevation = controlText(elevEdit);
    saveSettings(settings);
    setText(elevLabel, elevationLabel());
    lastInputs.clear();   // recalculate now
    update();
  }

  void onObserverView()
  {
    settings.observerView = ! settings.observerView;
    CheckMenuItem(viewMenu, ID_VIEW, settings.observerView ? MF_CHECKED : MF_UNCHECKED);
    saveSettings(settings);
    lastInputs.clear();   // redraw now
    update();
  }

  void onLabels()
  {
    settings.labels = ! settings.labels;
    CheckMenuItem(viewMenu, ID_LABELS, settings.labels ? MF_CHECKED : MF_UNCHECKED);
    saveSettings(settings);
    RECT r = pictureRect();
    InvalidateRect(mainWindow, &r, FALSE);
  }

  void onStars()
  {
    settings.stars = ! settings.stars;
    CheckMenuItem(viewMenu, ID_STARS, settings.stars ? MF_CHECKED : MF_UNCHECKED);
    saveSettings(settings);
    picture.clear();   // draw it again
    if (haveGeometry)
      showMoon(currentGeometry, currentAngle);
  }

  void onDarkMode()
  {
    settings.darkMode = ! settings.darkMode;
    CheckMenuItem(viewMenu, ID_DARK, settings.darkMode ? MF_CHECKED : MF_UNCHECKED);
    saveSettings(settings);
    applyTheme(settings.darkMode);
    showCalendar();
  }

  void showAbout()
  {
    std::wstring text = widen(
      "MoonInfo " MOONINFO_VERSION "\n"
      "The Moon's phase, distance, rise and set, quarters and position, and its picture as "
      "seen from your location.\n\n"
      "(c) 2026 Steve Ferrell, https://lidarwidgets.com\n\n"
      "Calculations: Astronomy Engine, (c) 2019-2023 Don Cross (MIT license).\n"
      "Moon maps: NASA's Scientific Visualization Studio (CGI Moon Kit), from the Lunar "
      "Reconnaissance Orbiter's LROC and LOLA teams.\n"
      "Stars: the Tycho-2 catalogue (Hog et al. 2000), ESA's Hipparcos mission.\n"
      "Location: Windows location services, or ipinfo.io.");
    MessageBoxW(mainWindow, text.c_str(), L"About MoonInfo", MB_OK | MB_ICONINFORMATION);
  }

  void showHelp()
  {
    std::wstring text = widen(
      "Date and Time: with Automatic checked, the computer's clock (updated every second); "
      "unchecked, type a local date and time, YYYY-MM-DD HH:MM:SS.\n\n"
      "Time of day: drag the slider to another time on the day shown. \"< Day\" and \"Day >\" "
      "go back and forward a day. \"Play day\" and \"Play month\" run the time forward (a day in "
      "about six seconds, a month in about thirty) so you can watch the Moon move, turn and "
      "change phase; the same button stops it. Play speed makes them slower (to the left) or faster; "
      "the times beside it are how long a day and a month take. Check Automatic to return to now.\n\n"
      "Location: \"Use My Location\" asks Windows' location service (if it's turned on for "
      "desktop apps in Windows' privacy settings), or failing that, looks up the approximate "
      "location of your internet address. Or type the latitude and longitude (degrees; north "
      "and east are positive) and the elevation.\n\n"
      "Times are shown in the computer's time zone. Distance: from the Earth's center to the "
      "Moon's.\n\n"
      "The picture is drawn from NASA's Lunar Reconnaissance Orbiter maps for the date, time "
      "and place: the exact phase, the libration and tilt, the apparent size (which changes "
      "with the Moon's distance) and earthshine on the dark side.\n\n"
      "The charts below the data show the Moon's path through the day (midnight to midnight): "
      "its altitude by the hour, and its altitude by direction (east at the left, through "
      "south, west and north). The shaded part is below the horizon; the dot is the Moon now, "
      "and the small circles are the day's moonrise (up arrow) and moonset (down arrow), with "
      "their times. The dashed line is the Sun, and the sky above the horizon in the first chart "
      "is colored by the daylight: day, the three twilights, and night, when the Moon is best seen. "
      "The round chart is the sky looking up: the horizon around the edge (north at the top, east "
      "at the left), straight up in the middle.\n\n"
      "Calendar: a little Moon for each day of the month, ringed on the days of the new moon, the "
      "quarters and the full moon. Click a day to show it; the arrows beside the month's name show "
      "the months before and after.\n\n"
      "Age: days since the last new moon. Libration: how far the Moon is turned, east-west and "
      "north-south, from facing us squarely. Perigee and apogee: when the Moon is next nearest and "
      "farthest. A full moon closer than 367,600 km (228,400 miles) is marked as a supermoon.\n\n"
      "Next lunar eclipse: the time of its peak and its kind; \"Moon up\" means the Moon is above "
      "your horizon then, so you can see it. Next solar eclipse here: the next one visible from "
      "your location, with how much of the Sun is covered at its peak there (or \"at sunrise\" or "
      "\"at sunset\" if the Sun is below your horizon at the peak, so you see only part of it).\n\n"
      "View > Stars behind the Moon: the stars the Moon is passing, where they really are (from "
      "the Tycho-2 catalogue, to about magnitude 12; far more than its glare lets you see). The "
      "picture is only a little wider than the Moon, so there are usually just a few; watch them "
      "disappear behind the Moon with Play day.\n\n"
      "View > Names of the seas and craters: labels the picture (more names appear as the picture "
      "gets bigger).\n\n"
      "Parallactic angle: the angle between celestial north and straight up at the Moon. "
      "With View > \"Moon as seen from my location\" checked, the picture is turned by it so it's tilted as "
      "the Moon appears in your sky (roughly upside down in the southern hemisphere); "
      "unchecked, it's shown north up.\n\n"
      "View > Miles and feet: the distance in miles and the elevation in feet (unchecked: km and "
      "meters).\n\n"
      "View > Dark mode: light text on a dark window. On the first run it follows Windows' app "
      "theme (Settings > Personalization > Colors).\n\n"
      "The picture grows and shrinks with the window (whose width follows its height as you "
      "resize it, so the picture fills it). If the window is too short for all "
      "the data, scroll it with the scroll bar or the mouse wheel.\n\n"
      "Your settings are kept in %APPDATA%\\MoonInfo\\settings.ini.");
    MessageBoxW(mainWindow, text.c_str(), L"MoonInfo Help", MB_OK | MB_ICONINFORMATION);
  }

  void onCommand(WPARAM wp)
  {
    switch (LOWORD(wp)) {
      case ID_AUTO:     onAutomatic(); break;
      case ID_LOCATION: requestLocation(false); break;
      case ID_VIEW:     onObserverView(); break;
      case ID_DARK:     onDarkMode(); break;
      case ID_UNITS:    onUnits(); break;
      case ID_LABELS:   onLabels(); break;
      case ID_STARS:    onStars(); break;
      case ID_PREVIOUS_DAY: onDayStep(-1); break;
      case ID_NEXT_DAY:     onDayStep(1); break;
      case ID_PLAY_DAY:
      case ID_PLAY_MONTH:   onPlay(LOWORD(wp)); break;
      case ID_EXIT:     DestroyWindow(mainWindow); break;
      case ID_HELP:     showHelp(); break;
      case ID_ABOUT:    showAbout(); break;
    }
  }

  //--------------------------------------------------------------------------
  // Window procedures
  //--------------------------------------------------------------------------

  LRESULT fillBackground(HWND hwnd, HDC dc)
  {
    RECT r;
    GetClientRect(hwnd, &r);
    FillRect(dc, &r, windowBrush);
    return 1;
  }

  LRESULT CALLBACK panelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
  {
    switch (msg) {
      case WM_VSCROLL:        onVScroll(wp); return 0;
      case WM_HSCROLL:
        if ((HWND) lp == timeSlider)
          onTimeSlider();
        else if ((HWND) lp == speedSlider)
          onSpeedSlider();
        return 0;
      case WM_MOUSEWHEEL:     onWheel(wp); return 0;
      case WM_SIZE:           setScrollRange(); return 0;
      case WM_COMMAND:        return SendMessageW(mainWindow, msg, wp, lp);
      case WM_CTLCOLORSTATIC:
      case WM_CTLCOLOREDIT:
      case WM_CTLCOLORBTN:    return controlColours(msg, (HDC) wp, (HWND) lp);
      case WM_ERASEBKGND:     return fillBackground(hwnd, (HDC) wp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  LRESULT CALLBACK mainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
  {
    switch (msg) {
      case WM_COMMAND:        onCommand(wp); return 0;
      case WM_TIMER:
        if (wp == TIMER_PLAY) onPlayTimer();
        else if (! playing)   update();
        return 0;
      case WM_LOCATED:        useFoundLocation(); update(); return 0;
      case WM_MOUSEWHEEL:     onWheel(wp); return 0;   // anywhere in the window scrolls the data
      case WM_SIZE: {
        // The data column runs the window's height.
        RECT r;
        GetClientRect(hwnd, &r);
        MoveWindow(panel, 0, 0, S(panelWidth) + GetSystemMetrics(SM_CXVSCROLL), r.bottom, TRUE);
        fitPicture();
        return 0;
      }
      case WM_SIZING: {
        // Keep the picture filling the right side as the window is dragged:
        // its width follows its height (or, dragging a side, the other way).
        RECT * w = (RECT *) lp;
        RECT frame = { 0, 0, 0, 0 };
        AdjustWindowRectEx(&frame, GetWindowLongW(hwnd, GWL_STYLE), TRUE, GetWindowLongW(hwnd, GWL_EXSTYLE));
        int extraW = frame.right - frame.left, extraH = frame.bottom - frame.top;
        if (wp == WMSZ_LEFT || wp == WMSZ_RIGHT)
          w->bottom = w->top + clientHeightFor((w->right - w->left) - extraW) + extraH;
        else {
          int width = clientWidthFor((w->bottom - w->top) - extraH) + extraW;
          if (wp == WMSZ_LEFT || wp == WMSZ_TOPLEFT || wp == WMSZ_BOTTOMLEFT)
            w->left = w->right - width;
          else
            w->right = w->left + width;
        }
        return TRUE;
      }
      case WM_GETMINMAXINFO: {
        MINMAXINFO * mm = (MINMAXINFO *) lp;
        mm->ptMinTrackSize.x = S(panelWidth + 120);
        mm->ptMinTrackSize.y = S(220);
        return 0;
      }
      case WM_CTLCOLORSTATIC:
      case WM_CTLCOLOREDIT:
      case WM_CTLCOLORBTN:    return controlColours(msg, (HDC) wp, (HWND) lp);
      case WM_ERASEBKGND:     return fillBackground(hwnd, (HDC) wp);
      case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paintPicture(dc);
        EndPaint(hwnd, &ps);
        return 0;
      }
      case WM_DESTROY:
        KillTimer(hwnd, TIMER_ID);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  //--------------------------------------------------------------------------
  // Building the window
  //--------------------------------------------------------------------------

  void createControls()
  {
    int y = margin;
    addLabel(panel, margin, y, labelWidth, "Date and time:");
    dateEdit = addEdit(y, 165, settings.automatic || settings.date.empty() ? formatLocal(std::time(NULL)) : settings.date);
    autoCheck = makeControl(panel, L"BUTTON", "Automatic", WS_TABSTOP | BS_AUTOCHECKBOX, valueX + 173, y + 2, 105,
                            rowHeight - 4, ID_AUTO);
    y += 32;

    // Moving through time: the time of day, a day at a time, or running.
    addLabel(panel, margin, y, labelWidth, "Time of day:");
    timeSlider = makeControl(panel, TRACKBAR_CLASSW, "", WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS, valueX - 6, y,
                             valueWidth + 12, rowHeight + 4);
    SendMessageW(timeSlider, TBM_SETRANGE, FALSE, MAKELPARAM(0, 287));
    SendMessageW(timeSlider, TBM_SETTICFREQ, 36, 0);    // a tick every three hours
    SendMessageW(timeSlider, TBM_SETPAGESIZE, 0, 12);   // an hour
    y += 36;
    {
      const int w = (panelWidth - 2 * margin - 3 * 8) / 4;
      makeControl(panel, L"BUTTON", "< Day", WS_TABSTOP | BS_PUSHBUTTON, margin, y, w, rowHeight, ID_PREVIOUS_DAY);
      makeControl(panel, L"BUTTON", "Day >", WS_TABSTOP | BS_PUSHBUTTON, margin + (w + 8), y, w, rowHeight, ID_NEXT_DAY);
      playDayButton = makeControl(panel, L"BUTTON", "Play day", WS_TABSTOP | BS_PUSHBUTTON, margin + 2 * (w + 8), y, w,
                                  rowHeight, ID_PLAY_DAY);
      playMonthButton = makeControl(panel, L"BUTTON", "Play month", WS_TABSTOP | BS_PUSHBUTTON, margin + 3 * (w + 8), y, w,
                                    rowHeight, ID_PLAY_MONTH);
    }
    y += 34;
    addLabel(panel, margin, y, labelWidth, "Play speed:");
    speedSlider = makeControl(panel, TRACKBAR_CLASSW, "", WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS, valueX - 6, y, 112,
                              rowHeight + 4);
    SendMessageW(speedSlider, TBM_SETRANGE, FALSE, MAKELPARAM(0, 4));
    SendMessageW(speedSlider, TBM_SETPAGESIZE, 0, 1);
    SendMessageW(speedSlider, TBM_SETPOS, TRUE, settings.playSpeed);
    speedLabel = addLabel(panel, valueX + 114, y, valueWidth - 114, "");
    showSpeed();
    y += 38;
    addLabel(panel, margin, y, labelWidth, "Location:");
    locationButton = makeControl(panel, L"BUTTON", "Use My Location", WS_TABSTOP | BS_PUSHBUTTON, valueX, y, 140,
                                 rowHeight, ID_LOCATION);
    y += 30;
    locationStatus = addLabel(panel, margin, y, panelWidth - 2 * margin, "");
    y += rowHeight + 6;
    addLabel(panel, margin, y, labelWidth, "Latitude:");
    latEdit = addEdit(y, 110, settings.latitude);
    y += 30;
    addLabel(panel, margin, y, labelWidth, "Longitude:");
    lonEdit = addEdit(y, 110, settings.longitude);
    y += 30;
    elevLabel = addLabel(panel, margin, y, labelWidth, elevationLabel());
    elevEdit = addEdit(y, 110, settings.elevation);
    y += 38;

    addValue(y, "Phase:", "phase");                   y += rowHeight;
    addValue(y, "Phase name:", "phaseName");          y += rowHeight;
    addValue(y, "Illumination:", "illumination");     y += rowHeight;
    addValue(y, "Age:", "age");                       y += rowHeight;
    addValue(y, "Distance:", "distance");             y += rowHeight;
    addValue(y, "Apparent diameter:", "diameter");    y += rowHeight;
    addValue(y, "Moonrise:", "moonrise");             y += rowHeight;
    addValue(y, "Moonset:", "moonset");               y += rowHeight;
    addValue(y, "Next new moon:", "nextNew");         y += rowHeight;
    addValue(y, "Next full moon:", "nextFull");       y += rowHeight;
    for (int i = 0; i < 4; ++i) {
      std::string n = std::to_string(i);
      values["quarterName" + n] = addLabel(panel, margin, y, labelWidth, "");
      values["quarterTime" + n] = addLabel(panel, valueX, y, valueWidth, "");
      y += rowHeight;
    }
    addValue(y, "Azimuth:", "azimuth");               y += rowHeight;
    addValue(y, "Altitude:", "altitude");             y += rowHeight;
    addValue(y, "Parallactic angle:", "parallactic"); y += rowHeight;
    addValue(y, "RA (J2000):", "ra");                 y += rowHeight;
    addValue(y, "Dec (J2000):", "dec");               y += rowHeight;
    addValue(y, "Constellation:", "constellation");   y += rowHeight;
    addValue(y, "Libration:", "libration");           y += rowHeight;
    addValue(y, "Next perigee:", "perigee");          y += rowHeight;
    addValue(y, "Next apogee:", "apogee");            y += rowHeight;
    addValue(y, "Next lunar eclipse:", "lunarEclipse");   y += rowHeight;
    addValue(y, "Next solar eclipse here:", "solarEclipse");   y += rowHeight;
    problemLabel = makeControl(panel, L"STATIC", "", SS_LEFT, margin, y + 5, panelWidth - 2 * margin, 2 * rowHeight);
    y += 2 * rowHeight;

    // The charts of the day's path, the width of the column.
    const int chartHeight = 190;
    addLabel(panel, margin, y, panelWidth - 2 * margin, "Altitude through the day:");
    y += rowHeight;
    altitudeChart = makeControl(panel, L"MoonInfoChart", "", 0, margin, y, panelWidth - 2 * margin, chartHeight);
    y += chartHeight + 10;
    addLabel(panel, margin, y, panelWidth - 2 * margin, "Path across the sky (altitude by direction):");
    y += rowHeight;
    horizonChart = makeControl(panel, L"MoonInfoChart", "", 0, margin, y, panelWidth - 2 * margin, chartHeight);
    y += chartHeight + 10;
    addLabel(panel, margin, y, panelWidth - 2 * margin, "The sky, looking up (the horizon is the circle):");
    y += rowHeight;
    const int domeHeight = 330;
    skyDome = makeControl(panel, L"MoonInfoChart", "", 0, margin, y, panelWidth - 2 * margin, domeHeight);
    y += domeHeight + 10;
    addLabel(panel, margin, y, panelWidth - 2 * margin, "Calendar (click a day to show it):");
    y += rowHeight;
    const int calendarHeight = 340;
    calendarView = makeControl(panel, L"MoonInfoChart", "", 0, margin, y, panelWidth - 2 * margin, calendarHeight);
    y += calendarHeight;
    contentHeight = y + margin;

    check(autoCheck, settings.automatic);
    EnableWindow(dateEdit, ! settings.automatic);
  }

  HMENU createMenu()
  {
    HMENU bar = CreateMenu(), file = CreatePopupMenu(), help = CreatePopupMenu();
    viewMenu = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, ID_EXIT, L"E&xit");
    auto checked = [](bool on) { return MF_STRING | (on ? MF_CHECKED : MF_UNCHECKED); };
    AppendMenuW(viewMenu, checked(settings.observerView), ID_VIEW, L"Moon &as seen from my location (unchecked: north up)");
    AppendMenuW(viewMenu, checked(settings.labels), ID_LABELS, L"&Names of the seas and craters on the picture");
    AppendMenuW(viewMenu, checked(settings.stars), ID_STARS, L"&Stars behind the Moon");
    AppendMenuW(viewMenu, checked(settings.darkMode), ID_DARK, L"&Dark mode");
    AppendMenuW(viewMenu, checked(settings.imperial), ID_UNITS, L"&Miles and feet (unchecked: km and meters)");
    AppendMenuW(help, MF_STRING, ID_HELP, L"&Using MoonInfo");
    AppendMenuW(help, MF_SEPARATOR, 0, NULL);
    AppendMenuW(help, MF_STRING, ID_ABOUT, L"&About MoonInfo");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR) file, L"&File");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR) viewMenu, L"&View");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR) help, L"&Help");
    return bar;
  }
}

//----------------------------------------------------------------------------

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int showCommand)
{
  instance = hInstance;
  SetProcessDPIAware();
  HDC screen = GetDC(NULL);
  dpiScale = GetDeviceCaps(screen, LOGPIXELSY) / 96.0;
  ReleaseDC(NULL, screen);
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);   // (for WIC)
  INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
  InitCommonControlsEx(&icc);

  settings = loadSettings();

  // The user's dialog font (Segoe UI, at the screen's scale)
  NONCLIENTMETRICSW metrics = { sizeof metrics };
  SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0);
  font = CreateFontIndirectW(&metrics.lfMessageFont);

  WNDCLASSEXW wc = { sizeof wc };
  wc.lpfnWndProc = mainProc;
  wc.hInstance = instance;
  wc.hIcon = (HICON) LoadImageW(instance, MAKEINTRESOURCEW(IDI_MOONINFO), IMAGE_ICON,
                                GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
  wc.hIconSm = (HICON) LoadImageW(instance, MAKEINTRESOURCEW(IDI_MOONINFO), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.lpszClassName = L"MoonInfoWindow";
  RegisterClassExW(&wc);
  WNDCLASSEXW pc = { sizeof pc };
  pc.lpfnWndProc = panelProc;
  pc.hInstance = instance;
  pc.hCursor = LoadCursor(NULL, IDC_ARROW);
  pc.lpszClassName = L"MoonInfoPanel";
  RegisterClassExW(&pc);
  WNDCLASSEXW cc = { sizeof cc };
  cc.lpfnWndProc = chartProc;
  cc.hInstance = instance;
  cc.hCursor = LoadCursor(NULL, IDC_ARROW);
  cc.lpszClassName = L"MoonInfoChart";
  RegisterClassExW(&cc);
  startCharts();

  // The window: the data column (with its scroll bar), then the picture.
  imageX = panelWidth + (int) (GetSystemMetrics(SM_CXVSCROLL) / dpiScale + 0.5) + 12;
  RECT r = { 0, 0, S(imageX + imageSize + margin), S(630) };
  AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, TRUE, WS_EX_CONTROLPARENT);
  mainWindow = CreateWindowExW(WS_EX_CONTROLPARENT, L"MoonInfoWindow", widen("MoonInfo " MOONINFO_VERSION).c_str(),
                               WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                               r.right - r.left, r.bottom - r.top, NULL, createMenu(), instance, NULL);
  panel = CreateWindowExW(WS_EX_CONTROLPARENT, L"MoonInfoPanel", NULL,
                          WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN,
                          0, 0, 10, 10, mainWindow, NULL, instance, NULL);
  mapsLoaded = loadMoonMaps(programFolder() + L"data\\", maps);
  starCatalog.load(programFolder() + L"data\\moon_stars.bin");   // (without it, no stars)
  pictureSize = S(imageSize);
  createControls();

  // Tall enough to show all the data (within the screen), and wide enough
  // for the picture to fill the right side.
  RECT work;
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  RECT frame = { 0, 0, 0, 0 };
  AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW, TRUE, WS_EX_CONTROLPARENT);
  int clientH = std::min<int>(S(contentHeight), (work.bottom - work.top) - (frame.bottom - frame.top));
  int windowW = clientWidthFor(clientH) + (frame.right - frame.left), windowH = clientH + (frame.bottom - frame.top);
  RECT placed;
  GetWindowRect(mainWindow, &placed);
  // Moved up and left if it would run off the screen (the column is taller
  // with the charts).
  int left = std::max<int>(work.left, std::min<int>(placed.left, work.right - windowW));
  int top = std::max<int>(work.top, std::min<int>(placed.top, work.bottom - windowH));
  SetWindowPos(mainWindow, NULL, left, top, windowW, windowH, SWP_NOZORDER);
  fitPicture();
  applyTheme(settings.darkMode);

  update();
  ShowWindow(mainWindow, showCommand);
  UpdateWindow(mainWindow);
  SetTimer(mainWindow, TIMER_ID, 200, NULL);

  // First run (no settings yet): find the location, as the website does.
  if (! settings.stored)
    requestLocation(true);
  else
    setText(locationStatus, "Your saved location. Click \"Use My Location\" to find it again.");

  MSG msg;
  while (GetMessageW(&msg, NULL, 0, 0) > 0) {
    if (IsDialogMessageW(mainWindow, &msg))   // (Tab between the controls)
      continue;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  if (locator.joinable())
    locator.join();
  stopCharts();
  CoUninitialize();
  return 0;
}
