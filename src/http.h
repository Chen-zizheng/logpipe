#pragma once

#include <cstddef>
#include <string>

/**
 * 只实现这个项目需要的那一小撮 HTTP/1.1：
 * 解析请求行、取查询参数、拼响应。不做 chunked、不做 keep-alive。
 */
struct HttpRequest {
    std::string method;
    std::string target;   // 原始请求目标，例如 /api/tail?n=20
    std::string path;     // 去掉查询串的部分
    std::string query;    // ? 之后的部分
};

/** 请求头是否已经完整到达（找到 \r\n\r\n 或 \n\n）。 */
bool http_request_complete(const std::string& buffer);

/** 解析请求行；格式不合法返回 false。 */
bool http_parse_request(const std::string& raw, HttpRequest& out);

/** 取查询参数，取不到或不是数字时返回 fallback，并夹到 [min,max]。 */
int http_query_int(const std::string& query, const std::string& key, int fallback, int min_value, int max_value);

/** 拼一个 Connection: close 的响应。 */
std::string http_response(int status, const std::string& content_type, const std::string& body);

/** 简单的 MIME 判断，够 dashboard 用。 */
std::string http_content_type_for(const std::string& path);
