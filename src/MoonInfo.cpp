// MoonInfo: the Moon Info website as a Windows program.
//
// Shows the Moon's phase, illumination, distance, rise and set, the next
// quarters, its position and a picture of it, turned to look as it does
// from the observer's location (or north up).  Plain Win32: the data are in
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
    }
    return s;
  }

  void saveSettings(const Settings & s)
  {
    std::ofstream out(settingsFile(), std::ios::trunc);
    out << "latitude=" << s.latitude << "\nlongitude=" << s.longitude << "\nelevation=" << s.elevation
        << "\ndate=" << s.date << "\nautomatic=" << (s.automatic ? 1 : 0)
        << "\nobserverView=" << (s.observerView ? 1 : 0) << "\ndarkMode=" << (s.darkMode ? 1 : 0)
        << "\nimperial=" << (s.imperial ? 1 : 0) << "\n";
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
  HWND viewCheck, darkCheck, unitsCheck, problemLabel;
  std::map<std::string, HWND> values;   // the results, by name

  enum { ID_AUTO = 100, ID_LOCATION, ID_VIEW, ID_DARK, ID_UNITS, ID_EXIT, ID_HELP, ID_ABOUT };
  const UINT WM_LOCATED = WM_APP + 1;   // the location worker has finished
  const UINT_PTR TIMER_ID = 1;

  // Layout, in unscaled (96 dpi) pixels.
  const int margin = 12, labelWidth = 150, valueX = margin + labelWidth, valueWidth = 270, rowHeight = 26;
  const int panelWidth = valueX + valueWidth + 8;   // the data column (its scroll bar is added to it)
  const int imageSize = 400;
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

  struct Picture
  {
    int width = 0, height = 0;
    std::vector<std::uint32_t> pixels;   // BGRA, top row first
  };

  Picture frame;          // the current NASA frame (730 x 730, north up)
  int frameNumber = -1;
  Picture shown;          // frame, scaled and turned for the screen
  double shownAngle = 1e9;
  int shownFrame = -1;

  // images\moon.NNNN.jpg, decoded with Windows Imaging Component.
  bool readFrame(int number, Picture & picture)
  {
    wchar_t name[32];
    swprintf(name, 32, L"images\\moon.%04d.jpg", number);
    std::wstring path = programFolder() + name;
    IWICImagingFactory * factory = NULL;
    IWICBitmapDecoder * decoder = NULL;
    IWICBitmapFrameDecode * source = NULL;
    IWICBitmapSource * converted = NULL;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), NULL, GENERIC_READ,
                                                     WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &source)) &&
        SUCCEEDED(WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, source, &converted))) {
      UINT w = 0, h = 0;
      converted->GetSize(&w, &h);
      picture.width = (int) w;
      picture.height = (int) h;
      picture.pixels.assign(std::size_t(w) * h, 0);
      ok = SUCCEEDED(converted->CopyPixels(NULL, w * 4, w * h * 4, (BYTE *) picture.pixels.data()));
    }
    if (converted) converted->Release();
    if (source)    source->Release();
    if (decoder)   decoder->Release();
    if (factory)   factory->Release();
    if (! ok)
      picture = Picture();
    return ok;
  }

  // The frame scaled to size x size and turned clockwise by angle degrees
  // (bilinear), clipped to a circle on black.
  void turnPicture(const Picture & src, int size, double angle, Picture & out)
  {
    out.width = out.height = size;
    out.pixels.assign(std::size_t(size) * size, 0);
    const double a = angle * 3.14159265358979323846 / 180, c = std::cos(a), s = std::sin(a);
    const double scale = double(src.width) / size, half = size / 2.0, r2 = half * half;
    for (int y = 0; y < size; ++y)
      for (int x = 0; x < size; ++x) {
        double dx = x + 0.5 - half, dy = y + 0.5 - half;
        if (dx * dx + dy * dy > r2)
          continue;
        // Undo the (clockwise, y down) turn to find the source point.
        double sx = ( dx * c + dy * s) * scale + src.width / 2.0 - 0.5;
        double sy = (-dx * s + dy * c) * scale + src.height / 2.0 - 0.5;
        int x0 = (int) std::floor(sx), y0 = (int) std::floor(sy);
        double fx = sx - x0, fy = sy - y0, rgb[3] = { 0, 0, 0 };
        for (int k = 0; k < 4; ++k) {
          int xi = x0 + (k & 1), yi = y0 + (k >> 1);
          if (xi < 0 || yi < 0 || xi >= src.width || yi >= src.height)
            continue;
          double w = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
          std::uint32_t p = src.pixels[std::size_t(yi) * src.width + xi];
          rgb[0] += w * (p & 0xFF); rgb[1] += w * ((p >> 8) & 0xFF); rgb[2] += w * ((p >> 16) & 0xFF);
        }
        auto channel = [](double v) { return (std::uint32_t) std::min(255.0, v + 0.5); };
        out.pixels[std::size_t(y) * size + x] =
          0xFF000000u | (channel(rgb[2]) << 16) | (channel(rgb[1]) << 8) | channel(rgb[0]);
      }
  }

  RECT pictureRect()
  {
    RECT r = { S(imageX), S(margin), S(imageX + imageSize), S(margin + imageSize) };
    return r;
  }

  void paintPicture(HDC dc)
  {
    RECT r = pictureRect();
    FillRect(dc, &r, (HBRUSH) GetStockObject(BLACK_BRUSH));
    if (! shown.pixels.empty()) {
      BITMAPINFO bi = {};
      bi.bmiHeader.biSize = sizeof bi.bmiHeader;
      bi.bmiHeader.biWidth = shown.width;
      bi.bmiHeader.biHeight = -shown.height;   // top row first
      bi.bmiHeader.biPlanes = 1;
      bi.bmiHeader.biBitCount = 32;
      bi.bmiHeader.biCompression = BI_RGB;
      SetDIBitsToDevice(dc, r.left, r.top, shown.width, shown.height, 0, 0, 0, shown.height,
                        shown.pixels.data(), &bi, DIB_RGB_COLORS);
    }
    else if (frameNumber >= 0) {
      SetTextColor(dc, RGB(200, 200, 200));
      SetBkMode(dc, TRANSPARENT);
      HFONT old = (HFONT) SelectObject(dc, font);
      DrawTextW(dc, L"(The Moon's picture, images\\moon.NNNN.jpg, is missing.)", -1, &r,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      SelectObject(dc, old);
    }
  }

  // Show the frame, turned by angle; redrawn only when it changes visibly.
  void showMoon(int number, double angle)
  {
    if (number != frameNumber) {
      readFrame(number, frame);
      frameNumber = number;
    }
    if (number == shownFrame && std::fabs(angle - shownAngle) < 0.05)
      return;
    if (frame.pixels.empty())
      shown = Picture();
    else
      turnPicture(frame, S(imageSize), angle, shown);
    shownFrame = number;
    shownAngle = angle;
    RECT r = pictureRect();
    InvalidateRect(mainWindow, &r, FALSE);
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

  void update()
  {
    bool automatic = isChecked(autoCheck), observerView = isChecked(viewCheck);
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
    if (now_settings.latitude != settings.latitude || now_settings.longitude != settings.longitude ||
        now_settings.elevation != settings.elevation || now_settings.automatic != settings.automatic ||
        now_settings.observerView != settings.observerView || now_settings.date != settings.date) {
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
      setValue(("quarterName" + std::to_string(i)).c_str(), std::string(quarterName(moon.quarters[i].quarter)) + ":");
      setValue(("quarterTime" + std::to_string(i)).c_str(), formatLocal(moon.quarters[i].time));
    }
    setValue("azimuth", fixed(moon.azimuth, 2) + degree);
    setValue("altitude", fixed(moon.altitude, 2) + degree);
    setValue("parallactic", fixed(moon.parallactic, 2) + degree);
    setValue("ra", fixed(moon.ra, 2) + " h");
    setValue("dec", fixed(moon.dec, 2) + degree);
    showMoon(moon.frame, observerView ? moon.parallactic : 0);
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
    EnableWindow(dateEdit, ! isChecked(autoCheck));
    update();
  }

  // Switch between metres and km, and feet and miles; the elevation typed
  // in is converted, so the place stays the same.
  void onUnits()
  {
    bool imperial = isChecked(unitsCheck);
    if (imperial == settings.imperial)
      return;
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

  void onDarkMode()
  {
    settings.darkMode = isChecked(darkCheck);
    saveSettings(settings);
    applyTheme(settings.darkMode);
  }

  void showAbout()
  {
    std::wstring text = widen(
      "MoonInfo " MOONINFO_VERSION "\n"
      "The Moon's phase, distance, rise and set, quarters and position, and its picture as "
      "seen from your location.\n\n"
      "(c) 2026 Steve Ferrell, https://lidarwidgets.com\n\n"
      "Calculations: Astronomy Engine, (c) 2019-2023 Don Cross (MIT licence).\n"
      "Moon images: NASA's Scientific Visualization Studio.\n"
      "Location: Windows location services, or ipinfo.io.");
    MessageBoxW(mainWindow, text.c_str(), L"About MoonInfo", MB_OK | MB_ICONINFORMATION);
  }

  void showHelp()
  {
    std::wstring text = widen(
      "Date and Time: with Automatic checked, the computer's clock (updated every second); "
      "unchecked, type a local date and time, YYYY-MM-DD HH:MM:SS.\n\n"
      "Location: \"Use My Location\" asks Windows' location service (if it's turned on for "
      "desktop apps in Windows' privacy settings), or failing that, looks up the approximate "
      "location of your internet address. Or type the latitude and longitude (degrees; north "
      "and east are positive) and the elevation.\n\n"
      "Times are shown in the computer's time zone. Distance: from the Earth's centre to the "
      "Moon's.\n\n"
      "Parallactic angle: the angle between celestial north and straight up at the Moon. "
      "With \"As seen from my location\" checked, the picture is turned by it so it's tilted as "
      "the Moon appears in your sky (roughly upside down in the southern hemisphere); "
      "unchecked, it's shown north up.\n\n"
      "Miles and feet: the distance in miles and the elevation in feet (unchecked: km and "
      "metres).\n\n"
      "Dark mode: light text on a dark window. On the first run it follows Windows' app "
      "theme (Settings > Personalization > Colors).\n\n"
      "If the window is too short for all the data, scroll it with the scroll bar or the "
      "mouse wheel.\n\n"
      "Your settings are kept in %APPDATA%\\MoonInfo\\settings.ini.");
    MessageBoxW(mainWindow, text.c_str(), L"MoonInfo Help", MB_OK | MB_ICONINFORMATION);
  }

  void onCommand(WPARAM wp)
  {
    switch (LOWORD(wp)) {
      case ID_AUTO:     onAutomatic(); break;
      case ID_LOCATION: requestLocation(false); break;
      case ID_VIEW:     update(); break;
      case ID_DARK:     onDarkMode(); break;
      case ID_UNITS:    onUnits(); break;
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
      case WM_TIMER:          update(); return 0;
      case WM_LOCATED:        useFoundLocation(); update(); return 0;
      case WM_MOUSEWHEEL:     onWheel(wp); return 0;   // anywhere in the window scrolls the data
      case WM_SIZE: {
        // The data column runs the window's height.
        RECT r;
        GetClientRect(hwnd, &r);
        MoveWindow(panel, 0, 0, S(panelWidth) + GetSystemMetrics(SM_CXVSCROLL), r.bottom, TRUE);
        return 0;
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
    addValue(y, "Distance:", "distance");             y += rowHeight;
    addValue(y, "Moonrise:", "moonrise");             y += rowHeight;
    addValue(y, "Moonset:", "moonset");               y += rowHeight;
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
    contentHeight = y + margin;

    // Beside the picture
    int cy = margin + imageSize + 8;
    viewCheck = makeControl(mainWindow, L"BUTTON", "As seen from my location (unchecked: north up)",
                            WS_TABSTOP | BS_AUTOCHECKBOX, imageX, cy, imageSize, rowHeight, ID_VIEW);
    darkCheck = makeControl(mainWindow, L"BUTTON", "Dark mode", WS_TABSTOP | BS_AUTOCHECKBOX,
                            imageX, cy + 28, imageSize, rowHeight, ID_DARK);
    unitsCheck = makeControl(mainWindow, L"BUTTON", "Miles and feet (unchecked: km and metres)",
                             WS_TABSTOP | BS_AUTOCHECKBOX, imageX, cy + 56, imageSize, rowHeight, ID_UNITS);
    problemLabel = makeControl(mainWindow, L"STATIC", "", SS_LEFT, imageX, cy + 88, imageSize, 2 * rowHeight);

    check(autoCheck, settings.automatic);
    EnableWindow(dateEdit, ! settings.automatic);
    check(viewCheck, settings.observerView);
    check(darkCheck, settings.darkMode);
    check(unitsCheck, settings.imperial);
  }

  HMENU createMenu()
  {
    HMENU bar = CreateMenu(), file = CreatePopupMenu(), help = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, ID_EXIT, L"E&xit");
    AppendMenuW(help, MF_STRING, ID_HELP, L"&Using MoonInfo");
    AppendMenuW(help, MF_SEPARATOR, 0, NULL);
    AppendMenuW(help, MF_STRING, ID_ABOUT, L"&About MoonInfo");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR) file, L"&File");
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
  INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES };
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
  createControls();
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
  CoUninitialize();
  return 0;
}
