#include "tools/registry.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace gamma::tools {

namespace {

// ── shared curl plumbing (same pattern as ollama/client.cpp) ────────────────

std::once_flag& curl_once() {
  static std::once_flag flag;
  return flag;
}

std::size_t write_callback(const char* data, std::size_t size, std::size_t count, void* user) {
  auto* buffer = static_cast<std::string*>(user);
  buffer->append(data, size * count);
  return size * count;
}

std::string url_encode(const std::string& text) {
  std::string out;
  out.reserve(text.size() * 3);
  for (const unsigned char c : text) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      char hex[8];
      std::snprintf(hex, sizeof(hex), "%%%02X", c);
      out += hex;
    }
  }
  return out;
}

std::string html_unescape(std::string text) {
  auto replace_all = [&text](const char* from, const char* to) {
    std::size_t at = text.find(from);
    while (at != std::string::npos) {
      text.replace(at, std::string(from).size(), to);
      at = text.find(from, at + std::string(to).size());
    }
  };
  replace_all("&", "&");
  replace_all("<", "<");
  replace_all(">", ">");
  replace_all(""", "\"");
  replace_all("&#x27;", "'");
  replace_all("&#39;", "'");
  replace_all("&nbsp;", " ");
  replace_all("&hellip;", "…");
  replace_all("&mdash;", "—");
  // numeric references: &#8594; and &#x2192;
  for (std::size_t at = text.find("&#"); at != std::string::npos;
       at = text.find("&#", at + 1)) {
    const std::size_t semi = text.find(';', at);
    if (semi == std::string::npos || semi > at + 8) continue;
    long code = -1;
    try {
      if (text.size() > at + 3 && (text[at + 2] == 'x' || text[at + 2] == 'X'))
        code = std::stol(text.substr(at + 3, semi - at - 3), nullptr, 16);
      else
        code = std::stol(text.substr(at + 2, semi - at - 2), nullptr, 10);
    } catch (const std::exception&) {
      continue;
    }
    if (code > 0 && code < 0x110000) {
      // encode the code point as UTF-8
      std::string utf8;
      if (code < 0x80) {
        utf8 += static_cast<char>(code);
      } else if (code < 0x800) {
        utf8 += static_cast<char>(0xC0 | (code >> 6));
        utf8 += static_cast<char>(0x80 | (code & 0x3F));
      } else if (code < 0x10000) {
        utf8 += static_cast<char>(0xE0 | (code >> 12));
        utf8 += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        utf8 += static_cast<char>(0x80 | (code & 0x3F));
      } else {
        utf8 += static_cast<char>(0xF0 | (code >> 18));
        utf8 += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        utf8 += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        utf8 += static_cast<char>(0x80 | (code & 0x3F));
      }
      text.replace(at, semi - at + 1, utf8);
    }
  }
  return text;
}

std::string strip_tags(std::string text) {
  std::string out;
  out.reserve(text.size());
  bool inside = false;
  for (const char c : text) {
    if (c == '<') {
      inside = true;
      continue;
    }
    if (c == '>') {
      inside = false;
      continue;
    }
    if (!inside) out += c;
  }
  return html_unescape(out);
}

// Squashes runs of whitespace so snippets survive minified HTML.
std::string collapse_space(std::string text) {
  text = strip_tags(std::move(text));
  std::string out;
  out.reserve(text.size());
  bool space = false;
  for (const unsigned char c : text) {
    if (std::isspace(c)) {
      space = true;
      continue;
    }
    if (space && !out.empty()) out += ' ';
    space = false;
    out += static_cast<char>(c);
  }
  return out;
}

// DDG wraps result links in a redirect (/l/?uddg=<encoded-url>&rut=...);
// unwrap to the real target.
std::string unwrap_ddg_link(std::string url) {
  const std::size_t marker = url.find("uddg=");
  if (marker != std::string::npos) {
    std::string encoded = url.substr(marker + 5);
    const std::size_t end = encoded.find('&');
    if (end != std::string::npos) encoded.resize(end);
    std::string out;
    for (std::size_t i = 0; i < encoded.size(); ++i) {
      if (encoded[i] == '%' && i + 2 < encoded.size() &&
          std::isxdigit(static_cast<unsigned char>(encoded[i + 1])) &&
          std::isxdigit(static_cast<unsigned char>(encoded[i + 2]))) {
        const char hex[] = {encoded[i + 1], encoded[i + 2], '\0'};
        out += static_cast<char>(std::strtol(hex, nullptr, 16));
        i += 2;
      } else {
        out += encoded[i];
      }
    }
    return out;
  }
  if (url.starts_with("//")) return "https:" + url;
  return url;
}

struct WebResult {
  std::string title;
  std::string url;
  std::string snippet;
};

// Pulls every <a ... href="URL" ...>TITLE</a> whose tag carries `klass`, and
// the snippet block that follows it. Deliberately crude: no HTML library, and
// the page is machine-generated.
std::vector<WebResult> parse_ddg_html(const std::string& html) {
  std::vector<WebResult> results;
  std::size_t at = 0;
  while (results.size() < 10) {
    const std::size_t anchor = html.find("result__a", at);
    if (anchor == std::string::npos) break;
    const std::size_t tag_open = html.rfind("<a", anchor);
    const std::size_t tag_close = html.find("</a>", anchor);
    if (tag_open == std::string::npos || tag_close == std::string::npos ||
        tag_close < anchor) {
      at = anchor + 9;
      continue;
    }
    const std::string tag = html.substr(tag_open, tag_close - tag_open);

    WebResult item;
    const std::size_t href = tag.find("href=\"");
    if (href != std::string::npos) {
      const std::size_t begin = href + 6;
      const std::size_t end = tag.find('"', begin);
      if (end != std::string::npos) item.url = unwrap_ddg_link(tag.substr(begin, end - begin));
    }
    const std::size_t text_begin = tag.find('>') + 1;
    if (text_begin < tag.size()) item.title = strip_tags(tag.substr(text_begin));

    // snippet: the next result__snippet block after this anchor
    const std::size_t snippet = html.find("result__snippet", tag_close);
    if (snippet != std::string::npos) {
      const std::size_t s_tag = html.rfind("<a", snippet);
      const std::size_t s_close = html.find("</a>", snippet);
      if (s_tag != std::string::npos && s_close != std::string::npos && s_close > snippet) {
        const std::string s_tag_text = html.substr(s_tag, s_close - s_tag);
        const std::size_t s_text = s_tag_text.find('>') + 1;
        if (s_text < s_tag_text.size())
          item.snippet = collapse_space(s_tag_text.substr(s_text));
      }
      at = s_close;
    } else {
      at = tag_close;
    }
    if (!item.title.empty() && !item.url.empty()) results.push_back(std::move(item));
  }
  return results;
}

} // namespace

// web_search — the ONLY outbound call in gamma besides the first-run Ollama
// installer download. Sends the query to DuckDuckGo's HTML endpoint (no API
// key, no tracking params) and returns numbered results. GAMMA_NO_WEB=1
// removes the tool entirely at registration time for strict local-only runs.
Tool make_web_search_tool() {
  Tool tool;
  tool.name = "web_search";
  tool.description =
      "Search the public web (DuckDuckGo, no API key) and get numbered results with title, URL and snippet. Use for documentation, error messages, library versions. This sends the query to the search engine — never include code or secrets in it.";
  tool.schema = nlohmann::json{
      {"type", "object"},
      {"properties",
       {
           {"query",
            {
                {"type", "string"},
                {"description", "Search query (plain keywords, no code or secrets)"},
            }},
           {"max_results",
            {
                {"type", "integer"},
                {"description", "How many results to return, 1-10 (default 5)"},
            }},
       }},
      {"required", nlohmann::json::array({"query"})},
  };
  tool.readonly = true;

  tool.run = [](const nlohmann::json& arguments) -> ToolResult {
    if (!arguments.contains("query") || !arguments["query"].is_string() ||
        arguments["query"].get<std::string>().empty()) {
      return {false, "", "web_search requires a non-empty 'query' string"};
    }
    const std::string query = arguments["query"].get<std::string>();
    long max_results = 5;
    if (arguments.contains("max_results") && arguments["max_results"].is_number_integer())
      max_results = std::clamp<long>(arguments["max_results"].get<long>(), 1, 10);

    std::call_once(curl_once(), [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
      return ToolResult{false, "", "web_search: curl init failed"};
    }
    std::string body;
    const std::string url = "https://html.duckduckgo.com/html/?q=" + url_encode(query);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 4L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_slist* headers = nullptr;
    headers = curl_slist_append(
        headers, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"
                 " (KHTML, like Gecko) Chrome/124.0 Safari/537.36");
    headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.9");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    const CURLcode status = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (status != CURLE_OK) {
      return ToolResult{false, "", std::string("web_search: request failed (curl: ") +
                                     curl_easy_strerror(status) + ")"};
    }
    if (http_code != 200) {
      return ToolResult{false, "", "web_search: search engine answered HTTP " +
                                      std::to_string(http_code) + " — try again later"};
    }

    std::vector<WebResult> results = parse_ddg_html(body);
    if (results.empty()) {
      return ToolResult{true, "no results for '" + query + "'", ""};
    }
    if (static_cast<long>(results.size()) > max_results) results.resize(max_results);

    std::string output;
    for (std::size_t i = 0; i < results.size(); ++i) {
      const WebResult& item = results[i];
      output += std::to_string(i + 1) + ". " + item.title + "\n   " + item.url + "\n";
      if (!item.snippet.empty()) {
        std::string snippet = item.snippet.substr(0, 300);
        if (item.snippet.size() > 300) snippet += "…";
        output += "   " + snippet + "\n";
      }
      output += "\n";
    }
    return ToolResult{true, output, ""};
  };

  return tool;
}

} // namespace gamma::tools
