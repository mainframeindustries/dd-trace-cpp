#include <datadog/dict_reader.h>
#include <datadog/dict_writer.h>
#include <datadog/expected.h>
#include <datadog/optional.h>
#include <datadog/string_view.h>

#include <initializer_list>

namespace datadog {
namespace tracing {

class Logger;
struct ExtractedData;

class Baggage {
 public:
  static Expected<Baggage> extract(const DictReader& reader);

  Baggage() = default;
  Baggage(std::unordered_map<std::string, std::string>);
  Baggage(std::initializer_list<std::pair<const std::string, std::string>>);

  bool contains(StringView key) const;
  Optional<StringView> get(StringView key) const;

  void set(std::string key, std::string value);
  void remove(StringView key);

  // visit?

  void inject(DictWriter& writer);

  inline bool operator==(const Baggage& rhs) const {
    return baggage_ == rhs.baggage_;
  }

 private:
  std::unordered_map<std::string, std::string> baggage_;
};

}  // namespace tracing
}  // namespace datadog
