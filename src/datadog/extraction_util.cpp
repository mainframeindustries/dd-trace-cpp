#include "extraction_util.h"

#include <datadog/logger.h>

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>

#include "extracted_data.h"
#include "hex.h"
#include "parse_util.h"
#include "string_util.h"
#include "tag_propagation.h"
#include "tags.h"

namespace datadog {
namespace tracing {
namespace {

// Decode the specified `trace_tags` and integrate them into the specified
// `result`. If an error occurs, add a `tags::internal::propagation_error` tag
// to the specified `span_tags` and log a diagnostic using the specified
// `logger`.
void handle_trace_tags(StringView trace_tags, ExtractedData& result,
                       std::unordered_map<std::string, std::string>& span_tags,
                       Logger& logger) {
  auto maybe_trace_tags = decode_tags(trace_tags);
  if (auto* error = maybe_trace_tags.if_error()) {
    logger.log_error(*error);
    span_tags[tags::internal::propagation_error] = "decoding_error";
    return;
  }

  for (auto& [key, value] : *maybe_trace_tags) {
    if (!starts_with(key, "_dd.p.")) {
      continue;
    }

    if (key == tags::internal::trace_id_high) {
      // _dd.p.tid contains the high 64 bits of the trace ID.
      const Optional<std::uint64_t> high = parse_trace_id_high(value);
      if (!high) {
        span_tags[tags::internal::propagation_error] = "malformed_tid " + value;
        continue;
      }

      if (result.trace_id) {
        // Note that this assumes the lower 64 bits of the trace ID have already
        // been extracted (i.e. we look for X-Datadog-Trace-ID first).
        result.trace_id->high = *high;
      }
    }

    result.trace_tags.emplace_back(std::move(key), std::move(value));
  }
}

// Extract an ID from the specified `header`, which might be present in the
// specified `headers`, and return the ID. If `header` is not present in
// `headers`, then return `nullopt`. If an error occurs, return an `Error`.
// Parse the ID with respect to the specified numeric `base`, e.g. `10` or `16`.
// The specified `header_kind` and `style_name` are used in diagnostic messages
// should an error occur.
Expected<Optional<std::uint64_t>> extract_id_header(const DictReader& headers,
                                                    StringView header,
                                                    StringView header_kind,
                                                    StringView style_name,
                                                    int base) {
  Optional<std::uint64_t> result;
  auto found = headers.lookup(header);
  if (!found) {
    return result;
  }
  auto parsed_id = parse_uint64(trim(*found), base);
  if (auto* error = parsed_id.if_error()) {
    std::string prefix;
    prefix += "Could not extract ";
    append(prefix, style_name);
    prefix += "-style ";
    append(prefix, header_kind);
    prefix += "ID from ";
    append(prefix, header);
    prefix += ": ";
    append(prefix, *found);
    prefix += ' ';
    return error->with_prefix(prefix);
  }
  result = std::move(*parsed_id);
  return result;
}

}  // namespace

Optional<std::uint64_t> parse_trace_id_high(const std::string& value) {
  if (value.size() != 16) {
    return nullopt;
  }

  auto high = parse_uint64(value, 16);
  if (high) {
    return *high;
  }

  return nullopt;
}

Expected<ExtractedData> extract_datadog(
    const DictReader& headers,
    std::unordered_map<std::string, std::string>& span_tags, Logger& logger) {
  ExtractedData result;
  result.style = PropagationStyle::DATADOG;

  auto trace_id =
      extract_id_header(headers, "x-datadog-trace-id", "trace", "Datadog", 10);
  if (auto* error = trace_id.if_error()) {
    return std::move(*error);
  }
  if (*trace_id) {
    result.trace_id = TraceID(**trace_id);
  }

  auto parent_id = extract_id_header(headers, "x-datadog-parent-id",
                                     "parent span", "Datadog", 10);
  if (auto* error = parent_id.if_error()) {
    return std::move(*error);
  }
  result.parent_id = *parent_id;

  if (auto found = headers.lookup("x-datadog-sampling-priority")) {
    auto sampling_priority = parse_int(trim(*found), 10);
    if (auto* error = sampling_priority.if_error()) {
      std::string prefix;
      prefix +=
          "Could not extract Datadog-style sampling priority from "
          "x-datadog-sampling-priority: ";
      append(prefix, *found);
      prefix += ' ';
      return error->with_prefix(prefix);
    }

    result.sampling_priority = *sampling_priority;
  }

  auto origin = headers.lookup("x-datadog-origin");
  if (origin) {
    result.origin = std::string(*origin);
  }

  auto trace_tags = headers.lookup("x-datadog-tags");
  if (trace_tags) {
    handle_trace_tags(*trace_tags, result, span_tags, logger);
  }

  return result;
}

Expected<ExtractedData> extract_b3(
    const DictReader& headers, std::unordered_map<std::string, std::string>&,
    Logger&) {
  ExtractedData result;
  result.style = PropagationStyle::B3;

  if (auto found = headers.lookup("x-b3-traceid")) {
    auto parsed = TraceID::parse_hex(trim(*found));
    if (auto* error = parsed.if_error()) {
      std::string prefix = "Could not extract B3-style trace ID from \"";
      append(prefix, *found);
      prefix += "\": ";
      return error->with_prefix(prefix);
    }
    result.trace_id = *parsed;
  }

  auto parent_id =
      extract_id_header(headers, "x-b3-spanid", "parent span", "B3", 16);
  if (auto* error = parent_id.if_error()) {
    return std::move(*error);
  }
  result.parent_id = *parent_id;

  const StringView sampling_priority_header = "x-b3-sampled";
  if (auto found = headers.lookup(sampling_priority_header)) {
    auto sampling_priority = parse_int(trim(*found), 10);
    if (auto* error = sampling_priority.if_error()) {
      std::string prefix;
      prefix += "Could not extract B3-style sampling priority from ";
      append(prefix, sampling_priority_header);
      prefix += ": ";
      append(prefix, *found);
      prefix += ' ';
      return error->with_prefix(prefix);
    }
    result.sampling_priority = *sampling_priority;
  }

  return result;
}

Expected<ExtractedData> extract_none(
    const DictReader&, std::unordered_map<std::string, std::string>&, Logger&) {
  ExtractedData result;
  result.style = PropagationStyle::NONE;
  return result;
}

std::string extraction_error_prefix(
    const Optional<PropagationStyle>& style,
    const std::vector<std::pair<std::string, std::string>>& headers_examined) {
  std::ostringstream stream;
  stream << "While extracting trace context";
  if (style) {
    stream << " in the " << to_string_view(*style) << " propagation style";
  }

  if (!headers_examined.empty()) {
    auto it = headers_examined.begin();
    stream << " from the following headers: [" << it->first << ": "
           << it->second;
    for (++it; it != headers_examined.end(); ++it) {
      stream << ", " << it->first << ": " << it->second;
    }
    stream << "]";
  }
  stream << ", an error occurred: ";
  return stream.str();
}

AuditedReader::AuditedReader(const DictReader& underlying)
    : underlying(underlying) {}

Optional<StringView> AuditedReader::lookup(StringView key) const {
  auto value = underlying.lookup(key);
  if (value) {
    entries_found.emplace_back(key, *value);
  }
  return value;
}

void AuditedReader::visit(
    const std::function<void(StringView key, StringView value)>& visitor)
    const {
  underlying.visit([&, this](StringView key, StringView value) {
    entries_found.emplace_back(key, value);
    visitor(key, value);
  });
}

ExtractedData merge(
    const PropagationStyle first_style,
    const std::unordered_map<PropagationStyle, ExtractedData>& contexts) {
  ExtractedData result;
  const auto found = contexts.find(first_style);
  if (found == contexts.end()) {
    return result;
  }

  // `found` refers to the first extracted context that yielded a trace ID.
  // This will be our main context.
  //
  // If the W3C style is present and its trace-id matches, we'll update the main
  // context with tracestate information that we want to include in `result`. We
  // may also need to use Datadog header information (only when the trace-id
  // matches).
  result = found->second;

  const auto w3c = contexts.find(PropagationStyle::W3C);
  const auto dd = contexts.find(PropagationStyle::DATADOG);

  if (w3c != contexts.end() && w3c->second.trace_id == result.trace_id) {
    result.additional_w3c_tracestate = w3c->second.additional_w3c_tracestate;
    result.additional_datadog_w3c_tracestate =
        w3c->second.additional_datadog_w3c_tracestate;
    result.headers_examined.insert(result.headers_examined.end(),
                                   w3c->second.headers_examined.begin(),
                                   w3c->second.headers_examined.end());

    if (result.parent_id != w3c->second.parent_id) {
      if (w3c->second.datadog_w3c_parent_id &&
          w3c->second.datadog_w3c_parent_id != "0000000000000000") {
        result.datadog_w3c_parent_id = w3c->second.datadog_w3c_parent_id;
      } else if (dd != contexts.end() &&
                 dd->second.trace_id == result.trace_id &&
                 dd->second.parent_id.has_value()) {
        result.datadog_w3c_parent_id = hex_padded(dd->second.parent_id.value());
      }

      result.parent_id = w3c->second.parent_id;
    }
  }

  return result;
}

}  // namespace tracing
}  // namespace datadog
