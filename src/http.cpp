#include "http.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

bool http_request_complete(const std::string& buffer) {
    return buffer.find("\r\n\r\n") != std::string::npos || buffer.find("\n\n") != std::string::npos;
}

bool http_parse_request(const std::string& raw, HttpRequest& out) {
    const std::size_t line_end = raw.find("\r\n");
    const std::size_t lf_end = raw.find('\n');
    std::size_t end = line_end;
    if (end == std::string::npos) end = lf_end;
    if (end == std::string::npos || end == 0) return false;

    std::string line = raw.substr(0, end);
    std::istringstream is(line);
    std::string version;
    if (!(is >> out.method >> out.target >> version)) return false;
    if (version.rfind("HTTP/", 0) != 0) return false;
    if (out.method.empty() || out.target.empty()) return false;

    const std::size_t q = out.target.find('?');
    if (q == std::string::npos) {
        out.path = out.target;
        out.query.clear();
    } else {
        out.path = out.target.substr(0, q);
        out.query = out.target.substr(q + 1);
    }
    return true;
}

int http_query_int(const std::string& query, const std::string& key, int fallback, int min_value, int max_value) {
    if (query.empty()) return fallback;
    std::size_t pos = 0;
    while (pos <= query.size()) {
        std::size_t amp = query.find('&', pos);
        std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        std::size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            if (pair.substr(0, eq) == key) {
                const std::string value = pair.substr(eq + 1);
                char* end = nullptr;
                long parsed = std::strtol(value.c_str(), &end, 10);
                if (end == value.c_str()) return fallback;
                parsed = std::max<long>(min_value, std::min<long>(max_value, parsed));
                return static_cast<int>(parsed);
            }
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return fallback;
}

std::string http_response(int status, const std::string& content_type, const std::string& body) {
    const char* reason = "OK";
    switch (status) {
        case 200: reason = "OK"; break;
        case 400: reason = "Bad Request"; break;
        case 404: reason = "Not Found"; break;
        case 405: reason = "Method Not Allowed"; break;
        case 413: reason = "Payload Too Large"; break;
        case 500: reason = "Internal Server Error"; break;
        default: reason = "OK"; break;
    }

    std::ostringstream os;
    os << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
       << "Content-Type: " << content_type << "\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Cache-Control: no-store\r\n"
       << "Connection: close\r\n\r\n"
       << body;
    return os.str();
}

std::string http_content_type_for(const std::string& path) {
    const std::string p = lower(path);
    auto ends_with = [&p](const char* suffix) {
        const std::size_t n = std::char_traits<char>::length(suffix);
        return p.size() >= n && p.compare(p.size() - n, n, suffix) == 0;
    };
    if (ends_with(".html") || ends_with(".htm")) return "text/html; charset=utf-8";
    if (ends_with(".css")) return "text/css; charset=utf-8";
    if (ends_with(".js")) return "application/javascript; charset=utf-8";
    if (ends_with(".json")) return "application/json; charset=utf-8";
    return "text/plain; charset=utf-8";
}
