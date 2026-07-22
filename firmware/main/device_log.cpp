#include "device_log.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <vector>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

namespace {

constexpr size_t kEntryCount = 128;
constexpr size_t kTextSize = 256;
constexpr size_t kMaxRead = 64;

struct LogEntry {
  uint64_t seq;
  uint32_t ms;
  char text[kTextSize];
};

static LogEntry sEntries[kEntryCount] = {};
static size_t sHead = 0;
static size_t sCount = 0;
static uint64_t sNextSeq = 1;
static uint64_t sOverwritten = 0;
static uint64_t sTruncated = 0;
static portMUX_TYPE sMux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t sOriginalVprintf = vprintf;

static int logVprintf(const char *format, va_list args) {
  va_list uartArgs;
  va_copy(uartArgs, args);
  const int result = sOriginalVprintf ? sOriginalVprintf(format, uartArgs) : 0;
  va_end(uartArgs);

  char text[kTextSize];
  va_list copy;
  va_copy(copy, args);
  const int needed = vsnprintf(text, sizeof(text), format, copy);
  va_end(copy);
  if (needed < 0) return result;

  const bool truncated = static_cast<size_t>(needed) >= sizeof(text);
  text[sizeof(text) - 1] = '\0';

  portENTER_CRITICAL(&sMux);
  size_t slot;
  if (sCount < kEntryCount) {
    slot = (sHead + sCount) % kEntryCount;
    sCount++;
  } else {
    slot = sHead;
    sHead = (sHead + 1) % kEntryCount;
    sOverwritten++;
  }
  LogEntry &entry = sEntries[slot];
  entry.seq = sNextSeq++;
  entry.ms = esp_log_timestamp();
  memcpy(entry.text, text, sizeof(entry.text));
  if (truncated) sTruncated++;
  portEXIT_CRITICAL(&sMux);
  return result;
}

static void appendJsonString(std::string &out, const char *text) {
  out.push_back('"');
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p) {
    switch (*p) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (*p < 0x20) {
          char escaped[7];
          snprintf(escaped, sizeof(escaped), "\\u%04x", *p);
          out += escaped;
        } else {
          out.push_back(static_cast<char>(*p));
        }
    }
  }
  out.push_back('"');
}

}  // namespace

void device_log_init() {
  vprintf_like_t previous = esp_log_set_vprintf(logVprintf);
  if (previous && previous != logVprintf) sOriginalVprintf = previous;
}

std::string device_log_json(uint64_t after, size_t limit) {
  if (limit == 0) limit = 1;
  if (limit > kMaxRead) limit = kMaxRead;

  std::vector<LogEntry> entries;
  entries.reserve(limit);
  uint64_t firstSeq = 0;
  uint64_t lastSeq = 0;
  uint64_t overwritten = 0;
  uint64_t truncated = 0;

  portENTER_CRITICAL(&sMux);
  if (sCount > 0) {
    firstSeq = sEntries[sHead].seq;
    lastSeq = sEntries[(sHead + sCount - 1) % kEntryCount].seq;
    for (size_t i = 0; i < sCount && entries.size() < limit; i++) {
      const LogEntry &entry = sEntries[(sHead + i) % kEntryCount];
      if (entry.seq > after) entries.push_back(entry);
    }
  }
  overwritten = sOverwritten;
  truncated = sTruncated;
  portEXIT_CRITICAL(&sMux);

  const bool gap = after != 0 && firstSeq != 0 && after + 1 < firstSeq;
  const uint64_t nextSeq = entries.empty() ? after : entries.back().seq;
  std::string out;
  out.reserve(256 + entries.size() * 300);
  char header[256];
  snprintf(header, sizeof(header),
           "{\"ok\":true,\"firstSeq\":%llu,\"lastSeq\":%llu,\"nextSeq\":%llu,"
           "\"overwritten\":%llu,\"truncated\":%llu,\"gap\":%s,\"entries\":[",
           static_cast<unsigned long long>(firstSeq), static_cast<unsigned long long>(lastSeq),
           static_cast<unsigned long long>(nextSeq), static_cast<unsigned long long>(overwritten),
           static_cast<unsigned long long>(truncated), gap ? "true" : "false");
  out += header;
  for (size_t i = 0; i < entries.size(); i++) {
    if (i) out.push_back(',');
    char prefix[96];
    snprintf(prefix, sizeof(prefix), "{\"seq\":%llu,\"ms\":%lu,\"text\":",
             static_cast<unsigned long long>(entries[i].seq),
             static_cast<unsigned long>(entries[i].ms));
    out += prefix;
    appendJsonString(out, entries[i].text);
    out.push_back('}');
  }
  out += "]}";
  return out;
}
