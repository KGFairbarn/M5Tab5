#include "DailyPlanner.h"
#if __has_include("Secrets.h")
#include "Secrets.h"
#else
#include "Secrets.h.example"
#endif
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>
#include <ctype.h>

// =========================== USER SETTINGS ============================
// Credentials live in gitignored src/Secrets.h (see src/Secrets.h.example)
const char* CALENDAR_SCRIPT_URL = SECRET_CALENDAR_SCRIPT_URL;

const char* OPENWEATHER_KEY    = SECRET_OPENWEATHER_KEY;
const char* TEMPEST_TOKEN      = SECRET_TEMPEST_TOKEN;
const char* TEMPEST_STATION_ID = SECRET_TEMPEST_STATION_ID;
const char* LATITUDE           = "43.1384";
const char* LONGITUDE          = "-70.9366";

// Time Settings
const char* TZ_RULE            = "EST5EDT,M3.2.0,M11.1.0"; // US Eastern
const char* ntpServer1         = "pool.ntp.org";
const char* ntpServer2         = "time.nist.gov";

const uint32_t OPENWEATHER_INTERVAL_MS = 30UL * 60UL * 1000UL;
const uint32_t TEMPEST_INTERVAL_MS     =  3UL * 60UL * 1000UL;
const uint32_t CALENDAR_INTERVAL_MS    = 15UL * 60UL * 1000UL;
const uint32_t TEMPEST_HISTORY_MS      = 15UL * 60UL * 1000UL;
static const int HISTORY_SLOTS = 96;

// ======================== DASHBOARD PALETTE ===========================
constexpr uint16_t DP_BG     = 0x10C5; // #171B28
constexpr uint16_t DP_PANEL  = 0x2126; // #202737
constexpr uint16_t DP_PANEL2 = 0x2988; // #2A3246
constexpr uint16_t DP_LINE   = 0x865D; // pale blue
constexpr uint16_t DP_LIME   = 0xF7A8; // luminous yellow
constexpr uint16_t DP_WHITE  = 0xFFFF;
constexpr uint16_t DP_MUTED  = 0xA599;
constexpr uint16_t DP_CYAN   = 0x56BF;
constexpr uint16_t DP_GOLD   = 0xFDC9;
constexpr uint16_t DP_GREEN  = 0x86D5;
constexpr uint16_t DP_RED    = 0xF455;
constexpr uint16_t DP_BORDER = 0x31C9;

// 240x320 -> 720x1280 Scaling Helpers (3x horizontal, 3.6x vertical + 90px top bar)
static const int TOP_BAR_H = 90;
static inline int SX(int x) { return x * 3; }
static inline int SY(int y) { return TOP_BAR_H + (y * 37) / 10; }
static inline int SW(int w) { return w * 3; }
static inline int SH(int h) { return (h * 37) / 10; }

// ======================== DATA STRUCTURES =============================
struct CurrentWeather {
    bool valid = false;
    float tempF = NAN;
    float feelsF = NAN;
    float windMph = NAN;
    float gustMph = NAN;
    int windDeg = 0;
    int humidity = -1;
    int clouds = -1;
    float pressureHpa = NAN;
    float rainIn = NAN;
    int weatherId = 800;
    String description = "";
    time_t sunrise = 0;
    time_t sunset = 0;
    time_t timestamp = 0;
};

struct ForecastDay {
    bool valid = false;
    float lowF = 9999.0f;
    float highF = -9999.0f;
    int weatherId = 800;
    int popPercent = 0;
    long bestNoonDistance = 99999999L;
    char dayName[5] = {0};
};

struct CalEvent {
    String time;
    String title;
};

struct KidHour {
    bool valid = false;
    float tempF = NAN;
    int weatherId = 800;
};

struct KidDay {
    bool valid = false;
    float lowF = NAN;
    float highF = NAN;
    int maxRainPercent = 0;
    float strongestWindMph = 0.0f;
    bool snowPossible = false;
    bool rainPossible = false;
    int weatherId = 800;
    KidHour hours[3]; // 8 AM, NOON, 5 PM
};

static CurrentWeather owm;
static CurrentWeather tempest;
static ForecastDay forecast[3];
static KidDay kidToday;
static KidDay kidTomorrow;

static CalEvent todayEvents[3];
static int numTodayEvents = 0;
static CalEvent tomorrowEvents[3];
static int numTomorrowEvents = 0;
static uint32_t lastCalendarRequest = 0;
static bool calValid = false;

static bool ntpConfigured = false;
enum UIPage : uint8_t { PAGE_TODAY=0, PAGE_TOMORROW=1, PAGE_FORECAST=2, PAGE_TEMPEST=3 };
static UIPage activePage = PAGE_TODAY;
static bool showTempest = false;
static bool hasForecast = false;
static uint32_t lastOwmRequest = 0;
static uint32_t lastTempestRequest = 0;
static uint32_t lastHeaderDraw = 0;
static String owmError = "Awaiting OpenWeather";
static String tempestError = "Tempest not loaded";

static float historyTemp[HISTORY_SLOTS];
static float historyWind[HISTORY_SLOTS];
static float historyGust[HISTORY_SLOTS];
static float historyHum[HISTORY_SLOTS];
static float historyPressure[HISTORY_SLOTS];
static bool historyValid = false;
static bool historyAttempted = false;
static uint16_t historySampleCount = 0;
static uint8_t historyPage = 0;
static uint32_t lastHistoryRequest = 0;
static String historyError = "History not loaded";
static time_t historyWindowStart = 0;

#define FETCH_ONLINE_NATIONAL_DAYS 1
const uint32_t HOLIDAY_ROTATE_MS = 9000UL;
const uint32_t HOLIDAY_RETRY_MS  = 6UL * 60UL * 60UL * 1000UL;
static const int MAX_TODAY_DAYS = 5;
static String todaysDays[MAX_TODAY_DAYS];
static uint8_t todaysCount = 0;
static uint8_t todaysIndex = 0;
static int todaysDateKey = -1;
static bool todaysOnlineChecked = false;
static uint32_t lastHolidayFetch = 0;
static uint32_t lastHolidayRotate = 0;

// Forward declarations
static void drawDashboard();
static void drawHeader();
static void fetchTempestData();
static void fetchOpenWeatherForecast();
static void fetchCalendarData();
static void refreshTodaysHolidays();
static void drawHolidayBanner();
static void drawKidPage(bool isTomorrow);
static void refreshTempestHistory();
static void drawTempestHistory();
static void clearTempestHistory();

static float mpsToMph(float x) { return x * 2.2369363f; }
static float cToF(float x) { return x * 1.8f + 32.0f; }

// ======================== DRAWING HELPERS =============================
static void drawTopHubBar() {
    M5.Display.fillRect(0, 0, sw, TOP_BAR_H, 0x0862);
    M5.Display.fillRoundRect(20, 15, 200, 60, 14, DP_PANEL2);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(DP_LIME);
    M5.Display.drawString("< RUBY HUB", 120, 45);

    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    M5.Display.setTextColor(DP_WHITE);
    M5.Display.drawString("DAILY PLANNER", sw / 2 + 30, 45);

    M5.Display.fillRoundRect(sw - 160, 15, 140, 60, 14, DP_PANEL2);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(DP_CYAN);
    M5.Display.drawString("REFRESH", sw - 90, 45);
    M5.Display.drawFastHLine(0, TOP_BAR_H - 2, sw, DP_BORDER);
}

static void put(const String &s, int x, int y, int font, int scale, uint16_t color,
                textdatum_t datum = top_left, uint16_t bg = DP_BG) {
    M5.Display.setTextDatum(datum);
    if (font == 1) {
        M5.Display.setFont(&fonts::FreeSansBold12pt7b);
        M5.Display.setTextSize(scale);
    } else if (font == 2) {
        M5.Display.setFont(&fonts::FreeSansBold18pt7b);
        M5.Display.setTextSize(scale);
    } else if (font == 4) {
        M5.Display.setFont(&fonts::FreeSansBold24pt7b);
        M5.Display.setTextSize(scale);
    } else {
        M5.Display.setFont(&fonts::FreeSansBold12pt7b);
        M5.Display.setTextSize(scale);
    }
    M5.Display.setTextColor(color);
    M5.Display.drawString(s, SX(x), SY(y));
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(top_left);
}

static void divLine(int y) {
    M5.Display.fillRect(SX(8), SY(y), SW(224), 2, DP_CYAN);
}

static void iconFallback(int wx, int x, int y, int sz) {
    int px = SX(x), py = SY(y), psz = SW(sz);
    int cx = px + psz / 2, cy = py + psz / 2;
    int r = psz / 5;
    if (wx == 800 || wx == 801 || wx == 802) {
        M5.Display.fillCircle(cx + 12, cy - 15, r, DP_LIME);
        for (int deg = 0; deg < 360; deg += 45) {
            float a = deg * 0.0174533f;
            M5.Display.drawLine(cx + 12 + cosf(a) * (r + 6), cy - 15 + sinf(a) * (r + 6),
                                cx + 12 + cosf(a) * (r + 20), cy - 15 + sinf(a) * (r + 20), DP_LIME);
        }
        if (wx == 800) return;
    }
    M5.Display.fillRoundRect(cx - psz / 3, cy, psz * 2 / 3, psz / 4, psz / 12, DP_WHITE);
    M5.Display.fillCircle(cx - psz / 6, cy, psz / 7, DP_WHITE);
    M5.Display.fillCircle(cx + psz / 10, cy - psz / 9, psz / 5, DP_WHITE);
    if (wx >= 200 && wx < 700) {
        for (int i = -1; i <= 1; i++) {
            M5.Display.drawLine(cx + i * psz / 5, cy + psz / 6, cx + i * psz / 5 - 9, cy + psz / 3, DP_CYAN);
        }
    }
}

static void drawIcon(int weatherId, int x, int y, int size) {
    iconFallback(weatherId, x, y, size);
}

static int moonPhase(time_t now) {
    const double synodic = 29.530588853;
    double days = ((double)now - 947182440.0) / 86400.0;
    double age = fmod(days, synodic);
    if (age < 0) age += synodic;
    return (int)floor((age / synodic) * 8.0 + 0.5) % 8;
}

static void moonImage(int x, int y) {
    M5.Display.drawCircle(SX(x + 12), SY(y + 12), SW(10), DP_CYAN);
    M5.Display.fillCircle(SX(x + 13), SY(y + 12), SW(8), DP_WHITE);
}

static String localTimeString(time_t stamp, const char *fmt) {
    if (stamp < 1700000000) return "--:--";
    char buff[28];
    struct tm local;
    localtime_r(&stamp, &local);
    strftime(buff, sizeof(buff), fmt, &local);
    return String(buff);
}

// ================= KID-FRIENDLY SPECIAL DAYS =========================
struct LocalDay { uint8_t month, day; const char *name; };
static const LocalDay FUN_DAYS[] = {
    {1,4,"National Trivia Day"}, {1,5,"National Bird Day"},
    {1,9,"Static Electricity Day"}, {1,21,"Squirrel Appreciation Day"},
    {1,23,"National Pie Day"}, {1,24,"National Compliment Day"},
    {2,2,"Groundhog Day"}, {2,9,"National Pizza Day"},
    {2,14,"Valentine's Day"}, {2,17,"Random Acts of Kindness Day"},
    {2,27,"International Polar Bear Day"},
    {3,3,"World Wildlife Day"}, {3,14,"Pi Day"},
    {3,20,"World Storytelling Day"}, {3,21,"World Poetry Day"},
    {3,22,"World Water Day"}, {3,23,"National Puppy Day"},
    {4,1,"April Fools' Day"}, {4,2,"Children's Book Day"},
    {4,10,"National Siblings Day"}, {4,12,"Grilled Cheese Day"},
    {4,22,"Earth Day"}, {4,23,"World Book Day"},
    {4,29,"International Dance Day"},
    {5,4,"Star Wars Day"}, {5,12,"National Limerick Day"},
    {5,15,"Chocolate Chip Day"}, {5,20,"World Bee Day"},
    {5,23,"World Turtle Day"}, {5,28,"Hamburger Day"},
    {6,1,"World Milk Day"}, {6,3,"World Bicycle Day"},
    {6,4,"National Cheese Day"}, {6,5,"World Environment Day"},
    {6,6,"National Yo-Yo Day"}, {6,8,"World Oceans Day"},
    {6,21,"International Yoga Day"},
    {7,7,"World Chocolate Day"}, {7,17,"World Emoji Day"},
    {7,20,"International Chess Day"}, {7,29,"International Tiger Day"},
    {7,30,"International Friendship Day"},
    {8,2,"National Coloring Book Day"}, {8,4,"Chocolate Chip Cookie Day"},
    {8,8,"International Cat Day"}, {8,9,"Book Lovers Day"},
    {8,12,"World Elephant Day"}, {8,15,"National Relaxation Day"},
    {8,19,"World Photography Day"}, {8,26,"National Dog Day"},
    {9,6,"Read a Book Day"}, {9,8,"International Literacy Day"},
    {9,13,"Positive Thinking Day"}, {9,16,"National Guacamole Day"},
    {9,18,"National Cheeseburger Day"}, {9,21,"International Day of Peace"},
    {9,25,"National Comic Book Day"}, {9,29,"World Heart Day"},
    {10,4,"National Taco Day"}, {10,6,"National Noodle Day"},
    {10,12,"National Farmers Day"}, {10,14,"National Dessert Day"},
    {10,17,"National Pasta Day"}, {10,18,"Chocolate Cupcake Day"},
    {10,20,"International Chef Day"}, {10,21,"Pumpkin Cheesecake Day"},
    {10,22,"National Nut Day"}, {10,24,"National Food Day"},
    {10,25,"World Pasta Day"}, {10,25,"International Artist Day"},
    {10,26,"National Pumpkin Day"}, {10,27,"National Black Cat Day"},
    {10,28,"National Chocolate Day"}, {10,29,"National Oatmeal Day"},
    {10,30,"National Candy Corn Day"}, {10,31,"Halloween"},
    {11,3,"National Sandwich Day"}, {11,8,"National STEM Day"},
    {11,13,"World Kindness Day"}, {11,17,"Homemade Bread Day"},
    {11,20,"World Children's Day"}, {11,21,"World Hello Day"},
    {12,4,"National Cookie Day"}, {12,5,"International Volunteer Day"},
    {12,7,"Cotton Candy Day"}, {12,13,"National Cocoa Day"},
    {12,21,"Crossword Puzzle Day"}, {12,24,"Christmas Eve"},
    {12,31,"New Year's Eve"}
};

static void addToday(const String &input, bool first = false) {
    String name = input;
    name.trim();
    if (!name.length()) return;
    for (size_t k = 0; k < name.length(); k++) {
        unsigned char ch = (unsigned char)name[k];
        if (ch < 32 || ch > 126) return;
    }
    if (name.length() > 60) return;
    for (uint8_t i = 0; i < todaysCount; i++) {
        if (todaysDays[i].equalsIgnoreCase(name)) return;
    }
    if (first) {
        if (todaysCount >= MAX_TODAY_DAYS) todaysCount = MAX_TODAY_DAYS - 1;
        for (int i = todaysCount; i > 0; --i) todaysDays[i] = todaysDays[i - 1];
        todaysDays[0] = name;
        todaysCount++;
    } else if (todaysCount < MAX_TODAY_DAYS) {
        todaysDays[todaysCount++] = name;
    }
}

static int calendarWeekday(int year, int month, int day) {
    struct tm t = {};
    t.tm_year = year - 1900; t.tm_mon = month - 1; t.tm_mday = day;
    t.tm_hour = 12; t.tm_isdst = -1;
    if (mktime(&t) == (time_t)-1) return -1;
    return t.tm_wday;
}

static bool nthWeekday(const struct tm &today, int month, int weekday, int nth) {
    if (today.tm_mon + 1 != month || today.tm_wday != weekday) return false;
    return (today.tm_mday - 1) / 7 + 1 == nth;
}

static bool lastWeekday(const struct tm &today, int month, int weekday) {
    if (today.tm_mon + 1 != month || today.tm_wday != weekday) return false;
    struct tm next = {};
    next.tm_year = today.tm_year;
    next.tm_mon = month - 1;
    next.tm_mday = today.tm_mday + 7;
    next.tm_hour = 12; next.tm_isdst = -1;
    mktime(&next);
    return next.tm_mon != month - 1;
}

static bool fixedHoliday(const struct tm &today, int year, int month, int day) {
    const int w = calendarWeekday(year, month, day);
    int obsM = month, obsD = day, obsY = year;
    if (w == 6) {
        obsD--;
        if (obsD == 0) {
            obsM--;
            if (obsM == 0) { obsM = 12; obsY--; }
            static const int dm[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
            obsD = dm[obsM];
            if (obsM == 2 && ((obsY % 4 == 0 && obsY % 100 != 0) || obsY % 400 == 0)) obsD = 29;
        }
    }
    if (w == 0) {
        obsD++;
        int dm[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
        if (obsM == 2 && ((obsY % 4 == 0 && obsY % 100 != 0) || obsY % 400 == 0)) dm[2] = 29;
        if (obsD > dm[obsM]) { obsD = 1; obsM++; if (obsM == 13) { obsM = 1; obsY++; } }
    }
    bool actual = today.tm_year + 1900 == year && today.tm_mon + 1 == month && today.tm_mday == day;
    bool observed = today.tm_year + 1900 == obsY && today.tm_mon + 1 == obsM && today.tm_mday == obsD;
    return actual || observed;
}

static void computeLocalSpecialDays(const struct tm &d) {
    todaysCount = 0; todaysIndex = 0;
    int year = d.tm_year + 1900;
    if (fixedHoliday(d, year, 1, 1) || fixedHoliday(d, year + 1, 1, 1)) addToday("New Year's Day", true);
    if (nthWeekday(d, 1, 1, 3)) addToday("Martin Luther King Jr. Day", true);
    if (nthWeekday(d, 2, 1, 3)) addToday("Presidents' Day", true);
    if (lastWeekday(d, 5, 1)) addToday("Memorial Day", true);
    if (fixedHoliday(d, year, 6, 19)) addToday("Juneteenth", true);
    if (fixedHoliday(d, year, 7, 4)) addToday("Independence Day", true);
    if (nthWeekday(d, 9, 1, 1)) addToday("Labor Day", true);
    if (nthWeekday(d, 10, 1, 2)) addToday("Columbus Day (federal)", true);
    if (fixedHoliday(d, year, 11, 11)) addToday("Veterans Day", true);
    if (nthWeekday(d, 11, 4, 4)) addToday("Thanksgiving Day", true);
    if (fixedHoliday(d, year, 12, 25)) addToday("Christmas Day", true);
    for (size_t i = 0; i < sizeof(FUN_DAYS) / sizeof(FUN_DAYS[0]); i++) {
        if (FUN_DAYS[i].month == d.tm_mon + 1 && FUN_DAYS[i].day == d.tm_mday)
            addToday(FUN_DAYS[i].name);
    }
}

static bool isKidSafeNationalDay(const String &item) {
    String t = item;
    t.toLowerCase();
    if (!(t.startsWith("national ") || t.startsWith("world ") ||
          t.startsWith("international ") || t.startsWith("global "))) return false;
    const char *blocked[] = {"alcohol","beer","wine","vodka","gin ",
        "marijuana","cannabis","cigarette","tobacco","suicide","death",
        "violence","murder","chucky","horror","sexual","sex ","nude",
        "blood","hangover","keto","diet","weight","obesity","terminal",
        "cancer","politic","election","war ","warfare","terror",
        "gun","firearm","military","greasy foods"};
    for (auto b : blocked) if (t.indexOf(b) >= 0) return false;
    return true;
}

static void fetchOnlineNationalDays() {
#if FETCH_ONLINE_NATIONAL_DAYS
    if (WiFi.status() != WL_CONNECTED || todaysDateKey < 0) return;
    time_t epoch = time(nullptr);
    if (epoch < 1700000000) return;
    struct tm d; localtime_r(&epoch, &d);
    char target[8]; snprintf(target, sizeof(target), "%02d-%02d", d.tm_mon + 1, d.tm_mday);
    lastHolidayFetch = millis();
    todaysOnlineChecked = true;
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(9000);
    http.useHTTP10(true);
    const String endpoint = "https://adayisaholiday.com/holidays.json";
    if (!http.begin(client, endpoint)) return;
    int status = http.GET();
    if (status != 200) { http.end(); return; }
    auto *stream = http.getStreamPtr();
    if (!stream) { http.end(); return; }
    char obj[700]; size_t len = 0; int depth = 0;
    bool inString = false, escaped = false, overflow = false;
    unsigned long started = millis(), lastByte = millis();
    unsigned long processedBytes = 0;
    while (millis() - started < 20000UL && millis() - lastByte < 6000UL) {
        int incoming = stream->read();
        if (incoming < 0) {
            if (!http.connected() && !stream->available()) break;
            delay(2); continue;
        }
        lastByte = millis();
        if ((++processedBytes & 255UL) == 0) delay(1);
        char c = (char)incoming;
        if (depth == 0) {
            if (c == '{') { depth = 1; len = 0; inString = false; escaped = false; overflow = false; obj[len++] = c; }
            continue;
        }
        if (len + 1 < sizeof(obj)) obj[len++] = c;
        else overflow = true;
        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
        } else {
            if (c == '"') inString = true;
            else if (c == '{') depth++;
            else if (c == '}') depth--;
        }
        if (depth == 0) {
            if (!overflow) {
                obj[len] = '\0';
                JsonDocument doc;
                if (deserializeJson(doc, obj) == DeserializationError::Ok) {
                    String date = doc["date"].as<String>();
                    if (date.compareTo(target) > 0) break;
                    if (date == target) {
                        String cat = doc["category"].as<String>();
                        String name = doc["name"].as<String>();
                        String country = doc["country"].as<String>();
                        bool eligible = (cat == "Secular/Cultural" || cat == "UN/International");
                        eligible = eligible && (country.isEmpty() || country == "United States" || country == "USA");
                        if (eligible && isKidSafeNationalDay(name)) {
                            addToday(name);
                        }
                    }
                }
            }
            len = 0;
        }
    }
    http.end();
    if (activePage == PAGE_TODAY && currentState == STATE_DAILY_PLANNER) drawHolidayBanner();
#endif
}

static void refreshTodaysHolidays() {
    time_t epoch = time(nullptr);
    if (epoch < 1700000000) return;
    struct tm d; localtime_r(&epoch, &d);
    const int key = (d.tm_year + 1900) * 10000 + (d.tm_mon + 1) * 100 + d.tm_mday;
    if (key != todaysDateKey) {
        todaysDateKey = key;
        computeLocalSpecialDays(d);
        todaysOnlineChecked = false;
        todaysIndex = 0;
        lastHolidayRotate = millis();
        if (activePage == PAGE_TODAY && currentState == STATE_DAILY_PLANNER) drawHolidayBanner();
    }
    if (WiFi.status() == WL_CONNECTED && (!todaysOnlineChecked ||
        (todaysOnlineChecked && millis() - lastHolidayFetch > HOLIDAY_RETRY_MS))) {
        fetchOnlineNationalDays();
    }
}

static void drawHolidayBanner() {
    if (activePage != PAGE_TODAY) return;
    M5.Display.fillRect(SX(0), SY(0), SW(240), SH(36), DP_BG);
    M5.Display.fillRoundRect(SX(4), SY(3), SW(232), SH(30), 12, DP_PANEL2);
    time_t t = time(nullptr);
    if (t < 1700000000) {
        put("Checking the date...", 120, 18, 1, 1, DP_WHITE, middle_center, DP_PANEL2);
    } else {
        if (todaysCount) {
            String name = todaysDays[todaysIndex % todaysCount];
            if (name.length() > 35) name = name.substring(0, 33) + "..";
            put(name, 120, 18, 1, 1, DP_WHITE, middle_center, DP_PANEL2);
        } else {
            put("Have a great day!", 120, 18, 1, 1, DP_WHITE, middle_center, DP_PANEL2);
        }
    }
    M5.Display.fillCircle(SX(231), SY(13), 8, (WiFi.status() == WL_CONNECTED) ? DP_GREEN : DP_RED);
}

// ======================== PAGE RENDERERS =============================
static void drawHeader() {
    M5.Display.fillRect(SX(0), SY(0), SW(240), SH(70), DP_BG);
    const CurrentWeather &cur = (showTempest && tempest.valid) ? tempest : owm;
    String u = localTimeString(cur.timestamp, "%a %b %d %I:%M%p");
    put("Updated: " + u, 9, 5, 1, 1, DP_GOLD);
    M5.Display.fillCircle(SX(230), SY(10), 8, (WiFi.status() == WL_CONNECTED) ? DP_GREEN : DP_RED);
    time_t t = time(nullptr);
    String clock = localTimeString(t, "%I:%M");
    put(clock, 104, 42, 4, 2, DP_LIME, middle_center);
    put(localTimeString(t, "%p"), 208, 45, 2, 1, DP_LIME, middle_center);
    divLine(69);
}

static void drawCompass(int x, int y, int degrees) {
    int cx = SX(x), cy = SY(y);
    M5.Display.drawCircle(cx, cy, SW(12), DP_CYAN);
    M5.Display.drawFastVLine(cx, cy - SH(16), SH(3), DP_WHITE);
    float a = (degrees - 90) * 0.01745329252f;
    int px = cx + roundf(cosf(a) * SW(9)), py = cy + roundf(sinf(a) * SW(9));
    M5.Display.fillTriangle(px, py,
                            cx + roundf(cosf(a + 2.1f) * SW(6)), cy + roundf(sinf(a + 2.1f) * SW(6)),
                            cx + roundf(cosf(a - 2.1f) * SW(6)), cy + roundf(sinf(a - 2.1f) * SW(6)), DP_GREEN);
}

static void drawCurrentCard() {
    M5.Display.fillRect(SX(0), SY(70), SW(240), SH(107), DP_BG);
    const CurrentWeather &cur = (showTempest && tempest.valid) ? tempest : owm;
    const bool missing = !cur.valid;
    if (!missing) {
        int iconCode = (showTempest && owm.valid) ? owm.weatherId : cur.weatherId;
        drawIcon(iconCode, 3, 78, 88);
        String descr = (showTempest ? (tempest.valid ? "TEMPEST LIVE" : "OWM BACKUP") : cur.description);
        int labelFont = descr.length() > 10 ? 1 : 2;
        put(descr, 111, 80, labelFont, 1, DP_WHITE);
        String reading = String((int)roundf(cur.tempF)) + " F";
        put(reading, 111, 104, 4, 1, DP_LIME);
        drawCompass(113, 158, cur.windDeg);
        put(isnan(cur.windMph) ? "-- mph" : (String(cur.windMph, 0) + " mph"), 133, 150, 2, 1, DP_GOLD);
        put(isnan(cur.pressureHpa) ? "-- hPa" : (String((int)roundf(cur.pressureHpa)) + " hPa"), 133, 168, 1, 1, DP_MUTED);
    } else {
        drawIcon(803, 3, 78, 88);
        put(showTempest ? "TEMPEST" : "OPENWEATHER", 111, 84, 2, 1, DP_GOLD);
        put("Data unavailable", 111, 110, 1, 1, DP_RED);
        String e = showTempest ? tempestError : owmError;
        if (e.length() > 20) e = e.substring(0, 20);
        put(e, 111, 132, 1, 1, DP_WHITE);
    }
    divLine(178);
}

static void drawForecastPanel() {
    put("3-DAY FORECAST", 9, 181, 1, 1, DP_CYAN);
    put("HIGH / LOW F", 231, 181, 1, 1, DP_GOLD, top_right);
    for (int i = 0; i < 3; i++) {
        int x = 2 + i * 80, center = x + 40;
        const ForecastDay &f = forecast[i];
        put(f.dayName[0] ? String(f.dayName) : String("---"), center, 198, 2, 1, DP_LIME, top_center);
        if (f.valid) {
            drawIcon(f.weatherId, x + 18, 211, 44);
            put(String((int)roundf(f.highF)) + "/" + String((int)roundf(f.lowF)),
                center, 258, 2, 1, DP_WHITE, middle_center);
        } else {
            put("--/--", center, 258, 2, 1, DP_MUTED, middle_center);
        }
        if (i < 2) M5.Display.drawFastVLine(SX(x + 79), SY(197), SH(68), DP_BORDER);
    }
    divLine(270);
}

static void drawFooter() {
    M5.Display.fillRect(SX(0), SY(271), SW(240), SH(25), DP_BG);
    moonImage(109, 272);
    String rise = owm.valid ? localTimeString(owm.sunrise, "%I:%M") : "--:--";
    String set  = owm.valid ? localTimeString(owm.sunset, "%I:%M") : "--:--";
    put("Rise " + rise, 4, 273, 1, 1, DP_GOLD);
    put("Set " + set, 141, 273, 1, 1, DP_GOLD);
    const CurrentWeather &c = (showTempest && tempest.valid) ? tempest : owm;
    put(c.clouds >= 0 ? ("Cloud " + String(c.clouds) + "%") : "Cloud --%", 4, 285, 1, 1, DP_CYAN);
    put(c.humidity >= 0 ? ("Hum " + String(c.humidity) + "%") : "Hum --%", 141, 285, 1, 1, DP_CYAN);
}

static void drawNavigation() {
    const char* labels[4] = {"TODAY", "TMRW", "3-DAY", "LIVE"};
    M5.Display.fillRect(SX(0), SY(296), SW(240), SH(24), DP_BG);
    for (int i = 0; i < 4; i++) {
        const int x = 3 + i * 59;
        const bool selected = (activePage == (UIPage)i);
        const uint16_t color = selected ? DP_LIME : DP_PANEL2;
        M5.Display.fillRoundRect(SX(x), SY(297), SW(56), SH(21), 10, color);
        put(labels[i], x + 28, 307, 1, 1, selected ? DP_BG : DP_WHITE, middle_center, color);
    }
}

static void drawKidPage(bool isTomorrow) {
    M5.Display.fillScreen(DP_BG);
    drawTopHubBar();

    KidDay &kd = isTomorrow ? kidTomorrow : kidToday;
    time_t t = time(nullptr);
    time_t displayT = isTomorrow ? (t + 86400) : t;

    if (!isTomorrow) {
        drawHolidayBanner();
    } else {
        M5.Display.fillRect(SX(0), SY(0), SW(240), SH(36), DP_BG);
        M5.Display.fillRoundRect(SX(4), SY(3), SW(232), SH(30), 12, DP_PANEL2);
        String topStr = "TOMORROW, " + localTimeString(displayT, "%b %d");
        topStr.toUpperCase();
        put(topStr, 120, 18, 1, 1, DP_GOLD, middle_center, DP_PANEL2);
        M5.Display.fillCircle(SX(231), SY(13), 8, (WiFi.status() == WL_CONNECTED) ? DP_GREEN : DP_RED);
    }

    // Hero weather block
    M5.Display.fillRoundRect(SX(5), SY(39), SW(230), SH(65), 14, DP_PANEL);
    if (!isTomorrow && owm.valid) {
        drawIcon(owm.weatherId, 10, 45, 44);
        String title = owm.description;
        title.toUpperCase();
        if (title.length() > 13) title = title.substring(0, 13);
        put(title, 67, 45, 1, 1, DP_WHITE, top_left, DP_PANEL);
        put(String((int)roundf(owm.tempF)) + " F", 67, 62, 4, 1, DP_LIME, top_left, DP_PANEL);
        put("Feels " + String((int)roundf(owm.feelsF)) + " F", 68, 89, 1, 1, DP_GOLD, top_left, DP_PANEL);

        if (kd.valid && !isnan(kd.lowF)) {
            put("REST OF DAY", 230, 45, 1, 1, DP_CYAN, top_right, DP_PANEL);
            put("H " + String((int)roundf(kd.highF)) + "  L " + String((int)roundf(kd.lowF)), 230, 89, 1, 1, DP_GOLD, top_right, DP_PANEL);
        }
    } else if (isTomorrow && kd.valid) {
        drawIcon(kd.weatherId, 10, 45, 44);
        put("EXPECTED", 67, 45, 1, 1, DP_WHITE, top_left, DP_PANEL);
        put(String((int)roundf(kd.highF)) + " F", 67, 62, 4, 1, DP_LIME, top_left, DP_PANEL);
        put("Low " + String((int)roundf(kd.lowF)) + " F", 68, 89, 1, 1, DP_GOLD, top_left, DP_PANEL);
    } else {
        drawIcon(803, 10, 45, 44);
        put("Weather loading...", 67, 54, 1, 1, DP_WHITE, top_left, DP_PANEL);
        String msg = owmError;
        if (msg.length() > 26) msg = msg.substring(0, 26);
        put(msg, 67, 75, 1, 1, DP_GOLD, top_left, DP_PANEL);
    }

    // Specific hourly blocks (8 AM, NOON, 5 PM)
    put(isTomorrow ? "TOMORROW'S OUTLOOK" : "COMING UP", 9, 108, 1, 1, DP_CYAN);
    put(kd.maxRainPercent > 0 ? ("Rain " + String(kd.maxRainPercent) + "%") : "Clear skies", 231, 108, 1, 1, DP_MUTED, top_right);
    const char* hourLabels[3] = {"8 AM", "NOON", "5 PM"};
    for (int i = 0; i < 3; i++) {
        const int x = 5 + i * 79;
        M5.Display.fillRoundRect(SX(x), SY(120), SW(73), SH(53), 12, DP_PANEL);
        put(hourLabels[i], x + 36, 122, 1, 1, DP_GOLD, top_center, DP_PANEL);
        if (kd.hours[i].valid) {
            drawIcon(kd.hours[i].weatherId, x + 2, 132, 36);
            put(String((int)roundf(kd.hours[i].tempF)) + "F", x + 50, 154, 2, 1, DP_WHITE, middle_center, DP_PANEL);
        } else {
            put((WiFi.status() == WL_CONNECTED) ? "Passed" : "--", x + 36, 150, 1, 1, DP_MUTED, middle_center, DP_PANEL);
        }
    }

    // Outfit Advice
    M5.Display.fillRoundRect(SX(5), SY(177), SW(230), SH(48), 12, DP_PANEL2);
    put("WHAT SHOULD I WEAR?", 12, 179, 1, 1, DP_LIME, top_left, DP_PANEL2);
    String outfit = "Check weather first";
    String extra  = "Have fun outside!";
    if ((!isTomorrow && owm.valid) || (isTomorrow && kd.valid)) {
        float chilly = (!isTomorrow && owm.valid) ? owm.feelsF : kd.lowF;
        if (kd.valid && !isnan(kd.lowF)) chilly = min(chilly, kd.lowF);
        const float wind = max((!isTomorrow && owm.valid && !isnan(owm.windMph)) ? owm.windMph : 0.0f, kd.strongestWindMph);

        if (chilly < 32) outfit = "Warm coat + gloves";
        else if (chilly < 45) outfit = "Coat + warm hat";
        else if (chilly < 60) outfit = "Jacket + long pants";
        else if (chilly < 70) outfit = "Hoodie + sneakers";
        else outfit = "T-shirt + shorts";

        if (kd.snowPossible) extra = "Snow boots! Watch for ice.";
        else if (kd.rainPossible || kd.maxRainPercent >= 50) extra = "Take a raincoat or umbrella!";
        else if (wind >= 20) extra = "Windy! Zip up your jacket.";
        else if (chilly >= 75) extra = "Bring water + sunscreen!";
        else if (chilly < 40) extra = "Don't forget warm socks!";
        else extra = "Comfy shoes; let's explore!";
    }
    put(outfit, 13, 193, 2, 1, DP_WHITE, top_left, DP_PANEL2);
    put(extra, 13, 213, 1, 1, DP_CYAN, top_left, DP_PANEL2);

    // Google Calendar Block
    M5.Display.fillRoundRect(SX(5), SY(228), SW(230), SH(65), 12, DP_PANEL);
    String calTitle = "GOOGLE CALENDAR - " + localTimeString(displayT, "%b %d");
    put(calTitle, 12, 231, 1, 1, DP_CYAN, top_left, DP_PANEL);

    int evCount = isTomorrow ? numTomorrowEvents : numTodayEvents;
    CalEvent* evList = isTomorrow ? tomorrowEvents : todayEvents;

    if (calValid) {
        if (evCount == 0) {
            put("No events scheduled", 12, 252, 1, 1, DP_WHITE, top_left, DP_PANEL);
        } else {
            for (int i = 0; i < evCount; i++) {
                String line = evList[i].time + (evList[i].time.length() ? " - " : "") + evList[i].title;
                if (line.length() > 34) line = line.substring(0, 32) + "..";
                put(line, 12, 246 + (i * 15), 1, 1, DP_WHITE, top_left, DP_PANEL);
            }
        }
    } else {
        put("Calendar URL not set or loading...", 12, 252, 1, 1, DP_MUTED, top_left, DP_PANEL);
    }

    drawNavigation();
}

static void drawHistoryPlot(int top, const String &title, const String &unit,
                            const float *data, const float *other, uint16_t color,
                            uint16_t otherColor) {
    const int left = 37, right = 231, upper = top + 17, lower = top + 68;
    put(title, 9, top, 2, 1, DP_WHITE);
    put(unit, 229, top, 1, 1, DP_MUTED, top_right);
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < HISTORY_SLOTS; i++) {
        if (!isnan(data[i])) { lo = min(lo, data[i]); hi = max(hi, data[i]); }
        if (other && !isnan(other[i])) { lo = min(lo, other[i]); hi = max(hi, other[i]); }
    }
    if (lo > hi) {
        put("No history", 120, upper + 23, 2, 1, DP_MUTED, middle_center);
        return;
    }
    float pad = max((hi - lo) * 0.13f, (title == "PRESSURE") ? 0.4f : 1.0f);
    lo -= pad; hi += pad;
    if (title == "HUMIDITY") { lo = max(0.0f, lo); hi = min(100.0f, hi); }
    for (int k = 0; k < 3; k++) {
        int y = upper + (lower - upper) * k / 2;
        M5.Display.drawFastHLine(SX(left), SY(y), SW(right - left + 1), DP_BORDER);
    }
    for (int hour = 0; hour <= 24; hour += 6) {
        int x = left + (right - left) * hour / 24;
        M5.Display.drawFastVLine(SX(x), SY(upper), SH(lower - upper + 1), DP_PANEL2);
    }
    const bool decimal = (title == "PRESSURE");
    put(String(hi, decimal ? 1 : 0), left - 3, upper - 4, 1, 1, DP_MUTED, top_right);
    put(String(lo, decimal ? 1 : 0), left - 3, lower - 5, 1, 1, DP_MUTED, top_right);
    for (int set = 0; set < (other ? 2 : 1); set++) {
        const float *series = (set == 0) ? data : other;
        uint16_t c = (set == 0) ? color : otherColor;
        int prevX = -1, prevY = -1;
        for (int i = 0; i < HISTORY_SLOTS; i++) {
            float val = series[i];
            if (isnan(val)) { prevX = -1; continue; }
            int x = SX(left + (right - left) * i / (HISTORY_SLOTS - 1));
            int y = SY(lower - (int)roundf((val - lo) * (lower - upper) / (hi - lo)));
            y = constrain(y, SY(upper), SY(lower));
            if (prevX >= 0) {
                M5.Display.drawLine(prevX, prevY, x, y, c);
                M5.Display.drawLine(prevX, prevY + 1, x, y + 1, c);
            } else {
                M5.Display.fillCircle(x, y, 3, c);
            }
            prevX = x; prevY = y;
        }
    }
    put("-24h", left, lower + 3, 1, 1, DP_MUTED);
    put("-12h", (left + right) / 2, lower + 3, 1, 1, DP_MUTED, top_center);
    put("NOW", right, lower + 3, 1, 1, DP_MUTED, top_right);
}

static void drawTempestHistory() {
    M5.Display.fillScreen(DP_BG);
    drawTopHubBar();
    drawHeader();
    put("TEMPEST", 9, 73, 2, 1, DP_LIME);
    put("LAST 24 HOURS", 230, 77, 1, 1, DP_CYAN, top_right);
    if (!historyValid) {
        put("HISTORY", 120, 111, 4, 1, DP_WHITE, middle_center);
        String msg = historyError;
        if (msg.length() > 33) msg = msg.substring(0, 33);
        put(msg, 120, 153, 1, 1, DP_GOLD, middle_center);
        put((WiFi.status() == WL_CONNECTED) ? "Tap REFRESH above" : "Wi-Fi disconnected",
            120, 179, 1, 1, DP_MUTED, middle_center);
    } else if (historyPage == 0) {
        drawHistoryPlot(93, "TEMPERATURE", "F", historyTemp, nullptr, DP_LIME, DP_LIME);
        drawHistoryPlot(187, "WIND AVG + GUST", "mph", historyWind, historyGust, DP_CYAN, DP_GOLD);
    } else {
        drawHistoryPlot(93, "HUMIDITY", "%", historyHum, nullptr, DP_GREEN, DP_GREEN);
        drawHistoryPlot(187, "PRESSURE", "hPa", historyPressure, nullptr, DP_GOLD, DP_GOLD);
    }
    put("TAP CHARTS: " + String(historyPage == 0 ? "HUMID/PRESS" : "TEMP/WIND"),
        120, 285, 1, 1, DP_MUTED, top_center);
    drawNavigation();
}

static void drawDashboard() {
    if (activePage == PAGE_TEMPEST)  { drawTempestHistory(); return; }
    if (activePage == PAGE_TODAY)    { drawKidPage(false); return; }
    if (activePage == PAGE_TOMORROW) { drawKidPage(true); return; }

    M5.Display.fillScreen(DP_BG);
    drawTopHubBar();
    drawHeader();
    drawCurrentCard();
    drawForecastPanel();
    drawFooter();
    drawNavigation();
}

// ======================== NETWORKING & PARSERS ========================
static bool httpsGET(const String &url, String &payload, String &error, const char *source) {
    payload = "";
    if (WiFi.status() != WL_CONNECTED) {
        error = "Wi-Fi disconnected";
        return false;
    }
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(12500);
    http.useHTTP10(true);
    if (!http.begin(client, url)) {
        error = "HTTPS start failed";
        return false;
    }
    const int status = http.GET();
    if (status != HTTP_CODE_OK) {
        error = "HTTP " + String(status);
        http.end();
        return false;
    }
    payload = http.getString();
    http.end();
    if (!payload.length()) {
        error = "Empty API response";
        return false;
    }
    return true;
}

static void parseForecast(const String &payload) {
    JsonDocument filter;
    filter["list"][0]["dt"] = true;
    filter["list"][0]["main"]["temp"] = true;
    filter["list"][0]["main"]["temp_min"] = true;
    filter["list"][0]["main"]["temp_max"] = true;
    filter["list"][0]["weather"][0]["id"] = true;
    filter["list"][0]["pop"] = true;
    filter["list"][0]["wind"]["speed"] = true;
    JsonDocument doc;
    DeserializationError jsonError = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
    if (jsonError) {
        owmError = String("Forecast JSON: ") + jsonError.c_str();
        return;
    }
    JsonArray items = doc["list"].as<JsonArray>();
    if (items.isNull() || items.size() == 0) {
        owmError = "Empty forecast list";
        return;
    }

    kidToday = KidDay();
    kidTomorrow = KidDay();

    time_t now = time(nullptr);
    int tYday = -1, tmrwYday = -1;
    if (now > 1700000000) {
        struct tm l; localtime_r(&now, &l);
        tYday = l.tm_yday;
        time_t tmrw = now + 86400;
        localtime_r(&tmrw, &l);
        tmrwYday = l.tm_yday;
    }
    if (owm.valid) {
        kidToday.valid = true;
        kidToday.lowF = owm.tempF;
        kidToday.highF = owm.tempF;
        kidToday.weatherId = owm.weatherId;
        kidToday.rainPossible = (owm.weatherId >= 200 && owm.weatherId < 600);
        kidToday.snowPossible = (owm.weatherId >= 600 && owm.weatherId < 700);
        if (!isnan(owm.windMph)) kidToday.strongestWindMph = owm.windMph;
    }

    int distT[3] = {999, 999, 999};
    int distM[3] = {999, 999, 999};
    int targetH[3] = {8, 12, 17};

    int keys[3];
    time_t noon[3];
    for (int d = 0; d < 3; d++) {
        struct tm local;
        localtime_r(&now, &local);
        local.tm_mday += d + 1;
        local.tm_hour = 12; local.tm_min = 0; local.tm_sec = 0; local.tm_isdst = -1;
        noon[d] = mktime(&local);
        localtime_r(&noon[d], &local);
        keys[d] = (local.tm_year + 1900) * 400 + local.tm_yday;
        forecast[d] = ForecastDay();
        String dn = localTimeString(noon[d], "%a");
        dn.toUpperCase();
        dn.toCharArray(forecast[d].dayName, sizeof(forecast[d].dayName));
    }

    for (JsonObject row : items) {
        time_t ts = row["dt"].as<time_t>();
        struct tm l; localtime_r(&ts, &l);
        int yday = l.tm_yday;

        float temp = row["main"]["temp"] | NAN;
        float low  = row["main"]["temp_min"] | NAN;
        float high = row["main"]["temp_max"] | NAN;
        int code   = row["weather"][0]["id"] | 800;
        int chance = (int)roundf((row["pop"] | 0.0f) * 100.0f);
        float wind = row["wind"]["speed"] | 0.0f;

        KidDay *kd = nullptr;
        int *dist = nullptr;
        if (yday == tYday && ts >= now - 600) { kd = &kidToday; dist = distT; }
        else if (yday == tmrwYday) { kd = &kidTomorrow; dist = distM; }

        if (kd) {
            kd->valid = true;
            if (isnan(kd->lowF) || low < kd->lowF) kd->lowF = low;
            if (isnan(kd->highF) || high > kd->highF) kd->highF = high;
            if (chance > kd->maxRainPercent) kd->maxRainPercent = chance;
            if (wind > kd->strongestWindMph) kd->strongestWindMph = wind;
            if (code >= 600 && code < 700 && chance >= 25) kd->snowPossible = true;
            if (code >= 200 && code < 600 && chance >= 25) kd->rainPossible = true;
            if (l.tm_hour >= 11 && l.tm_hour <= 15) kd->weatherId = code;

            for (int i = 0; i < 3; i++) {
                int d = abs(l.tm_hour - targetH[i]);
                if (d <= 2 && d < dist[i]) {
                    dist[i] = d;
                    kd->hours[i].valid = true;
                    kd->hours[i].tempF = temp;
                    kd->hours[i].weatherId = code;
                }
            }
        }

        int rowKey = (l.tm_year + 1900) * 400 + l.tm_yday;
        for (int d = 0; d < 3; d++) {
            if (rowKey != keys[d] || isnan(low) || isnan(high)) continue;
            ForecastDay &f = forecast[d];
            f.valid = true;
            if (low < f.lowF) f.lowF = low;
            if (high > f.highF) f.highF = high;
            if (chance > f.popPercent) f.popPercent = chance;
            long distance = (long)labs((long)(ts - noon[d]));
            if (distance < f.bestNoonDistance) {
                f.bestNoonDistance = distance;
                f.weatherId = code;
            }
        }
    }
    hasForecast = forecast[0].valid || forecast[1].valid || forecast[2].valid;
}

static void parseCurrent(const String &payload) {
    JsonDocument filter;
    filter["dt"] = true;
    filter["main"]["temp"] = true;
    filter["main"]["feels_like"] = true;
    filter["main"]["humidity"] = true;
    filter["main"]["pressure"] = true;
    filter["wind"]["speed"] = true;
    filter["wind"]["gust"] = true;
    filter["wind"]["deg"] = true;
    filter["clouds"]["all"] = true;
    filter["weather"][0]["id"] = true;
    filter["weather"][0]["main"] = true;
    filter["sys"]["sunrise"] = true;
    filter["sys"]["sunset"] = true;
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
    if (e || doc["main"]["temp"].isNull()) return;

    CurrentWeather c;
    c.valid       = true;
    c.tempF       = doc["main"]["temp"].as<float>();
    c.feelsF      = doc["main"]["feels_like"] | c.tempF;
    c.humidity    = doc["main"]["humidity"] | -1;
    c.pressureHpa = doc["main"]["pressure"] | NAN;
    c.clouds      = doc["clouds"]["all"] | -1;
    c.windMph     = doc["wind"]["speed"] | NAN;
    c.gustMph     = doc["wind"]["gust"] | NAN;
    c.windDeg     = doc["wind"]["deg"] | 0;
    c.weatherId   = doc["weather"][0]["id"] | 800;
    c.description = doc["weather"][0]["main"].as<String>();
    c.sunrise     = doc["sys"]["sunrise"].as<time_t>();
    c.sunset      = doc["sys"]["sunset"].as<time_t>();
    c.timestamp   = doc["dt"].as<time_t>();
    if (c.timestamp < 1700000000) c.timestamp = time(nullptr);
    owm = c;
    owmError = "";
}

static void fetchOpenWeatherForecast() {
    String payload, error;
    const String args = String("?lat=") + LATITUDE + "&lon=" + LONGITUDE + "&appid=" + OPENWEATHER_KEY + "&units=imperial";
    if (httpsGET("https://api.openweathermap.org/data/2.5/weather" + args, payload, error, "OWM current")) {
        parseCurrent(payload);
    } else owmError = error;

    payload = "";
    if (httpsGET("https://api.openweathermap.org/data/2.5/forecast" + args, payload, error, "OWM forecast")) {
        parseForecast(payload);
    } else if (!owm.valid) owmError = error;
}

static void fetchCalendarData() {
    if (String(CALENDAR_SCRIPT_URL) == "YOUR_GOOGLE_APPS_SCRIPT_URL_HERE") return;
    String payload, error;
    if (httpsGET(CALENDAR_SCRIPT_URL, payload, error, "Calendar")) {
        JsonDocument doc;
        if (!deserializeJson(doc, payload)) {
            numTodayEvents = 0;
            for (JsonObject elem : doc["today"].as<JsonArray>()) {
                if (numTodayEvents < 3) {
                    todayEvents[numTodayEvents].time = elem["time"].as<String>();
                    todayEvents[numTodayEvents].title = elem["title"].as<String>();
                    numTodayEvents++;
                }
            }
            numTomorrowEvents = 0;
            for (JsonObject elem : doc["tomorrow"].as<JsonArray>()) {
                if (numTomorrowEvents < 3) {
                    tomorrowEvents[numTomorrowEvents].time = elem["time"].as<String>();
                    tomorrowEvents[numTomorrowEvents].title = elem["title"].as<String>();
                    numTomorrowEvents++;
                }
            }
            calValid = true;
        }
    }
}

static void fetchTempestData() {
    String payload, error;
    const String url = String("https://swd.weatherflow.com/swd/rest/observations/station/") + TEMPEST_STATION_ID + "?token=" + TEMPEST_TOKEN;
    if (!httpsGET(url, payload, error, "Tempest")) {
        tempestError = error;
        tempest.valid = false;
        return;
    }
    JsonDocument filter;
    filter["status"]["status_code"] = true;
    filter["obs"][0]["air_temperature"] = true;
    filter["obs"][0]["feels_like"] = true;
    filter["obs"][0]["wind_avg"] = true;
    filter["obs"][0]["wind_gust"] = true;
    filter["obs"][0]["wind_direction"] = true;
    filter["obs"][0]["relative_humidity"] = true;
    filter["obs"][0]["sea_level_pressure"] = true;
    filter["obs"][0]["timestamp"] = true;
    filter["obs"][0]["precip"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) {
        tempest.valid = false;
        return;
    }
    int apiStatus = doc["status"]["status_code"] | 0;
    JsonObject obs = doc["obs"][0].as<JsonObject>();
    if (apiStatus != 0 || obs.isNull() || obs["air_temperature"].isNull()) {
        tempest.valid = false;
        return;
    }
    CurrentWeather c;
    c.valid = true;
    c.tempF = cToF(obs["air_temperature"].as<float>());
    c.feelsF = obs["feels_like"].isNull() ? c.tempF : cToF(obs["feels_like"].as<float>());
    c.windMph = obs["wind_avg"].isNull() ? NAN : mpsToMph(obs["wind_avg"].as<float>());
    c.gustMph = obs["wind_gust"].isNull() ? NAN : mpsToMph(obs["wind_gust"].as<float>());
    c.windDeg = obs["wind_direction"] | 0;
    c.humidity = obs["relative_humidity"] | -1;
    c.pressureHpa = obs["sea_level_pressure"] | NAN;
    c.rainIn = obs["precip"].isNull() ? NAN : (obs["precip"].as<float>() / 25.4f);
    c.weatherId = owm.valid ? owm.weatherId : 803;
    c.clouds = owm.valid ? owm.clouds : -1;
    c.description = "Local station";
    c.timestamp = obs["timestamp"].as<time_t>();
    tempest = c;
    tempestError = "";
}

static void clearTempestHistory() {
    for (int i = 0; i < HISTORY_SLOTS; ++i) {
        historyTemp[i] = NAN;
        historyWind[i] = NAN;
        historyGust[i] = NAN;
        historyHum[i] = NAN;
        historyPressure[i] = NAN;
    }
    historyValid = false;
    historySampleCount = 0;
}

static float historyNumber(JsonVariantConst j) {
    if (j.isNull()) return NAN;
    if (j.is<float>() || j.is<double>() || j.is<int>() || j.is<long>()) return j.as<float>();
    return NAN;
}

static void refreshTempestHistory() {
    const time_t now = time(nullptr);
    if (now < 1700000000) {
        historyError = "Waiting for time sync";
        return;
    }
    historyAttempted = true;
    lastHistoryRequest = millis();
    historyWindowStart = now - 24L * 3600L;
    clearTempestHistory();
    historyError = "Loading 24h observations";

    String url = String("https://swd.weatherflow.com/swd/rest/observations/stn/") +
        TEMPEST_STATION_ID + "?token=" + TEMPEST_TOKEN +
        "&time_start=" + String((uint32_t)historyWindowStart) +
        "&time_end=" + String((uint32_t)now) +
        "&bucket=5&ob_fields=air_temp,wind_avg,wind_gust,rh,sea_level_pressure" +
        "&units_temp=f&units_wind=mph&units_pressure=hpa";
    String payload, error;
    if (!httpsGET(url, payload, error, "Tempest 24h")) {
        historyError = error;
        return;
    }
    JsonDocument filter;
    filter["status"]["status_code"] = true;
    filter["obs"][0] = true;
    JsonDocument doc;
    if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) return;
    if ((doc["status"]["status_code"] | 0) != 0) return;
    JsonArray observations = doc["obs"].as<JsonArray>();
    if (observations.isNull() || observations.size() == 0) return;

    uint8_t count[5][HISTORY_SLOTS] = {};
    float* values[5] = {historyTemp, historyWind, historyGust, historyHum, historyPressure};
    for (JsonVariantConst entry : observations) {
        long timestamp = 0;
        float measurement[5] = {NAN, NAN, NAN, NAN, NAN};
        if (entry.is<JsonObjectConst>()) {
            JsonObjectConst o = entry.as<JsonObjectConst>();
            timestamp = o["timestamp"] | 0L;
            measurement[0] = historyNumber(o["air_temp"]);
            if (isnan(measurement[0])) measurement[0] = historyNumber(o["air_temperature"]);
            measurement[1] = historyNumber(o["wind_avg"]);
            measurement[2] = historyNumber(o["wind_gust"]);
            measurement[3] = historyNumber(o["rh"]);
            if (isnan(measurement[3])) measurement[3] = historyNumber(o["relative_humidity"]);
            measurement[4] = historyNumber(o["sea_level_pressure"]);
        } else if (entry.is<JsonArrayConst>()) {
            JsonArrayConst row = entry.as<JsonArrayConst>();
            if (row.size() < 6) continue;
            timestamp = row[0].as<long>();
            int first = (row.size() >= 7) ? 2 : 1;
            if (row.size() >= 7) {
                float p6 = historyNumber(row[6]);
                float p5 = historyNumber(row[5]);
                if (p5 > 800 && p5 < 1150 && !(p6 > 800 && p6 < 1150)) first = 1;
            }
            for (int v = 0; v < 5; ++v) measurement[v] = historyNumber(row[first + v]);
        } else continue;

        if (timestamp < historyWindowStart || timestamp > now) continue;
        int slot = (int)(((int64_t)timestamp - historyWindowStart) * HISTORY_SLOTS / (24L * 3600L));
        if (slot < 0 || slot >= HISTORY_SLOTS) continue;
        for (int v = 0; v < 5; ++v) {
            float m = measurement[v];
            if (isnan(m) || isinf(m)) continue;
            if ((v == 0 && (m < -120 || m > 150)) ||
                ((v == 1 || v == 2) && (m < 0 || m > 250)) ||
                (v == 3 && (m < 0 || m > 100)) ||
                (v == 4 && (m < 800 || m > 1150))) continue;
            if (count[v][slot] == 0) values[v][slot] = m;
            else if (v == 2) values[v][slot] = max(values[v][slot], m);
            else values[v][slot] += m;
            if (count[v][slot] < 255) count[v][slot]++;
        }
    }
    for (int v = 0; v < 5; v++)
        for (int i = 0; i < HISTORY_SLOTS; i++)
            if (v != 2 && count[v][i] > 1) values[v][i] /= count[v][i];
    historyValid = false;
    for (int i = 0; i < HISTORY_SLOTS; i++)
        if (!isnan(historyTemp[i])) { historyValid = true; break; }
    historyError = historyValid ? "" : "No usable history values";
}

// ======================== PUBLIC INTERFACE ===========================
void initDailyPlanner() {
    if (!historyAttempted && !owm.valid) {
        clearTempestHistory();
    }
    if (WiFi.status() == WL_CONNECTED && !ntpConfigured) {
        configTzTime(TZ_RULE, ntpServer1, ntpServer2);
        ntpConfigured = true;
    }
    drawDashboard();
    if (WiFi.status() == WL_CONNECTED && lastOwmRequest == 0) {
        fetchOpenWeatherForecast();
        fetchCalendarData();
        fetchTempestData();
        refreshTodaysHolidays();
        lastOwmRequest = millis();
        lastCalendarRequest = millis();
        lastTempestRequest = millis();
        drawDashboard();
    }
}

void drawDailyPlanner() {
    drawDashboard();
}

void handleDailyPlannerTouch(int tx, int ty) {
    // 1. Top Bar: "< RUBY HUB" button
    if (ty < TOP_BAR_H) {
        if (tx >= 20 && tx <= 220) {
            playTone(440, 60);
            currentState = STATE_MAIN_MENU;
            drawMainMenu();
            return;
        }
        // "REFRESH" button
        if (tx >= sw - 160 && tx <= sw - 20) {
            playTone(660, 60);
            if (WiFi.status() != WL_CONNECTED) connectToWiFi();
            if (WiFi.status() == WL_CONNECTED) {
                if (!ntpConfigured) { configTzTime(TZ_RULE, ntpServer1, ntpServer2); ntpConfigured = true; }
                fetchOpenWeatherForecast();
                fetchCalendarData();
                fetchTempestData();
                if (activePage == PAGE_TEMPEST) refreshTempestHistory();
                refreshTodaysHolidays();
                lastOwmRequest = millis();
                lastCalendarRequest = millis();
                lastTempestRequest = millis();
            }
            drawDashboard();
            return;
        }
        return;
    }

    // Convert 720x1280 touch to virtual 240x320 coordinates
    int vx = constrain(tx / 3, 0, 239);
    int vy = constrain(((ty - TOP_BAR_H) * 10) / 37, 0, 319);

    // Bottom 4-Tab Navigation Bar (TODAY, TMRW, 3-DAY, LIVE)
    if (vy >= 292 && vy <= 319) {
        UIPage next = (UIPage)constrain(vx / 60, 0, 3);
        if (next != activePage) {
            playTone(523, 40);
            activePage = next;
            showTempest = (activePage == PAGE_TEMPEST);
            if (showTempest && !historyValid && WiFi.status() == WL_CONNECTED) {
                fetchTempestData();
                refreshTempestHistory();
            }
            drawDashboard();
        }
        return;
    }

    // Tap Holiday Banner to cycle holidays
    if (activePage == PAGE_TODAY && vy < 36 && todaysCount > 1) {
        playTone(587, 40);
        todaysIndex = (uint8_t)((todaysIndex + 1) % todaysCount);
        lastHolidayRotate = millis();
        drawHolidayBanner();
        return;
    }

    // Tap Tempest 24h Charts to toggle Temp/Wind vs Humid/Pressure
    if (activePage == PAGE_TEMPEST && vy >= 87 && vy < 292) {
        playTone(587, 40);
        historyPage = (uint8_t)(1 - historyPage);
        drawTempestHistory();
        return;
    }
}

void loopDailyPlanner() {
    if (currentState != STATE_DAILY_PLANNER) return;
    uint32_t now = millis();

    if (WiFi.status() == WL_CONNECTED && !ntpConfigured) {
        configTzTime(TZ_RULE, ntpServer1, ntpServer2);
        ntpConfigured = true;
    }

    if (now - lastHeaderDraw > 15000UL) {
        lastHeaderDraw = now;
        if (activePage != PAGE_TODAY && activePage != PAGE_TOMORROW) drawHeader();
    }

    static uint32_t lastHolidayCheck = 0;
    if (now - lastHolidayCheck >= 30000UL) {
        lastHolidayCheck = now;
        refreshTodaysHolidays();
    }

    if (activePage == PAGE_TODAY && todaysCount > 1 && now - lastHolidayRotate >= HOLIDAY_ROTATE_MS) {
        lastHolidayRotate = now;
        todaysIndex = (uint8_t)((todaysIndex + 1) % todaysCount);
        drawHolidayBanner();
    }

    if (WiFi.status() == WL_CONNECTED) {
        bool updated = false;
        if (now - lastOwmRequest >= OPENWEATHER_INTERVAL_MS || lastOwmRequest == 0) {
            fetchOpenWeatherForecast();
            lastOwmRequest = now;
            updated = true;
        }
        if (now - lastCalendarRequest >= CALENDAR_INTERVAL_MS || lastCalendarRequest == 0) {
            fetchCalendarData();
            lastCalendarRequest = now;
            updated = true;
        }
        if (showTempest) {
            if (now - lastTempestRequest >= TEMPEST_INTERVAL_MS || lastTempestRequest == 0) {
                fetchTempestData();
                lastTempestRequest = now;
                updated = true;
            }
            if ((!historyAttempted || now - lastHistoryRequest >= TEMPEST_HISTORY_MS) && time(nullptr) > 1700000000) {
                refreshTempestHistory();
                lastHistoryRequest = now;
                updated = true;
            }
        }
        if (updated) drawDashboard();
    }
}