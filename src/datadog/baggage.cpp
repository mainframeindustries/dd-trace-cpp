#include "baggage.h"

#include <sstream>

namespace datadog {
namespace tracing {

namespace {

std::string trim(const std::string& str) {
  size_t start = str.find_first_not_of(' ');
  size_t end = str.find_last_not_of(' ');
  return (start == std::string::npos || end == std::string::npos)
             ? ""
             : str.substr(start, end - start + 1);
}

Expected<std::unordered_map<std::string, std::string>> parse_baggage(
    StringView input) {
  std::unordered_map<std::string, std::string> result;
  std::stringstream ss(std::string{input});
  std::string pair;

  // Split by commas
  while (std::getline(ss, pair, ',')) {
    size_t equalPos = pair.find('=');

    // Skip if '=' is not found
    if (equalPos == std::string::npos) continue;

    // Extract key and value, then trim spaces
    std::string key = trim(pair.substr(0, equalPos));
    std::string value = trim(pair.substr(equalPos + 1));

    // Insert the key-value pair into the map
    if (!key.empty()) {
      result[key] = value;
    }
  }
  return result;
}
}  // namespace

Baggage::Baggage(
    std::initializer_list<std::pair<const std::string, std::string>> baggage)
    : baggage_(baggage) {}

Baggage::Baggage(std::unordered_map<std::string, std::string> baggage)
    : baggage_(std::move(baggage)) {}

Optional<StringView> Baggage::get(StringView key) const {
  (void)key;
  return nullopt;
}

void Baggage::set(std::string key, std::string value) {
  (void)key;
  (void)value;
}

void Baggage::remove(StringView key) { (void)key; }

bool Baggage::contains(StringView key) const {
  auto found = extracted_baggages_.kv.find(key);
  if (found != extracted_baggages_.kv.cend()) return true;

  auto found2 = baggage_.find(std::string(key));
  return found2 != baggage_.cend();
}

void Baggage::inject(DictWriter& writer) { writer.set("baggage", ""); }

Expected<Baggage> Baggage::extract(const DictReader& headers) {
  auto found = headers.lookup("baggage");
  if (!found) {
    return Error{Error::Code::MISSING_BAGGAGE_HEADER,
                 "There's no baggage context to extract"};
  }

  auto bv = parse_baggage(*found);
  if (auto error = bv.if_error()) {
    return *error;
  }

  Baggage result(*bv);
  return result;
}

}  // namespace tracing
}  // namespace datadog
