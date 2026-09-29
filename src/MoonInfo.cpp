// MoonInfo: the Moon Info website as a Windows program.
//
// Shows the Moon's phase, illumination, rise and set, the next quarters, its
// position and a picture of it, rotated to look as it does from the
// observer's location (or north up).  Built on GraphApp; the calculations
// are in MoonCalc.cpp and finding the location in Location.cpp.
//
// Copyright 2026 Steve Ferrell.

// GraphApp's headers need stdio/cstdlib/cstring before them, and have no
// extern "C" guard of their own.
#include <stdio.h>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include "resource.h"

extern "C" {
#include "app.h"
}

#include "Location.h"
#include "MoonCalc.h"
#include "config.h"

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

  std::string trim(const char * text)
  {
    std::string s(text ? text : "");
    std::size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
      return std::string();
    std::size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
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
    // (shlobj.h's SHGetKnownFolderPath would do, but it clashes with GraphApp.)
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
    snprintf(text, sizeof text, "%.*f", decimals, value);
    return text;
  }

  const char * degree = "\xC2\xB0";   // UTF-8

  //--------------------------------------------------------------------------
  // Settings
  //--------------------------------------------------------------------------

  struct Settings
  {
    std::string latitude = "30", longitude = "-90", elevation = "0";
    std::string date;               // when not automatic
    bool automatic = true;
    bool observerView = true;       // rotate the picture as seen from here
    bool darkMode = false;          // (first run: as Windows' app theme)
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
      std::string key = trim(line.substr(0, eq).c_str()), value = trim(line.substr(eq + 1).c_str());
      double number;
      if      (key == "latitude"  && parseNumber(value, number)) { s.latitude = value;  s.stored = true; }
      else if (key == "longitude" && parseNumber(value, number))   s.longitude = value;
      else if (key == "elevation" && parseNumber(value, number))   s.elevation = value;
      else if (key == "date")                                      s.date = value;
      else if (key == "automatic")                                 s.automatic = value != "0";
      else if (key == "observerView")                              s.observerView = value != "0";
      else if (key == "darkMode")                                  s.darkMode = value != "0";
    }
    return s;
  }

  void saveSettings(const Settings & s)
  {
    std::ofstream out(settingsFile(), std::ios::trunc);
    out << "latitude=" << s.latitude << "\nlongitude=" << s.longitude << "\nelevation=" << s.elevation
        << "\ndate=" << s.date << "\nautomatic=" << (s.automatic ? 1 : 0)
        << "\nobserverView=" << (s.observerView ? 1 : 0) << "\ndarkMode=" << (s.darkMode ? 1 : 0) << "\n";
  }

  //--------------------------------------------------------------------------
  // The window
  //--------------------------------------------------------------------------

  App *    app;
  Window * win;
  double   dpiScale = 1.0;
  Settings settings;

  Control *dateField, *autoCheck, *locationButton, *locationStatus;
  Control *latField, *lonField, *elevField, *viewCheck, *moonView;
  std::map<std::string, Control *> values;   // the results, by name

  // Space between the menu bar and the first row of controls
  const int topMargin = 10;

  int S(int n) { return static_cast<int>(n * dpiScale + 0.5); }
  Rect R(int x, int y, int w, int h) { return app_new_rect(S(x), S(y + topMargin), S(w), S(h)); }

  std::string fieldText(Control * c) { return trim(app_get_control_text(c)); }

  // Controls are redrawn only when their text changes (no flicker).
  void setText(Control * c, const std::string & text)
  {
    static std::map<Control *, std::string> shown;
    auto it = shown.find(c);
    if (it != shown.end() && it->second == text)
      return;
    shown[c] = text;
    app_set_control_text(c, text.c_str());
  }

  //--------------------------------------------------------------------------
  // The Moon's picture
  //--------------------------------------------------------------------------

  const int imageSize = 400;   // on screen, unscaled pixels

  Image * frameImage = NULL;   // the current NASA frame (730 x 730, north up)
  int     frameNumber = -1;
  Image * shownImage = NULL;   // frameImage, scaled and rotated for the screen
  double  shownAngle = 1e9;
  int     shownFrame = -1;

  Image * readFrame(int frame)
  {
    wchar_t name[32];
    swprintf(name, 32, L"images\\moon.%04d.jpg", frame);
    FILE * file = _wfopen((programFolder() + name).c_str(), L"rb");
    if (! file)
      return NULL;
    Image * img = app_read_image_file(file, 32);
    fclose(file);
    return img;
  }

  // The frame scaled to size x size and turned clockwise by angle degrees
  // (bilinear), clipped to a circle on black.
  Image * turnedImage(const Image * src, int size, double angle)
  {
    Image * out = app_new_image(size, size, 32);
    const double a = angle * 3.14159265358979323846 / 180, c = std::cos(a), s = std::sin(a);
    const double scale = double(src->width) / size, half = size / 2.0;
    const double r2 = half * half;
    for (int y = 0; y < size; ++y)
      for (int x = 0; x < size; ++x) {
        Colour & px = out->data32[y][x];
        px.alpha = 0; px.red = px.green = px.blue = 0;
        double dx = x + 0.5 - half, dy = y + 0.5 - half;
        if (dx * dx + dy * dy > r2)
          continue;
        // Undo the (clockwise, y down) turn to find the source point.
        double sx = ( dx * c + dy * s) * scale + src->width / 2.0 - 0.5;
        double sy = (-dx * s + dy * c) * scale + src->height / 2.0 - 0.5;
        int x0 = (int) std::floor(sx), y0 = (int) std::floor(sy);
        double fx = sx - x0, fy = sy - y0;
        double r = 0, g = 0, b = 0;
        for (int k = 0; k < 4; ++k) {
          int xi = x0 + (k & 1), yi = y0 + (k >> 1);
          if (xi < 0 || yi < 0 || xi >= src->width || yi >= src->height)
            continue;
          double w = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
          const Colour & sp = src->data32[yi][xi];
          r += w * sp.red; g += w * sp.green; b += w * sp.blue;
        }
        px.red = (byte) std::min(255.0, r + 0.5);
        px.green = (byte) std::min(255.0, g + 0.5);
        px.blue = (byte) std::min(255.0, b + 0.5);
      }
    return out;
  }

  void drawMoon(Control * c, Graphics * g)
  {
    Rect area = app_get_control_area(c);
    Rect local = app_new_rect(0, 0, area.width, area.height);
    app_set_rgb(g, app_new_rgb(0, 0, 0));
    app_fill_rect(g, local);
    if (shownImage)
      app_draw_image(g, local, shownImage, app_new_rect(0, 0, shownImage->width, shownImage->height));
    else if (frameNumber >= 0) {
      app_set_rgb(g, app_new_rgb(200, 200, 200));
      const char * text = "(the Moon's picture, images\\moon.NNNN.jpg, is missing)";
      app_draw_utf8(g, app_new_point(S(10), area.height / 2), text, (int) strlen(text));
    }
  }

  // Show the frame, turned by angle; redrawn only when it changes visibly.
  void showMoon(int frame, double angle)
  {
    if (frame != frameNumber) {
      if (frameImage)
        app_del_image(frameImage);
      frameImage = readFrame(frame);
      frameNumber = frame;
    }
    if (frame == shownFrame && std::fabs(angle - shownAngle) < 0.05 && (shownImage || ! frameImage))
      return;
    if (shownImage)
      app_del_image(shownImage);
    shownImage = frameImage ? turnedImage(frameImage, S(imageSize), angle) : NULL;
    shownFrame = frame;
    shownAngle = angle;
    app_redraw_control(moonView);
  }

  //--------------------------------------------------------------------------
  // Finding the location (on a worker thread)
  //--------------------------------------------------------------------------

  std::thread         locator;
  std::atomic<bool>   locating(false), located(false);
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
    located = false;
    setText(locationStatus, automatic ? "Detecting your location..." : "Locating...");
    app_disable(locationButton);
    locator = std::thread([]() {
      Location where = findLocation();
      std::lock_guard<std::mutex> lock(locationMutex);
      foundLocation = where;
      located = true;
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
    located = false;
    locating = false;
    app_enable(locationButton);
    if (! where.found) {
      setText(locationStatus, locatingAutomatically
        ? "Couldn't find your location (" + where.problem + "). Enter it, or click \"Use My Location\"."
        : "Couldn't find your location: " + where.problem + ".");
      return;
    }
    app_set_control_text(latField, fixed(where.latitude, 4).c_str());
    app_set_control_text(lonField, fixed(where.longitude, 4).c_str());
    if (where.hasElevation)
      app_set_control_text(elevField, fixed(where.elevation, 0).c_str());
    setText(locationStatus, "Location from " + where.source + ".");
  }

  //--------------------------------------------------------------------------
  // Updating
  //--------------------------------------------------------------------------

  void setValue(const char * name, const std::string & text) { setText(values[name], text); }

  void clearValues()
  {
    for (auto & v : values)
      setText(v.second, "");
  }

  std::string lastInputs;   // the inputs of the last calculation
  std::time_t lastTime = 0;

  void update()
  {
    bool automatic = app_is_checked(autoCheck) != 0;
    bool observerView = app_is_checked(viewCheck) != 0;
    std::time_t now = std::time(NULL), when = now;
    if (automatic) {
      if (now != lastTime)
        app_set_control_text(dateField, formatLocal(now).c_str());
    }
    std::string dateText = fieldText(dateField);
    std::string latText = fieldText(latField), lonText = fieldText(lonField), elevText = fieldText(elevField);
    std::string inputs = dateText + "|" + latText + "|" + lonText + "|" + elevText + "|"
                         + (automatic ? "a" : "m") + (observerView ? "v" : "n");
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
      clearValues();
      setValue("problem", "Check the date and time (YYYY-MM-DD HH:MM:SS) and the coordinates.");
      return;
    }

    // Remember the settings.
    Settings now_settings;
    now_settings.latitude = latText;
    now_settings.longitude = lonText;
    now_settings.elevation = elevText;
    now_settings.automatic = automatic;
    now_settings.observerView = observerView;
    now_settings.date = automatic ? settings.date : dateText;
    now_settings.darkMode = settings.darkMode;
    if (now_settings.latitude != settings.latitude || now_settings.longitude != settings.longitude ||
        now_settings.elevation != settings.elevation || now_settings.automatic != settings.automatic ||
        now_settings.observerView != settings.observerView || now_settings.date != settings.date) {
      settings = now_settings;
      saveSettings(settings);
    }

    MoonInfo moon = calculate(double(when), where);
    setValue("problem", "");
    setValue("phase", fixed(moon.phase, 3) + degree);
    setValue("illumination", fixed(moon.illumination * 100, 2) + "%");
    setValue("moonrise", moon.riseFound ? formatLocal(moon.rise) : "none within 300 days");
    setValue("moonset", moon.setFound ? formatLocal(moon.set) : "none within 300 days");
    for (int i = 0; i < 4; ++i) {
      setValue(("quarterName" + std::to_string(i)).c_str(), quarterName(moon.quarters[i].quarter));
      setValue(("quarterTime" + std::to_string(i)).c_str(), formatLocal(moon.quarters[i].time));
    }
    setValue("azimuth", fixed(moon.azimuth, 2) + degree);
    setValue("altitude", fixed(moon.altitude, 2) + degree);
    setValue("parallactic", fixed(moon.parallactic, 2) + degree);
    setValue("ra", fixed(moon.ra, 2) + " h");
    setValue("dec", fixed(moon.dec, 2) + degree);
    showMoon(moon.frame, observerView ? moon.parallactic : 0);
  }

  void onTimer(Timer *)
  {
    if (located)
      useFoundLocation();
    update();
  }

  //--------------------------------------------------------------------------
  // Controls and menus
  //--------------------------------------------------------------------------

  void onAutomatic(Control *)
  {
    // Unchecked, the field keeps the time shown, for the user to edit.
    if (app_is_checked(autoCheck))
      app_disable(dateField);
    else
      app_enable(dateField);
    update();
  }

  void onView(Control *) { update(); }
  void onLocationButton(Control *) { requestLocation(false); }

  void onExitMenu(MenuItem *) { app_del_window(win); }

  void onAbout(MenuItem *)
  {
    std::wstring text = widen(
      "MoonInfo " MOONINFO_VERSION "\n"
      "The Moon's phase, rise and set, quarters and position, and its picture as seen "
      "from your location.\n\n"
      "(c) 2026 Steve Ferrell, https://lidarwidgets.com\n\n"
      "Calculations: Astronomy Engine, (c) 2019-2023 Don Cross (MIT licence).\n"
      "Moon images: NASA's Scientific Visualization Studio.\n"
      "Location: Windows location services, or ipinfo.io.\n"
      "GUI: GraphApp.");
    MessageBoxW(NULL, text.c_str(), L"About MoonInfo", MB_OK | MB_ICONINFORMATION);
  }

  void onHelp(MenuItem *)
  {
    std::wstring text = widen(
      "Date and Time: with Automatic checked, the computer's clock (updated every second); "
      "unchecked, type a local date and time, YYYY-MM-DD HH:MM:SS.\n\n"
      "Location: \"Use My Location\" asks Windows' location service (if it's turned on for "
      "desktop apps in Windows' privacy settings), or failing that, looks up the approximate "
      "location of your internet address. Or type the latitude and longitude (degrees; north "
      "and east are positive) and the elevation in metres.\n\n"
      "Times are shown in the computer's time zone.\n\n"
      "Parallactic angle: the angle between celestial north and straight up at the Moon. "
      "With \"As seen from my location\" checked, the picture is turned by it so it's tilted as "
      "the Moon appears in your sky (roughly upside down in the southern hemisphere); "
      "unchecked, it's shown north up.\n\n"
      "Dark mode: light text on a dark window. On the first run it follows Windows' app "
      "theme (Settings > Personalization > Colors).\n\n"
      "Your settings are kept in %APPDATA%\\MoonInfo\\settings.ini.");
    MessageBoxW(NULL, text.c_str(), L"MoonInfo Help", MB_OK | MB_ICONINFORMATION);
  }

  void onClose(Window *)
  {
    app_del_window(win);
  }

  BOOL CALLBACK setIcon(HWND hwnd, LPARAM)
  {
    HINSTANCE instance = GetModuleHandle(NULL);
    HICON bigIcon   = (HICON) LoadImage(instance, MAKEINTRESOURCE(IDI_MOONINFO), IMAGE_ICON,
                                        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    HICON smallIcon = (HICON) LoadImage(instance, MAKEINTRESOURCE(IDI_MOONINFO), IMAGE_ICON,
                                        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (bigIcon)   SendMessage(hwnd, WM_SETICON, ICON_BIG,   (LPARAM) bigIcon);
    if (smallIcon) SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM) smallIcon);
    return TRUE;
  }

  //--------------------------------------------------------------------------
  // Dark mode
  //--------------------------------------------------------------------------

  Control * darkCheck;
  Colour lightWindow, lightField, lightButton, lightText;   // GraphApp's own colours
  bool lightSaved = false;

  // Windows' dark title bar (Windows 10 20H1 and later; 19 before that).
  BOOL CALLBACK setTitleBar(HWND hwnd, LPARAM dark)
  {
    BOOL value = dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &value, sizeof value)))
      DwmSetWindowAttribute(hwnd, 19, &value, sizeof value);
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    return TRUE;
  }

  void applyTheme(bool dark)
  {
    if (! lightSaved) {
      lightWindow = app_get_window_background(win);
      lightField = app_get_control_background(latField);
      lightButton = app_get_control_background(locationButton);
      lightText = app_get_control_foreground(latField);
      lightSaved = true;
    }
    Colour window = dark ? app_new_rgb(28, 28, 30) : lightWindow;
    Colour field  = dark ? app_new_rgb(48, 48, 52) : lightField;
    Colour button = dark ? app_new_rgb(64, 64, 68) : lightButton;
    Colour text   = dark ? app_new_rgb(230, 230, 230) : lightText;
    app_set_window_background(win, window);
    for (int i = 0; i < win->num_children; ++i) {
      Control * c = win->children[i];
      if (c == moonView)
        continue;
      if (c == dateField || c == latField || c == lonField || c == elevField)
        app_set_control_background(c, field);
      else if (c == locationButton)
        app_set_control_background(c, button);
      app_set_control_foreground(c, text);
    }
    EnumThreadWindows(GetCurrentThreadId(), setTitleBar, dark ? 1 : 0);
    app_redraw_window(win);
  }

  void onDarkMode(Control *)
  {
    settings.darkMode = app_is_checked(darkCheck) != 0;
    saveSettings(settings);
    applyTheme(settings.darkMode);
  }

  // Layout, in unscaled (96 dpi) pixels.
  const int margin = 12, labelWidth = 150, valueX = margin + labelWidth, valueWidth = 240;
  const int rowHeight = 26, imageX = valueX + valueWidth + 24;

  void addLabel(int y, const char * text)
  {
    app_new_label(win, R(margin, y + 4, labelWidth, rowHeight - 4), text, ALIGN_LEFT);
  }

  Control * addField(int y, int w, const char * text, const char * allowed)
  {
    Control * f = app_new_field(win, R(valueX, y, w, rowHeight), text);
    app_set_field_allowed_chars(f, allowed);
    app_set_field_disallowed_chars(f, "\t");
    return f;
  }

  void addValue(int y, const char * label, const char * name)
  {
    addLabel(y, label);
    values[name] = app_new_label(win, R(valueX, y + 4, valueWidth, rowHeight - 4), "", ALIGN_LEFT);
  }
}

//----------------------------------------------------------------------------

int main(int argc, char * argv[])
{
  SetProcessDPIAware();
  HDC screen = GetDC(NULL);
  dpiScale = GetDeviceCaps(screen, LOGPIXELSY) / 96.0;
  ReleaseDC(NULL, screen);

  settings = loadSettings();

  app = app_new_app(argc, argv);
  win = app_new_window(app, app_new_rect(S(80), S(60), S(imageX + imageSize + margin), S(600)),
                       "MoonInfo " MOONINFO_VERSION, STANDARD_WINDOW | MENUBAR);
  app_on_window_close(win, onClose);

  MenuBar * mb = app_new_menu_bar(win);
  Menu * m = app_new_menu(mb, "File");
  app_new_menu_item(m, "Exit", 0, onExitMenu);
  m = app_new_menu(mb, "Help");
  app_new_menu_item(m, "Using MoonInfo", 0, onHelp);
  app_new_menu_item(m, "-", 0, NULL);
  app_new_menu_item(m, "About MoonInfo", 0, onAbout);

  int y = 12;
  addLabel(y, "Date and time:");
  dateField = addField(y, 170, settings.automatic || settings.date.empty() ? formatLocal(std::time(NULL)).c_str()
                                                                              : settings.date.c_str(), "0123456789-: ");
  autoCheck = app_new_check_box(win, R(valueX + 178, y, 110, rowHeight), "Automatic", onAutomatic);
  y += 32;
  addLabel(y, "Location:");
  locationButton = app_new_button(win, R(valueX, y, 140, rowHeight), "Use My Location", onLocationButton);
  y += 30;
  locationStatus = app_new_label(win, R(margin, y, imageX - margin - 12, rowHeight), "", ALIGN_LEFT);
  y += rowHeight + 6;
  addLabel(y, "Latitude:");
  latField = addField(y, 110, settings.latitude.c_str(), "0123456789.-+");
  y += 30;
  addLabel(y, "Longitude:");
  lonField = addField(y, 110, settings.longitude.c_str(), "0123456789.-+");
  y += 30;
  addLabel(y, "Elevation (m):");
  elevField = addField(y, 110, settings.elevation.c_str(), "0123456789.-+");
  y += 38;

  addValue(y, "Phase:", "phase");                  y += rowHeight;
  addValue(y, "Illumination:", "illumination");    y += rowHeight;
  addValue(y, "Moonrise:", "moonrise");            y += rowHeight;
  addValue(y, "Moonset:", "moonset");              y += rowHeight;
  for (int i = 0; i < 4; ++i) {
    std::string n = std::to_string(i);
    values["quarterName" + n] = app_new_label(win, R(margin, y + 4, labelWidth, rowHeight - 4), "", ALIGN_LEFT);
    values["quarterTime" + n] = app_new_label(win, R(valueX, y + 4, valueWidth, rowHeight - 4), "", ALIGN_LEFT);
    y += rowHeight;
  }
  addValue(y, "Azimuth:", "azimuth");              y += rowHeight;
  addValue(y, "Altitude:", "altitude");            y += rowHeight;
  addValue(y, "Parallactic angle:", "parallactic"); y += rowHeight;
  addValue(y, "RA (J2000):", "ra");                y += rowHeight;
  addValue(y, "Dec (J2000):", "dec");              y += rowHeight + 6;

  moonView = app_new_control(win, R(imageX, 12, imageSize, imageSize));
  app_on_control_redraw(moonView, drawMoon);
  viewCheck = app_new_check_box(win, R(imageX, 12 + imageSize + 8, imageSize, rowHeight),
                                "As seen from my location (unchecked: north up)", onView);
  darkCheck = app_new_check_box(win, R(imageX, 12 + imageSize + 38, imageSize, rowHeight), "Dark mode", onDarkMode);
  values["problem"] = app_new_label(win, R(imageX, 12 + imageSize + 72, imageSize, 2 * rowHeight), "", ALIGN_LEFT);

  if (settings.automatic) {
    app_check(autoCheck);
    app_disable(dateField);
  }
  if (settings.observerView)
    app_check(viewCheck);
  if (settings.darkMode) {
    app_check(darkCheck);
    applyTheme(true);
  }

  update();
  app_new_timer(app, onTimer, 200);
  app_show_window(win);
  EnumThreadWindows(GetCurrentThreadId(), setIcon, 0);

  // First run (no settings yet): find the location, as the website does.
  if (! settings.stored)
    requestLocation(true);
  else
    setText(locationStatus, "Your saved location. Click \"Use My Location\" to find it again.");

  app_main_loop(app);
  if (locator.joinable())
    locator.join();
  return 0;
}
