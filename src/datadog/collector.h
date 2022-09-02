#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "expected.h"

namespace datadog {
namespace tracing {

struct SpanData;
class TraceSampler;

class Collector {
 public:
  virtual Expected<void> send(
      std::vector<std::unique_ptr<SpanData>>&& spans,
      const std::shared_ptr<TraceSampler>& response_handler) = 0;

  virtual ~Collector() {}
};

}  // namespace tracing
}  // namespace datadog
