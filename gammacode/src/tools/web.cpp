#include "tools/registry.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

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
  auto replace_all = [&text](const std::string& from, const std::string& to) {
    if (from.empty()) return;
    std::size_t at = text.find(from);
    while (at != std::string::npos) {
      text.replace(at, from.size(), to);
      at = text.find(from, at + to.size());
    }
  };
  // Entities are built at runtime: "&"+"amp;" — so the needles survive any
  // tooling that would otherwise decode them inside this source file.
  const std::string amp = "&";
  replace_all(amp + "quot;", "\"");
  replace_all(amp + "#x27;", "'");
  replace_all(amp + "#39;", "'");
  replace_all(amp + "nbsp;", " ");
  replace_all(amp + "hellip;", "…");
  replace_all(amp + "mdash;", "—");
  replace_all(amp + "lt;", "<");
  replace_all(amp + "gt;", ">");
  replace_all(amp + "amp;", "&"); // last: < ends up as "<", never "<"
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

// ── robust multi-selector HTML parsing ─────────────────────────────────────
// DDG's HTML layout changes periodically. We try multiple selectors in order
// of specificity so one layout change doesn't break everything.
static std::vector<WebResult> extract_results(const std::string& html,
                                              const std::vector<std::string>& title_selectors,
                                              const std::vector<std::string>& url_attr = {"href"}) {
  std::vector<WebResult> results;
  // Simple token-based parser: find all <a ...>...</a> pairs with matching class
  for (const std::string& sel : title_selectors) {
    std::size_t at = 0;
    while (results.size() < 10) {
      const std::size_t anchor = html.find(sel, at);
      if (anchor == std::string::npos) break;

      // Find the enclosing <a> tag
      const std::size_t tag_open = html.rfind("<a", anchor);
      if (tag_open == std::string::npos) {
        at = anchor + sel.size();
        continue;
      }
      const std::size_t tag_close = html.find("</a>", anchor);
      if (tag_close == std::string::npos || tag_close < anchor) {
        at = anchor + sel.size();
        continue;
      }
      const std::string tag = html.substr(tag_open, tag_close - tag_open + 4);

      WebResult item;
      // Extract URL from href attribute
      for (const std::string& attr : url_attr) {
        const std::size_t href = tag.find(attr + "=\"");
        if (href != std::string::npos) {
          const std::size_t begin = href + attr.size() + 2;
          const std::size_t end = tag.find('"', begin);
          if (end != std::string::npos) {
            item.url = unwrap_ddg_link(tag.substr(begin, end - begin));
            break;
          }
        }
      }
      // Extract title text (between > and <)
      const std::size_t text_begin = tag.find('>');
      if (text_begin != std::string::npos && text_begin + 1 < tag.size()) {
        const std::size_t text_end = tag.find('<', text_begin + 1);
        if (text_end != std::string::npos) {
          item.title = strip_tags(tag.substr(text_begin + 1, text_end - text_begin - 1));
        }
      }

      // Snippet: look for a nearby snippet container
      static const std::vector<std::string> snippet_sels = {
          "result__snippet", "result__snippet__text", "web-result__snippet",
          "snippet", "result-snippet", "data-snippet"
      };
      for (const std::string& ss : snippet_sels) {
        const std::size_t snippet = html.find(ss, tag_close);
        if (snippet != std::string::npos) {
          const std::size_t s_open = html.rfind("<", snippet);
          const std::size_t s_close = html.find(">", snippet);
          if (s_open != std::string::npos && s_close != std::string::npos && s_close > snippet) {
            // Find the closing tag of the snippet container
            const std::size_t container_end = html.find("</", s_close);
            if (container_end != std::string::npos) {
              const std::string snippet_html = html.substr(s_open, container_end - s_open);
              item.snippet = collapse_space(snippet_html);
              break;
            }
          }
        }
      }

      if (!item.title.empty() && !item.url.empty()) {
        results.push_back(std::move(item));
      }
      at = tag_close + 4;
    }
    if (!results.empty()) break; // succeeded with this selector
  }
  return results;
}

std::vector<WebResult> parse_ddg_html(const std::string& html) {
  // Multiple selector sets for different DDG HTML versions
  static const std::vector<std::vector<std::string>> selector_sets = {
      // Modern DDG (2024+): class="result__a"
      {"result__a", "result__title", "result__url"},
      // Lite DDG / older: class="result__snippet" near <a class="result__snippet__text">
      {"result__snippet__text"},
      // Alternative layout: data-testid attributes
      {"web-result__title-link", "web-result__link"},
      // Very old / fallback: generic result links
      {"result-link", "result__link"},
  };

  for (const auto& selectors : selector_sets) {
    auto results = extract_results(html, selectors);
    if (!results.empty()) return results;
  }
  return {};
}

// ── curl request with retry & fallback ──────────────────────────────────────
struct FetchResult {
  bool ok = false;
  long http_code = 0;
  std::string body;
  std::string error;
};

FetchResult fetch_with_retry(const std::string& url, int max_attempts = 3) {
  std::call_once(curl_once(), [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

  for (int attempt = 1; attempt <= max_attempts; ++attempt) {
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
      return {false, 0, "", "curl init failed"};
    }

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 4L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_slist* headers = nullptr;
    headers = curl_slist_append(
        headers, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"
                 " (KHTML, like Gecko) Chrome/124.0 Safari/537.36");
    headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.9");
    headers = curl_slist_append(headers, "Accept: text/html,application/xhtml+xml");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    const CURLcode status = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (status == CURLE_OK && http_code == 200) {
      return {true, http_code, body, ""};
    }

    // Don't retry on client errors (4xx) — they won't succeed
    if (http_code >= 400 && http_code < 500) {
      return {false, http_code, body, "search engine answered HTTP " + std::to_string(http_code)};
    }

    // Transient failure — wait and retry (exponential backoff)
    if (attempt < max_attempts) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500 * attempt));
      continue;
    }

    std::string err = "request failed";
    if (status != CURLE_OK) err += " (curl: " + std::string(curl_easy_strerror(status)) + ")";
    else err += " (HTTP " + std::to_string(http_code) + ")";
    return {false, http_code, body, err};
  }
  return {false, 0, "", "max retries exceeded"};
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

    // Primary: DuckDuckGo HTML endpoint
    const std::string ddg_url = "https://html.duckduckgo.com/html/?q=" + url_encode(query);

    FetchResult result = fetch_with_retry(ddg_url, 3);

    // Fallback: DDG Lite (lighter, sometimes works when main doesn't)
    if (!result.ok) {
      const std::string lite_url = "https://lite.duckduckgo.com/lite/?q=" + url_encode(query);
      result = fetch_with_retry(lite_url, 2);
    }

    if (!result.ok) {
      return ToolResult{false, "", "web_search: " + result.error + " — try again later"};
    }

    std::vector<WebResult> results = parse_ddg_html(result.body);
    if (results.empty()) {
      // Try parsing Lite DDG format (table-based)
      if (result.body.find("table") != std::string::npos &&
          result.body.find("class=\"link\"") != std::string::npos) {
        // Lite DDG uses <table><tr><td class="link"><a href="...">Title</a></td></tr>
        results = extract_results(result.body, {"class=\"link\""});
      }
    }

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