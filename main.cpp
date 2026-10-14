#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "httplib.h"
#include "nlohmann/json.hpp"
#include <curl/curl.h>
#include <sqlite3.h>

namespace fs = std::filesystem;
using json = nlohmann::json;
using Minute = long long;

namespace {

constexpr std::size_t MAX_UPLOAD_BYTES = 25 * 1024 * 1024;
constexpr int SERVER_PORT = 8081;
constexpr const char* SERVER_HOST = "127.0.0.1";
constexpr const char* DATABASE_PATH = "ship_events.db";
constexpr const char* DEFAULT_TIKA_URL = "http://localhost:9998/tika/text";
constexpr const char* DEFAULT_GEMINI_MODEL = "gemini-3.8-flash";

std::mutex g_ingestion_mutex;

std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string normalize_name(std::string s) {
    s = lower(trim(std::move(s)));
    std::string out;
    bool previous_space = false;
    for (unsigned char c : s) {
        if (std::isspace(c)) {
            if (!previous_space) out.push_back(' ');
            previous_space = true;
        } else {
            out.push_back(static_cast<char>(c));
            previous_space = false;
        }
    }
    return out;
}

std::string html_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string current_date() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

std::string current_timestamp() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

void load_dotenv(const std::string& path = ".env") {
    std::ifstream in(path);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        if (!key.empty() && std::getenv(key.c_str()) == nullptr) {
#if defined(_WIN32)
            _putenv_s(key.c_str(), value.c_str());
#else
            setenv(key.c_str(), value.c_str(), 0);
#endif
        }
    }
}

std::string env_or(const char* key, const std::string& fallback = "") {
    const char* value = std::getenv(key);
    return value && *value ? std::string(value) : fallback;
}

std::string file_extension(const std::string& filename) {
    auto dot = filename.find_last_of('.');
    if (dot == std::string::npos) return "";
    return lower(filename.substr(dot + 1));
}

bool allowed_file(const std::string& filename) {
    const auto ext = file_extension(filename);
    return ext == "pdf" || ext == "docx";
}

std::string safe_filename(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name) {
        if (std::isalnum(c) || c == '.' || c == '-' || c == '_') out.push_back(static_cast<char>(c));
        else out.push_back('_');
    }
    if (out.empty() || out == "." || out == "..") return "uploaded_document";
    return out;
}

std::string mime_type(const std::string& filename) {
    const auto ext = file_extension(filename);
    if (ext == "pdf") return "application/pdf";
    if (ext == "docx") return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
    return "application/octet-stream";
}

// Days relative to 1970-01-01; civil calendar arithmetic, no timezone conversion.
long long days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const int shifted_month = static_cast<int>(month) + (month > 2 ? -3 : 9);
    const unsigned doy = static_cast<unsigned>((153 * shifted_month + 2) / 5) + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<long long>(era) * 146097LL + static_cast<long long>(doe) - 719468LL;
}

void civil_from_days(long long z, int& year, unsigned& month, unsigned& day) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    year = static_cast<int>(yoe) + static_cast<int>(era * 400);
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    day = doy - (153 * mp + 2) / 5 + 1;
    month = static_cast<unsigned>(static_cast<int>(mp) + (mp < 10 ? 3 : -9));
    year += month <= 2;
}

int month_number(const std::string& text) {
    const std::string m = lower(text.substr(0, std::min<std::size_t>(3, text.size())));
    static const std::map<std::string, int> months = {
        {"jan",1},{"feb",2},{"mar",3},{"apr",4},{"may",5},{"jun",6},
        {"jul",7},{"aug",8},{"sep",9},{"oct",10},{"nov",11},{"dec",12}
    };
    auto it = months.find(m);
    return it == months.end() ? 0 : it->second;
}

bool valid_date(int y, int m, int d) {
    if (y < 1900 || y > 2200 || m < 1 || m > 12 || d < 1) return false;
    static const int month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int maxd = month_days[m - 1];
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    if (m == 2 && leap) maxd = 29;
    return d <= maxd;
}

bool parse_date(const std::string& input, int& y, int& m, int& d) {
    const std::string s = trim(input);
    std::smatch match;
    static const std::regex ymd(R"(^\s*(\d{4})[-/](\d{1,2})[-/](\d{1,2})\s*$)");
    static const std::regex dmy_word(R"(^\s*(\d{1,2})[- ]([A-Za-z]{3,9})[- ](\d{4})\s*$)", std::regex::icase);
    static const std::regex dmy_num(R"(^\s*(\d{1,2})[./-](\d{1,2})[./-](\d{4})\s*$)");
    if (std::regex_match(s, match, ymd)) {
        y = std::stoi(match[1]); m = std::stoi(match[2]); d = std::stoi(match[3]);
    } else if (std::regex_match(s, match, dmy_word)) {
        d = std::stoi(match[1]); m = month_number(match[2]); y = std::stoi(match[3]);
    } else if (std::regex_match(s, match, dmy_num)) {
        d = std::stoi(match[1]); m = std::stoi(match[2]); y = std::stoi(match[3]);
    } else {
        return false;
    }
    return valid_date(y, m, d);
}

std::optional<Minute> parse_clock(const std::string& clock) {
    static const std::regex re(R"(^\s*(\d{1,2}):(\d{2})\s*$)");
    std::smatch m;
    if (!std::regex_match(clock, m, re)) return std::nullopt;
    int h = std::stoi(m[1]);
    int minute = std::stoi(m[2]);
    if (h < 0 || h > 23 || minute < 0 || minute > 59) return std::nullopt;
    return h * 60 + minute;
}

std::optional<Minute> parse_datetime(const std::string& date, const std::string& clock) {
    int y = 0, m = 0, d = 0;
    if (!parse_date(date, y, m, d)) return std::nullopt;
    auto t = parse_clock(clock);
    if (!t) return std::nullopt;
    return days_from_civil(y, static_cast<unsigned>(m), static_cast<unsigned>(d)) * 1440LL + *t;
}

std::optional<Minute> parse_date_clock(const std::string& date, const std::string& clock) {
    return parse_datetime(date, clock);
}

std::string format_minute(Minute value) {
    long long days = value / 1440;
    long long rem = value % 1440;
    if (rem < 0) { rem += 1440; --days; }
    int year = 0; unsigned month = 1, day = 1;
    civil_from_days(days, year, month, day);
    const int hour = static_cast<int>(rem / 60);
    const int minute = static_cast<int>(rem % 60);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u %02d:%02d", year, month, day, hour, minute);
    return buf;
}

std::string format_duration(Minute mins) {
    if (mins < 0) return "unknown";
    const long long hours = mins / 60;
    const long long rem = mins % 60;
    if (hours == 0) return std::to_string(rem) + "m";
    if (rem == 0) return std::to_string(hours) + "h";
    return std::to_string(hours) + "h " + std::to_string(rem) + "m";
}

struct ParsedTimestamp {
    std::size_t pos{};
    std::size_t length{};
    Minute value{};
};

// Supported row formats include: 2026-10-14 08:30, 14/10/2026 08:30,
// 14-Oct-2026 08:30 and a same-date end time such as 08:30 - 11:00.
std::vector<ParsedTimestamp> extract_timestamps(const std::string& line) {
    static const std::regex datetime_re(
        R"((\d{4}[-/]\d{1,2}[-/]\d{1,2}|\d{1,2}[./-]\d{1,2}[./-]\d{4}|\d{1,2}[- ](?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec)[- ]\d{4})[\sT,;|]+([0-2]?\d:[0-5]\d))",
        std::regex::icase);
    static const std::regex time_re(R"(([0-2]?\d):([0-5]\d))");

    std::vector<ParsedTimestamp> result;
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    std::string base_date;
    auto begin = std::sregex_iterator(line.begin(), line.end(), datetime_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        std::smatch match = *it;
        auto timestamp = parse_datetime(match[1].str(), match[2].str());
        if (!timestamp) continue;
        const auto pos = static_cast<std::size_t>(match.position());
        const auto len = static_cast<std::size_t>(match.length());
        result.push_back({pos, len, *timestamp});
        ranges.emplace_back(pos, pos + len);
        if (base_date.empty()) base_date = match[1].str();
    }

    // Some table-like SoF rows show one date and then two times, e.g. "14-Oct-2026 08:00 - 10:00 Cargo".
    if (result.size() == 1 && !base_date.empty()) {
        for (auto it = std::sregex_iterator(line.begin(), line.end(), time_re); it != end; ++it) {
            std::smatch match = *it;
            const auto pos = static_cast<std::size_t>(match.position());
            bool inside = false;
            for (const auto& r : ranges) if (pos >= r.first && pos < r.second) { inside = true; break; }
            if (inside) continue;
            const std::string time = match[1].str() + ":" + match[2].str();
            auto tm = parse_datetime(base_date, time);
            if (!tm) continue;
            Minute value = *tm;
            if (value <= result.front().value) value += 1440; // common overnight/time-only end case
            result.push_back({pos, static_cast<std::size_t>(match.length()), value});
            ranges.emplace_back(pos, pos + static_cast<std::size_t>(match.length()));
            break;
        }
    }

    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.pos < b.pos; });
    return result;
}

std::string remove_ranges(std::string input, std::vector<std::pair<std::size_t, std::size_t>> ranges) {
    std::sort(ranges.begin(), ranges.end(), [](auto a, auto b) { return a.first > b.first; });
    for (const auto& r : ranges) {
        if (r.first <= input.size() && r.second <= input.size() && r.first <= r.second)
            input.erase(r.first, r.second - r.first);
    }
    static const std::regex connector_words(R"(\b(from|to|at|between)\b)", std::regex::icase);
    static const std::regex separators(R"([|:;,]+)");
    static const std::regex dash(R"([\-–—]+)");
    static const std::regex spaces(R"(\s+)");
    input = std::regex_replace(input, connector_words, " ");
    input = std::regex_replace(input, separators, " ");
    input = std::regex_replace(input, dash, " ");
    input = std::regex_replace(input, spaces, " ");
    return trim(input);
}

std::string classify_event(const std::string& raw_name, bool has_duration) {
    const std::string name = lower(raw_name);
    auto contains = [&](const std::string& word) { return name.find(word) != std::string::npos; };

    if (has_duration && (contains("berth occupancy") || contains("berth stay") ||
                         contains("docking period") || contains("vessel at berth") ||
                         contains("vessel berth") || contains("ship berth") ||
                         contains("vessel alongside") || contains("ship alongside") ||
                         contains("vessel berthed") || contains("docked"))) return "dock_stay";

    if (contains("last line") || contains("cast off") || contains("unberthed") ||
        contains("un-berth") || contains("left berth") || contains("departed berth") ||
        contains("vessel sailed") || contains("ship sailed") || contains("all lines away") ||
        contains("all lines cast off") || contains("last line away")) return "berth_end";

    if (contains("first line ashore") || contains("all fast") || contains("berthed") ||
        contains("alongside") || contains("commenced berthing") || contains("made fast") ||
        contains("at berth") || contains("vessel berth") || contains("ship berth") || name == "berth") return "berth_start";

    if (contains("lunch") || contains("breakfast") || contains("dinner") || contains("meal")) return "meal_break";
    if (contains("rest period") || contains("rest hour") || contains("break period")) return "rest_period";
    if (contains("loading") || contains("loaded") || contains("load cargo")) return "loading";
    if (contains("unloading") || contains("discharging") || contains("discharge")) return "unloading";
    if (contains("waiting") || contains("wait for") || contains("awaiting")) return "waiting";
    if (contains("inspection") || contains("survey")) return "inspection";
    if (contains("pilot on board") || contains("pilot onboard") || contains("pilot away")) return "pilotage";
    if (contains("arrival") || contains("arrived at port") || contains("arrived port") || contains("port limits")) return "port_arrival";
    if (contains("departure") || contains("departed port") || contains("sailed from port")) return "port_departure";
    return "other";
}

struct ShipIdentity {
    std::string imo;
    std::string name;
};

ShipIdentity extract_ship_identity(const std::string& text) {
    ShipIdentity identity;
    std::smatch match;
    static const std::regex imo_re(R"(\bIMO\s*(?:number|no\.?|#)?\s*[:#\-]?\s*(\d{7})\b)", std::regex::icase);
    static const std::regex vessel_re(R"(\b(?:name\s+of\s+vessel|vessel\s+name|ship\s+name|vessel|ship)\s*[:#\-]\s*([^\r\n]{2,80}))", std::regex::icase);
    if (std::regex_search(text, match, imo_re)) identity.imo = match[1].str();
    if (std::regex_search(text, match, vessel_re)) {
        identity.name = trim(match[1].str());
        identity.name = std::regex_replace(identity.name, std::regex(R"(\s{2,}.*$)"), "");
        identity.name = trim(identity.name);
    }
    return identity;
}

struct ParsedEvent {
    std::string name;
    std::string type;
    std::optional<Minute> start;
    std::optional<Minute> end;
    std::string raw_line;
};

std::vector<ParsedEvent> parse_events(const std::string& document_text) {
    std::vector<ParsedEvent> events;
    std::istringstream input(document_text);
    std::string line;
    static const std::regex ignorable(R"(statement\s+of\s+facts|event\s+description|date\s+time|vessel\s+name|imo\s+number)", std::regex::icase);

    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty()) continue;
        if (std::regex_search(line, ignorable) && extract_timestamps(line).empty()) continue;
        const auto timestamps = extract_timestamps(line);
        if (timestamps.empty() || timestamps.size() > 2) continue; // avoid guessing ambiguous rows

        std::vector<std::pair<std::size_t, std::size_t>> ranges;
        // Ranges for the full date-time tokens.
        static const std::regex datetime_re(
            R"((\d{4}[-/]\d{1,2}[-/]\d{1,2}|\d{1,2}[./-]\d{1,2}[./-]\d{4}|\d{1,2}[- ](?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec)[- ]\d{4})[\sT,;|]+([0-2]?\d:[0-5]\d))",
            std::regex::icase);
        for (auto it = std::sregex_iterator(line.begin(), line.end(), datetime_re); it != std::sregex_iterator(); ++it) {
            ranges.emplace_back(static_cast<std::size_t>(it->position()),
                                static_cast<std::size_t>(it->position() + it->length()));
        }
        // If we detected a time-only end boundary, remove that token too.
        if (timestamps.size() == 2 && ranges.size() == 1) {
            static const std::regex time_re(R"(([0-2]?\d):([0-5]\d))");
            for (auto it = std::sregex_iterator(line.begin(), line.end(), time_re); it != std::sregex_iterator(); ++it) {
                std::size_t pos = static_cast<std::size_t>(it->position());
                if (pos >= ranges.front().first && pos < ranges.front().second) continue;
                ranges.emplace_back(pos, pos + static_cast<std::size_t>(it->length()));
                break;
            }
        }
        std::string label = remove_ranges(line, ranges);
        if (label.size() < 2) continue;
        if (timestamps.size() == 2 && timestamps[1].value <= timestamps[0].value) continue;
        const bool has_duration = timestamps.size() == 2;
        ParsedEvent event;
        event.name = label;
        event.type = classify_event(label, has_duration);
        event.start = timestamps.front().value;
        if (has_duration) event.end = timestamps[1].value;
        event.raw_line = line;
        events.push_back(std::move(event));
    }
    return events;
}

// ---------- libcurl helpers ----------
size_t curl_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t bytes = size * nmemb;
    auto* output = static_cast<std::string*>(userdata);
    output->append(ptr, bytes);
    return bytes;
}

struct HttpResult { long status = 0; std::string body; };

HttpResult http_post_json(const std::string& url, const json& payload) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("Could not initialize libcurl.");
    HttpResult result;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    const std::string body = payload.dump();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 90L);
    CURLcode code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK) throw std::runtime_error(std::string("HTTP request failed: ") + curl_easy_strerror(code));
    return result;
}

std::string extract_text_with_tika(const std::string& bytes, const std::string& filename,
                                   const std::string& tika_url) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("Could not initialize libcurl for Apache Tika.");
    HttpResult result;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Content-Type: " + mime_type(filename)).c_str());
    headers = curl_slist_append(headers, "Accept: text/plain");
    curl_easy_setopt(curl, CURLOPT_URL, tika_url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bytes.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(bytes.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 90L);
    CURLcode code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK) throw std::runtime_error(std::string("Apache Tika request failed: ") + curl_easy_strerror(code));
    if (result.status < 200 || result.status >= 300) {
        throw std::runtime_error("Apache Tika returned HTTP " + std::to_string(result.status) + ". Is Tika running at " + tika_url + "?");
    }
    if (trim(result.body).empty()) throw std::runtime_error("Apache Tika extracted no text. The PDF may be scanned; configure Tika OCR for that file.");
    return result.body;
}

std::string gemini_text(const std::string& prompt, bool json_mode = false) {
    const std::string api_key = env_or("GEMINI_API_KEY");
    if (api_key.empty()) throw std::runtime_error("GEMINI_API_KEY is missing. Set it in .env to enable AI questions.");
    const std::string model = env_or("GEMINI_MODEL", DEFAULT_GEMINI_MODEL);
    const std::string url = "https://generativelanguage.googleapis.com/v1beta/models/" + model + ":generateContent?key=" + api_key;
    json part = {{"text", prompt}};
    json content = {{"parts", json::array({part})}};
    json payload;
    payload["contents"] = json::array({content});
    if (json_mode) payload["generationConfig"] = {{"responseMimeType", "application/json"}};
    HttpResult res = http_post_json(url, payload);
    if (res.status < 200 || res.status >= 300) {
        throw std::runtime_error("Gemini API returned HTTP " + std::to_string(res.status) + ": " + res.body);
    }
    json parsed = json::parse(res.body);
    if (!parsed.contains("candidates") || parsed["candidates"].empty())
        throw std::runtime_error("Gemini returned no candidates: " + res.body);
    std::string answer;
    const auto& parts = parsed["candidates"][0]["content"]["parts"];
    for (const auto& part : parts) if (part.contains("text")) answer += part["text"].get<std::string>();
    if (trim(answer).empty()) throw std::runtime_error("Gemini returned an empty answer.");
    return answer;
}

// ---------- DSA model ----------
struct TimeInterval {
    int ship_id{};
    std::string imo;
    std::string vessel_name;
    Minute start{};
    Minute end{};
};

struct EventRow {
    int ship_id{};
    std::string imo;
    std::string vessel_name;
    std::string event_type;
    std::string event_name;
    std::optional<Minute> start;
    std::optional<Minute> end;
};

struct SweepStats {
    int peak_concurrency = 0;
    std::optional<Minute> peak_time;
    std::vector<std::pair<Minute, int>> timeline;
};

std::vector<TimeInterval> merge_intervals(std::vector<TimeInterval> intervals) {
    if (intervals.empty()) return {};
    std::sort(intervals.begin(), intervals.end(), [](const auto& a, const auto& b) {
        if (a.start != b.start) return a.start < b.start;
        return a.end < b.end;
    });
    std::vector<TimeInterval> merged;
    merged.push_back(intervals.front());
    for (std::size_t i = 1; i < intervals.size(); ++i) {
        auto& current = merged.back();
        const auto& next = intervals[i];
        if (next.start <= current.end) {
            current.end = std::max(current.end, next.end);
        } else {
            merged.push_back(next);
        }
    }
    return merged;
}

SweepStats sweep_line(const std::vector<TimeInterval>& intervals) {
    SweepStats result;
    std::vector<std::pair<Minute, int>> points;
    points.reserve(intervals.size() * 2);
    for (const auto& interval : intervals) {
        if (interval.end <= interval.start) continue;
        points.emplace_back(interval.start, +1);
        points.emplace_back(interval.end, -1);
    }
    std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first < b.first;
        return a.second < b.second; // -1 (leave) before +1 (arrive) for [start,end)
    });
    int active = 0;
    for (const auto& point : points) {
        active += point.second;
        if (active > result.peak_concurrency) {
            result.peak_concurrency = active;
            result.peak_time = point.first;
        }
        result.timeline.emplace_back(point.first, active);
    }
    return result;
}

// ---------- SQLite wrapper ----------
class Statement {
public:
    Statement(sqlite3* db, const std::string& sql) : db_(db) {
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db_));
    }
    ~Statement() { if (stmt_) sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void text(int index, const std::string& value) {
        if (sqlite3_bind_text(stmt_, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db_));
    }
    void integer(int index, long long value) {
        if (sqlite3_bind_int64(stmt_, index, static_cast<sqlite3_int64>(value)) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db_));
    }
    void null(int index) {
        if (sqlite3_bind_null(stmt_, index) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db_));
    }
    bool step() {
        const int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
    sqlite3_stmt* get() { return stmt_; }
private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

std::string column_text(sqlite3_stmt* stmt, int col) {
    const unsigned char* p = sqlite3_column_text(stmt, col);
    return p ? reinterpret_cast<const char*>(p) : "";
}

std::optional<Minute> column_optional_minute(sqlite3_stmt* stmt, int col) {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) return std::nullopt;
    return static_cast<Minute>(sqlite3_column_int64(stmt, col));
}

class Database {
public:
    explicit Database(const std::string& path) {
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
            std::string message = db_ ? sqlite3_errmsg(db_) : "sqlite3_open failed";
            if (db_) sqlite3_close(db_);
            db_ = nullptr;
            throw std::runtime_error(message);
        }
        sqlite3_busy_timeout(db_, 5000);
        exec_unlocked("PRAGMA foreign_keys = ON;");
        exec_unlocked(R"SQL(
            CREATE TABLE IF NOT EXISTS ships (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                imo TEXT UNIQUE,
                vessel_name TEXT NOT NULL,
                normalized_name TEXT NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_ships_name ON ships(normalized_name);

            CREATE TABLE IF NOT EXISTS documents (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                source_name TEXT NOT NULL,
                uploaded_at TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                ship_id INTEGER NOT NULL REFERENCES ships(id),
                document_id INTEGER NOT NULL REFERENCES documents(id),
                event_type TEXT NOT NULL,
                event_name TEXT NOT NULL,
                start_minute INTEGER,
                end_minute INTEGER,
                raw_line TEXT NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_events_ship_time ON events(ship_id, start_minute);
            CREATE INDEX IF NOT EXISTS idx_events_type_time ON events(event_type, start_minute);

            CREATE TABLE IF NOT EXISTS dock_stays (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                ship_id INTEGER NOT NULL REFERENCES ships(id),
                start_minute INTEGER NOT NULL,
                end_minute INTEGER NOT NULL,
                CHECK(end_minute > start_minute)
            );
            CREATE INDEX IF NOT EXISTS idx_dock_stays_time ON dock_stays(start_minute, end_minute);
            CREATE INDEX IF NOT EXISTS idx_dock_stays_ship ON dock_stays(ship_id, start_minute);
        )SQL");
    }

    ~Database() { if (db_) sqlite3_close(db_); }

    struct IngestResult { int ship_id = 0; int document_id = 0; int inserted = 0; int deduplicated = 0; };

    IngestResult ingest(const ShipIdentity& identity, const std::string& source_name,
                        const std::vector<ParsedEvent>& events) {
        std::lock_guard<std::mutex> guard(mutex_);
        exec_unlocked("BEGIN IMMEDIATE;");
        try {
            int ship_id = find_or_create_ship_unlocked(identity);
            Statement doc(db_, "INSERT INTO documents(source_name, uploaded_at) VALUES(?, ?)");
            doc.text(1, source_name);
            doc.text(2, current_timestamp());
            doc.step();
            const int document_id = static_cast<int>(sqlite3_last_insert_rowid(db_));
            int inserted = 0, deduplicated = 0;

            for (const auto& event : events) {
                Statement exists(db_, R"SQL(
                    SELECT id FROM events
                    WHERE ship_id = ? AND event_type = ? AND lower(event_name) = lower(?)
                      AND COALESCE(start_minute, -1) = COALESCE(?, -1)
                      AND COALESCE(end_minute, -1) = COALESCE(?, -1)
                    LIMIT 1
                )SQL");
                exists.integer(1, ship_id);
                exists.text(2, event.type);
                exists.text(3, event.name);
                if (event.start) exists.integer(4, *event.start); else exists.null(4);
                if (event.end) exists.integer(5, *event.end); else exists.null(5);
                if (exists.step()) { ++deduplicated; continue; }

                Statement insert(db_, R"SQL(
                    INSERT INTO events(ship_id, document_id, event_type, event_name,
                                       start_minute, end_minute, raw_line)
                    VALUES(?, ?, ?, ?, ?, ?, ?)
                )SQL");
                insert.integer(1, ship_id);
                insert.integer(2, document_id);
                insert.text(3, event.type);
                insert.text(4, event.name);
                if (event.start) insert.integer(5, *event.start); else insert.null(5);
                if (event.end) insert.integer(6, *event.end); else insert.null(6);
                insert.text(7, event.raw_line);
                insert.step();
                ++inserted;
            }
            exec_unlocked("COMMIT;");
            return {ship_id, document_id, inserted, deduplicated};
        } catch (...) {
            try { exec_unlocked("ROLLBACK;"); } catch (...) {}
            throw;
        }
    }

    void rebuild_dock_stays() {
        std::lock_guard<std::mutex> guard(mutex_);
        struct StoredEvent { int ship_id; std::string type; std::string name; Minute start; std::optional<Minute> end; };
        std::map<int, std::vector<StoredEvent>> by_ship;
        {
            Statement query(db_, R"SQL(
                SELECT ship_id, event_type, event_name, start_minute, end_minute
                FROM events WHERE start_minute IS NOT NULL
                ORDER BY ship_id, start_minute,
                         CASE event_type WHEN 'berth_end' THEN 0 ELSE 1 END, id
            )SQL");
            while (query.step()) {
                StoredEvent e;
                e.ship_id = sqlite3_column_int(query.get(), 0);
                e.type = column_text(query.get(), 1);
                e.name = column_text(query.get(), 2);
                e.start = static_cast<Minute>(sqlite3_column_int64(query.get(), 3));
                e.end = column_optional_minute(query.get(), 4);
                by_ship[e.ship_id].push_back(std::move(e));
            }
        }

        std::map<int, std::vector<std::pair<Minute, Minute>>> stays_by_ship;
        for (const auto& [ship_id, ship_events] : by_ship) {
            std::optional<Minute> open_start;
            for (const auto& event : ship_events) {
                if (event.type == "dock_stay" && event.end && *event.end > event.start) {
                    stays_by_ship[ship_id].emplace_back(event.start, *event.end);
                } else if (event.type == "berth_start") {
                    if (!open_start) open_start = event.start;
                } else if (event.type == "berth_end") {
                    if (open_start && event.start > *open_start) {
                        stays_by_ship[ship_id].emplace_back(*open_start, event.start);
                        open_start.reset();
                    }
                }
            }
        }

        exec_unlocked("BEGIN IMMEDIATE;");
        try {
            exec_unlocked("DELETE FROM dock_stays;");
            for (auto& [ship_id, pairs] : stays_by_ship) {
                std::vector<TimeInterval> intervals;
                for (const auto& p : pairs) intervals.push_back({ship_id, "", "", p.first, p.second});
                const auto merged = merge_intervals(std::move(intervals));
                for (const auto& interval : merged) {
                    Statement insert(db_, "INSERT INTO dock_stays(ship_id, start_minute, end_minute) VALUES(?, ?, ?)");
                    insert.integer(1, ship_id);
                    insert.integer(2, interval.start);
                    insert.integer(3, interval.end);
                    insert.step();
                }
            }
            exec_unlocked("COMMIT;");
        } catch (...) {
            try { exec_unlocked("ROLLBACK;"); } catch (...) {}
            throw;
        }
    }

    json docked_between(Minute from, Minute until) {
        std::lock_guard<std::mutex> guard(mutex_);
        json ships = json::array();
        std::set<int> seen;
        Statement query(db_, R"SQL(
            SELECT s.id, COALESCE(s.imo, ''), s.vessel_name, d.start_minute, d.end_minute
            FROM dock_stays d JOIN ships s ON s.id = d.ship_id
            WHERE d.start_minute < ? AND d.end_minute > ?
            ORDER BY s.vessel_name, d.start_minute
        )SQL");
        query.integer(1, until);
        query.integer(2, from);
        while (query.step()) {
            const int id = sqlite3_column_int(query.get(), 0);
            if (!seen.insert(id).second) continue;
            ships.push_back({{"imo", column_text(query.get(), 1)},
                             {"vessel_name", column_text(query.get(), 2)},
                             {"dock_start", format_minute(static_cast<Minute>(sqlite3_column_int64(query.get(), 3)))},
                             {"dock_end", format_minute(static_cast<Minute>(sqlite3_column_int64(query.get(), 4)))}});
        }
        return {{"intent", "ships_docked_between"}, {"window_start", format_minute(from)},
                {"window_end", format_minute(until)}, {"count", seen.size()}, {"ships", ships}};
    }

    json docking_periods(const std::string& ship_query) {
        std::lock_guard<std::mutex> guard(mutex_);
        json results = json::array();
        const std::string pattern = "%" + ship_query + "%";
        Statement query(db_, R"SQL(
            SELECT s.imo, s.vessel_name, d.start_minute, d.end_minute
            FROM dock_stays d JOIN ships s ON s.id = d.ship_id
            WHERE lower(s.vessel_name) LIKE lower(?) OR COALESCE(s.imo, '') LIKE ?
            ORDER BY s.vessel_name, d.start_minute
            LIMIT 500
        )SQL");
        query.text(1, pattern);
        query.text(2, pattern);
        while (query.step()) {
            Minute start = static_cast<Minute>(sqlite3_column_int64(query.get(), 2));
            Minute end = static_cast<Minute>(sqlite3_column_int64(query.get(), 3));
            results.push_back({{"imo", column_text(query.get(), 0)},
                               {"vessel_name", column_text(query.get(), 1)},
                               {"start_time", format_minute(start)},
                               {"end_time", format_minute(end)},
                               {"duration", format_duration(end - start)},
                               {"duration_minutes", end - start}});
        }
        return {{"intent", "docking_period_for_ship"}, {"ship_query", ship_query}, {"docking_periods", results}};
    }

    json activities(const std::string& ship_query, const std::string& keyword) {
        std::lock_guard<std::mutex> guard(mutex_);
        json results = json::array();
        const std::string ship_pattern = ship_query.empty() ? "%" : "%" + ship_query + "%";
        const std::string keyword_pattern = keyword.empty() ? "%" : "%" + keyword + "%";
        Statement query(db_, R"SQL(
            SELECT s.imo, s.vessel_name, e.event_type, e.event_name,
                   e.start_minute, e.end_minute, e.raw_line, d.source_name
            FROM events e JOIN ships s ON s.id = e.ship_id
            JOIN documents d ON d.id = e.document_id
            WHERE (lower(s.vessel_name) LIKE lower(?) OR COALESCE(s.imo, '') LIKE ?)
              AND (lower(e.event_name) LIKE lower(?) OR lower(e.event_type) LIKE lower(?))
            ORDER BY e.start_minute
            LIMIT 500
        )SQL");
        query.text(1, ship_pattern);
        query.text(2, ship_pattern);
        query.text(3, keyword_pattern);
        query.text(4, keyword_pattern);
        while (query.step()) {
            json item = {{"imo", column_text(query.get(), 0)},
                         {"vessel_name", column_text(query.get(), 1)},
                         {"event_type", column_text(query.get(), 2)},
                         {"event", column_text(query.get(), 3)},
                         {"raw_line", column_text(query.get(), 6)},
                         {"source_document", column_text(query.get(), 7)}};
            auto start = column_optional_minute(query.get(), 4);
            auto end = column_optional_minute(query.get(), 5);
            item["start_time"] = start ? json(format_minute(*start)) : json(nullptr);
            item["end_time"] = end ? json(format_minute(*end)) : json(nullptr);
            results.push_back(std::move(item));
        }
        return {{"intent", "activity_for_ship"}, {"ship_query", ship_query},
                {"activity_keyword", keyword}, {"matching_events", results}};
    }

    json port_summary() {
        std::lock_guard<std::mutex> guard(mutex_);
        json out;
        out["intent"] = "port_summary";
        out["generated_at"] = current_timestamp();
        out["total_ships"] = scalar_int_unlocked("SELECT COUNT(*) FROM ships");
        out["total_events"] = scalar_int_unlocked("SELECT COUNT(*) FROM events");
        out["total_docking_stays"] = scalar_int_unlocked("SELECT COUNT(*) FROM dock_stays");
        std::vector<TimeInterval> intervals;
        Statement query(db_, R"SQL(
            SELECT s.id, COALESCE(s.imo, ''), s.vessel_name, d.start_minute, d.end_minute
            FROM dock_stays d JOIN ships s ON s.id = d.ship_id
            ORDER BY d.start_minute
        )SQL");
        while (query.step()) {
            intervals.push_back({sqlite3_column_int(query.get(), 0), column_text(query.get(), 1),
                                 column_text(query.get(), 2),
                                 static_cast<Minute>(sqlite3_column_int64(query.get(), 3)),
                                 static_cast<Minute>(sqlite3_column_int64(query.get(), 4))});
        }
        const auto stats = sweep_line(intervals);
        out["peak_concurrent_ships"] = stats.peak_concurrency;
        out["peak_time"] = stats.peak_time ? json(format_minute(*stats.peak_time)) : json(nullptr);
        json busiest = json::array();
        if (stats.peak_time) {
            for (const auto& point : stats.timeline) {
                if (point.first == *stats.peak_time) {
                    busiest.push_back({{"time", format_minute(point.first)}, {"active_ships", point.second}});
                }
            }
        }
        out["peak_timeline_points"] = busiest;
        return out;
    }

    json general_snapshot() {
        std::lock_guard<std::mutex> guard(mutex_);
        json out;
        out["intent"] = "general_insight";
        out["total_ships"] = scalar_int_unlocked("SELECT COUNT(*) FROM ships");
        out["total_events"] = scalar_int_unlocked("SELECT COUNT(*) FROM events");
        out["total_docking_stays"] = scalar_int_unlocked("SELECT COUNT(*) FROM dock_stays");
        json recent = json::array();
        Statement query(db_, R"SQL(
            SELECT s.imo, s.vessel_name, e.event_type, e.event_name,
                   e.start_minute, e.end_minute, d.source_name
            FROM events e JOIN ships s ON s.id = e.ship_id
            JOIN documents d ON d.id = e.document_id
            ORDER BY e.start_minute DESC LIMIT 80
        )SQL");
        while (query.step()) {
            json item = {{"imo", column_text(query.get(), 0)}, {"vessel_name", column_text(query.get(), 1)},
                         {"event_type", column_text(query.get(), 2)}, {"event", column_text(query.get(), 3)},
                         {"source_document", column_text(query.get(), 6)}};
            auto start = column_optional_minute(query.get(), 4);
            auto end = column_optional_minute(query.get(), 5);
            item["start_time"] = start ? json(format_minute(*start)) : json(nullptr);
            item["end_time"] = end ? json(format_minute(*end)) : json(nullptr);
            recent.push_back(std::move(item));
        }
        out["recent_events"] = recent;
        return out;
    }

    json ingestion_summary(int ship_id) {
        std::lock_guard<std::mutex> guard(mutex_);
        json out = json::object();
        Statement ship(db_, "SELECT COALESCE(imo, ''), vessel_name FROM ships WHERE id = ?");
        ship.integer(1, ship_id);
        if (ship.step()) {
            out["imo"] = column_text(ship.get(), 0);
            out["vessel_name"] = column_text(ship.get(), 1);
        }
        out["ship_event_count"] = scalar_int_unlocked("SELECT COUNT(*) FROM events WHERE ship_id = " + std::to_string(ship_id));
        out["ship_docking_stay_count"] = scalar_int_unlocked("SELECT COUNT(*) FROM dock_stays WHERE ship_id = " + std::to_string(ship_id));
        return out;
    }

private:
    sqlite3* db_ = nullptr;
    std::mutex mutex_;

    void exec_unlocked(const std::string& sql) {
        char* error = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
            std::string message = error ? error : sqlite3_errmsg(db_);
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }

    int scalar_int_unlocked(const std::string& sql) {
        Statement query(db_, sql);
        if (!query.step()) return 0;
        return sqlite3_column_int(query.get(), 0);
    }

    int find_or_create_ship_unlocked(const ShipIdentity& identity) {
        const std::string name = trim(identity.name);
        const std::string normalized = normalize_name(name);
        if (!identity.imo.empty()) {
            int id = -1;
            {
                Statement find(db_, "SELECT id FROM ships WHERE imo = ? LIMIT 1");
                find.text(1, identity.imo);
                if (find.step()) id = sqlite3_column_int(find.get(), 0);
            }
            if (id >= 0) {
                Statement update(db_, "UPDATE ships SET vessel_name = ?, normalized_name = ? WHERE id = ?");
                update.text(1, name); update.text(2, normalized); update.integer(3, id); update.step();
                return id;
            }
            // If an earlier upload had only the vessel name, attach the newly known IMO to that row.
            {
                Statement by_name(db_, "SELECT id FROM ships WHERE normalized_name = ? AND imo IS NULL LIMIT 1");
                by_name.text(1, normalized);
                if (by_name.step()) id = sqlite3_column_int(by_name.get(), 0);
            }
            if (id >= 0) {
                Statement update(db_, "UPDATE ships SET imo = ?, vessel_name = ?, normalized_name = ? WHERE id = ?");
                update.text(1, identity.imo); update.text(2, name); update.text(3, normalized); update.integer(4, id); update.step();
                return id;
            }
        } else {
            Statement find(db_, "SELECT id FROM ships WHERE normalized_name = ? LIMIT 1");
            find.text(1, normalized);
            if (find.step()) return sqlite3_column_int(find.get(), 0);
        }
        Statement insert(db_, "INSERT INTO ships(imo, vessel_name, normalized_name) VALUES(?, ?, ?)");
        if (identity.imo.empty()) insert.null(1); else insert.text(1, identity.imo);
        insert.text(2, name);
        insert.text(3, normalized);
        insert.step();
        return static_cast<int>(sqlite3_last_insert_rowid(db_));
    }
};

json parse_intent(const std::string& question) {
    const std::string prompt =
        "Convert the user's question into query intent for a maritime port event database.\n"
        "Current local port date: " + current_date() + ".\n"
        "Return a JSON object only with exactly these fields: intent, ship_query, date, start_time, end_time, activity_keyword.\n"
        "Allowed intents: ships_docked_between, docking_period_for_ship, activity_for_ship, port_summary, general_insight.\n"
        "Rules: do not invent a ship name. For a ship count in a specified time window use ships_docked_between. For when a named ship was docked use docking_period_for_ship. For an activity (lunch, unloading, loading, waiting, etc.) use activity_for_ship and put the meaningful activity word in activity_keyword. For overall port statistics use port_summary. Otherwise use general_insight.\n"
        "Format date as YYYY-MM-DD or empty. Format time as HH:MM 24-hour or empty. If the user says today, use the current local port date. If a time window is mentioned without a date, leave date empty.\n"
        "Question: " + question;
    json intent = json::parse(gemini_text(prompt, true));
    static const std::set<std::string> allowed = {
        "ships_docked_between", "docking_period_for_ship", "activity_for_ship", "port_summary", "general_insight"
    };
    if (!intent.contains("intent") || !intent["intent"].is_string() ||
        !allowed.count(intent["intent"].get<std::string>())) {
        throw std::runtime_error("AI returned an unsupported query intent.");
    }
    for (const char* field : {"ship_query", "date", "start_time", "end_time", "activity_keyword"}) {
        if (!intent.contains(field) || !intent[field].is_string()) intent[field] = "";
    }
    if (intent["intent"] == "activity_for_ship" && intent["activity_keyword"].get<std::string>().empty()) {
        const std::string q = lower(question);
        for (const std::string& keyword : {"lunch", "breakfast", "dinner", "meal", "loading", "unloading", "waiting", "inspection", "pilot", "rest", "break"}) {
            if (q.find(keyword) != std::string::npos) {
                intent["activity_keyword"] = keyword;
                break;
            }
        }
    }
    return intent;
}

json build_query_facts(Database& db, const json& intent) {
    const std::string kind = intent.value("intent", "general_insight");
    if (kind == "ships_docked_between") {
        std::string date = intent.value("date", "");
        const std::string start = intent.value("start_time", "");
        const std::string end = intent.value("end_time", "");
        const bool date_defaulted = date.empty();
        if (date_defaulted) date = current_date();
        auto from = parse_date_clock(date, start);
        auto until = parse_date_clock(date, end);
        if (!from || !until || *until <= *from) {
            return {{"intent", kind}, {"error", "A valid date and start/end times are required. Use a window such as 2026-10-14, 10:00–11:00."}};
        }
        json facts = db.docked_between(*from, *until);
        if (date_defaulted) facts["date_assumption"] = "No date was specified; used the server's local date: " + date;
        return facts;
    }
    if (kind == "docking_period_for_ship") {
        const std::string ship = trim(intent.value("ship_query", ""));
        if (ship.empty()) return {{"intent", kind}, {"error", "Please specify a ship name or IMO number."}};
        return db.docking_periods(ship);
    }
    if (kind == "activity_for_ship") {
        const std::string ship = trim(intent.value("ship_query", ""));
        const std::string keyword = trim(intent.value("activity_keyword", ""));
        return db.activities(ship, keyword);
    }
    if (kind == "port_summary") return db.port_summary();
    return db.general_snapshot();
}

std::string explain_facts(const std::string& question, const json& facts) {
    const std::string prompt =
        "You are an assistant for maritime port operations. Answer the user's question using ONLY the verified database facts in the JSON below. "
        "Do not invent vessels, timestamps, meals, activities, counts, or durations. If matching data is absent, explicitly say it was not found in the uploaded records. "
        "Distinguish port arrival from berth docking. The database's times are local port times and are not timezone-converted. Be concise.\n\n"
        "User question: " + question + "\n\nVerified database facts:\n" + facts.dump(2);
    return gemini_text(prompt, false);
}

// ---------- HTML ----------
const std::string HTML_HEAD = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Ship Event Analytics</title>
<style>
body{font-family:Arial,sans-serif;max-width:1050px;margin:2rem auto;padding:0 1rem;color:#202733;line-height:1.45}
h1{color:#17395c}h2{margin-top:1.7rem;color:#215b49}.card{border:1px solid #d6dce3;border-radius:10px;padding:1rem;margin:1rem 0;background:#fbfcfd}
label{font-weight:600;display:block;margin:.6rem 0 .25rem}input[type=text],input[type=file],textarea{box-sizing:border-box;width:100%;padding:.65rem;border:1px solid #aeb8c5;border-radius:6px}
button{background:#17395c;color:white;border:0;border-radius:6px;padding:.7rem 1rem;margin-top:.8rem;cursor:pointer}button:hover{opacity:.9}
.muted{color:#657180;font-size:.92rem}.error{color:#a11919}.success{color:#17643f}.facts{white-space:pre-wrap;overflow-wrap:anywhere;background:#f2f5f7;padding:1rem;border-radius:7px}
</style></head><body>
)HTML";
const std::string HTML_FOOT = "</body></html>";

std::string render_page(const std::string& title, const std::string& message_html = "", const std::string& extra_html = "") {
    std::ostringstream out;
    out << HTML_HEAD << "<h1>Ship Event Analytics</h1>";
    if (!message_html.empty()) out << "<div class=\"card\">" << message_html << "</div>";
    out << R"HTML(
<div class="card"><h2>1. Upload Statement of Facts</h2>
<p class="muted">Apache Tika extracts document text. The C++ parser recognizes timestamped event rows. Optional ship fields override values detected in the document.</p>
<form method="post" action="/upload" enctype="multipart/form-data">
<label>PDF or DOCX file</label><input type="file" name="sof_document" accept=".pdf,.docx" required>
<label>Vessel name (optional)</label><input type="text" name="vessel_name" placeholder="e.g. MV Ocean Explorer">
<label>IMO number (optional)</label><input type="text" name="imo_number" placeholder="7 digits, if known">
<button type="submit">Extract and analyze events</button></form></div>
<div class="card"><h2>2. Ask about ships and port activity</h2>
<p class="muted">Examples: “How many ships were docked today between 10 AM and 11 AM?”, “When was MV Ocean Explorer docked?”, “When was lunch recorded for Ship A?”</p>
<form method="post" action="/ask"><label>Your question</label><textarea name="question" rows="3" required placeholder="Ask a question about uploaded records"></textarea><button type="submit">Analyze question</button></form></div>
)HTML";
    if (!title.empty()) out << "<p class=\"muted\">" << html_escape(title) << "</p>";
    out << extra_html << HTML_FOOT;
    return out.str();
}

std::string render_facts_result(const std::string& question, const std::string& answer, const json& facts) {
    std::ostringstream extra;
    extra << "<div class=\"card\"><h2>AI insight</h2><p>" << html_escape(answer) << "</p>"
          << "<h3>Verified records used</h3><pre class=\"facts\">" << html_escape(facts.dump(2)) << "</pre></div>";
    return render_page("Question: " + question, "", extra.str());
}

} // namespace

int main() {
    try {
        load_dotenv();
        curl_global_init(CURL_GLOBAL_DEFAULT);
        fs::create_directories("uploads");
        Database database(DATABASE_PATH);
        httplib::Server server;
        server.set_payload_max_length(MAX_UPLOAD_BYTES + 1024 * 1024);

        server.Get("/", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(render_page(""), "text/html; charset=utf-8");
        });

        server.Post("/upload", [&database](const httplib::Request& req, httplib::Response& res) {
            try {
                if (!req.has_file("sof_document")) {
                    res.status = 400;
                    res.set_content(render_page("", "<p class=\"error\">Please choose a PDF or DOCX file.</p>"), "text/html; charset=utf-8");
                    return;
                }
                const auto& file = req.get_file_value("sof_document");
                const std::string original_name = file.filename;
                if (!allowed_file(original_name)) {
                    res.status = 400;
                    res.set_content(render_page("", "<p class=\"error\">Only PDF and DOCX files are supported.</p>"), "text/html; charset=utf-8");
                    return;
                }
                if (file.content.size() > MAX_UPLOAD_BYTES) {
                    res.status = 413;
                    res.set_content(render_page("", "<p class=\"error\">The file exceeds the 25 MB limit.</p>"), "text/html; charset=utf-8");
                    return;
                }

                const std::string tika_url = env_or("TIKA_URL", DEFAULT_TIKA_URL);
                const std::string extracted_text = extract_text_with_tika(file.content, original_name, tika_url);
                ShipIdentity identity = extract_ship_identity(extracted_text);
                if (req.has_param("vessel_name") && !trim(req.get_param_value("vessel_name")).empty())
                    identity.name = trim(req.get_param_value("vessel_name"));
                if (req.has_param("imo_number")) {
                    const std::string supplied_imo = trim(req.get_param_value("imo_number"));
                    if (!supplied_imo.empty()) identity.imo = supplied_imo;
                }
                if (identity.name.empty() && !identity.imo.empty()) identity.name = "IMO " + identity.imo;
                if (identity.name.empty()) {
                    res.status = 400;
                    res.set_content(render_page("", "<p class=\"error\">Could not identify the ship. Enter the vessel name or IMO number in the form and upload again.</p>"), "text/html; charset=utf-8");
                    return;
                }
                if (!identity.imo.empty() && !std::regex_match(identity.imo, std::regex(R"(\d{7})"))) {
                    res.status = 400;
                    res.set_content(render_page("", "<p class=\"error\">IMO number must contain exactly seven digits.</p>"), "text/html; charset=utf-8");
                    return;
                }

                const std::vector<ParsedEvent> parsed_events = parse_events(extracted_text);
                if (parsed_events.empty()) {
                    res.status = 422;
                    res.set_content(render_page("", "<p class=\"error\">Tika extracted the document text, but no event rows matched the configured date/time patterns. Review README.md and adapt parse_events() to your SoF layout.</p>"), "text/html; charset=utf-8");
                    return;
                }

                Database::IngestResult ingest;
                {
                    std::lock_guard<std::mutex> lock(g_ingestion_mutex);
                    ingest = database.ingest(identity, safe_filename(original_name), parsed_events);
                    database.rebuild_dock_stays();
                }
                json summary = database.ingestion_summary(ingest.ship_id);
                std::ostringstream message;
                message << "<p class=\"success\"><strong>Upload processed.</strong></p>"
                        << "<p>File: <strong>" << html_escape(original_name) << "</strong></p>"
                        << "<p>Vessel: <strong>" << html_escape(identity.name) << "</strong>; IMO: <strong>"
                        << html_escape(identity.imo.empty() ? "not recorded" : identity.imo) << "</strong></p>"
                        << "<p>Event rows recognized: <strong>" << parsed_events.size() << "</strong>; new records stored: <strong>"
                        << ingest.inserted << "</strong>; duplicates skipped: <strong>" << ingest.deduplicated << "</strong>.</p>"
                        << "<p>Stored events for ship: <strong>" << summary.value("ship_event_count", 0) << "</strong>; reconstructed docking stays: <strong>"
                        << summary.value("ship_docking_stay_count", 0) << "</strong>.</p>"
                        << "<p class=\"muted\">Only recognized timestamped rows are included. A docking stay is reconstructed only when berth-start/end events or an explicit docking interval can be identified.</p>";
                res.set_content(render_page("Upload result", message.str()), "text/html; charset=utf-8");
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(render_page("Upload error", "<p class=\"error\">" + html_escape(e.what()) + "</p>"), "text/html; charset=utf-8");
            }
        });

        server.Post("/ask", [&database](const httplib::Request& req, httplib::Response& res) {
            try {
                const std::string question = trim(req.has_param("question") ? req.get_param_value("question") : "");
                if (question.empty()) {
                    res.status = 400;
                    res.set_content(render_page("", "<p class=\"error\">Enter a question.</p>"), "text/html; charset=utf-8");
                    return;
                }
                const json intent = parse_intent(question);
                const json facts = build_query_facts(database, intent);
                const std::string answer = explain_facts(question, facts);
                res.set_content(render_facts_result(question, answer, facts), "text/html; charset=utf-8");
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(render_page("Question error", "<p class=\"error\">" + html_escape(e.what()) + "</p>"), "text/html; charset=utf-8");
            }
        });

        std::cout << "Ship Event Analytics is running at http://" << SERVER_HOST << ":" << SERVER_PORT << "\n";
        std::cout << "Apache Tika URL: " << env_or("TIKA_URL", DEFAULT_TIKA_URL) << "\n";
        if (env_or("GEMINI_API_KEY").empty())
            std::cout << "GEMINI_API_KEY is not set; uploads work, but AI question answering will be disabled.\n";
        server.listen(SERVER_HOST, SERVER_PORT);
        curl_global_cleanup();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        curl_global_cleanup();
        return 1;
    }
}
