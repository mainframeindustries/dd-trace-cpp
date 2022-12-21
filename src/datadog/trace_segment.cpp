#include "trace_segment.h"

#include <cassert>
#include <string>
#include <unordered_map>
#include <utility>

#include "collector.h"
#include "collector_response.h"
#include "dict_writer.h"
#include "error.h"
#include "hex.h"
#include "logger.h"
#include "optional.h"
#include "span_data.h"
#include "span_sampler.h"
#include "tag_propagation.h"
#include "tags.h"
#include "trace_sampler.h"
#include "w3c_propagation.h"

namespace datadog {
namespace tracing {
namespace {

// Encode the specified `trace_tags`. If the encoded value is not longer than
// the specified `tags_header_max_size`, then set it as the "x-datadog-tags"
// header using the specified `writer`. If the encoded value is oversized, then
// write a diagnostic to the specified `logger` and set a propagation error tag
// on the specified `local_root_tags`.
void inject_trace_tags(
    DictWriter& writer,
    const std::unordered_map<std::string, std::string>& trace_tags,
    std::size_t tags_header_max_size,
    std::unordered_map<std::string, std::string>& local_root_tags,
    Logger& logger) {
  const std::string encoded_trace_tags = encode_tags(trace_tags);

  if (encoded_trace_tags.size() > tags_header_max_size) {
    std::string message;
    message +=
        "Serialized x-datadog-tags header value is too large.  The configured "
        "maximum size is ";
    message += std::to_string(tags_header_max_size);
    message += " bytes, but the encoded value is ";
    message += std::to_string(encoded_trace_tags.size());
    message += " bytes.";
    logger.log_error(message);
    local_root_tags[tags::internal::propagation_error] = "inject_max_size";
  } else if (!encoded_trace_tags.empty()) {
    writer.set("x-datadog-tags", encoded_trace_tags);
  }
}

}  // namespace

TraceSegment::TraceSegment(
    const std::shared_ptr<Logger>& logger,
    const std::shared_ptr<Collector>& collector,
    const std::shared_ptr<TraceSampler>& trace_sampler,
    const std::shared_ptr<SpanSampler>& span_sampler,
    const std::shared_ptr<const SpanDefaults>& defaults,
    const std::vector<PropagationStyle>& injection_styles,
    const Optional<std::string>& hostname, Optional<std::string> origin,
    std::size_t tags_header_max_size,
    std::unordered_map<std::string, std::string> trace_tags,
    Optional<SamplingDecision> sampling_decision,
    Optional<std::string> full_w3c_trace_id_hex,
    Optional<std::string> additional_w3c_tracestate,
    Optional<std::string> additional_datadog_w3c_tracestate,
    std::unique_ptr<SpanData> local_root)
    : logger_(logger),
      collector_(collector),
      trace_sampler_(trace_sampler),
      span_sampler_(span_sampler),
      defaults_(defaults),
      injection_styles_(injection_styles),
      hostname_(hostname),
      origin_(std::move(origin)),
      tags_header_max_size_(tags_header_max_size),
      trace_tags_(std::move(trace_tags)),
      num_finished_spans_(0),
      sampling_decision_(std::move(sampling_decision)),
      full_w3c_trace_id_hex_(std::move(full_w3c_trace_id_hex)),
      additional_w3c_tracestate_(std::move(additional_w3c_tracestate)),
      additional_datadog_w3c_tracestate_(
          std::move(additional_datadog_w3c_tracestate)) {
  assert(logger_);
  assert(collector_);
  assert(trace_sampler_);
  assert(span_sampler_);
  assert(defaults_);

  register_span(std::move(local_root));
}

const SpanDefaults& TraceSegment::defaults() const { return *defaults_; }

const Optional<std::string>& TraceSegment::hostname() const {
  return hostname_;
}

const Optional<std::string>& TraceSegment::origin() const { return origin_; }

Optional<SamplingDecision> TraceSegment::sampling_decision() const {
  // `sampling_decision_` can change, so we need a lock.
  std::lock_guard<std::mutex> lock(mutex_);
  return sampling_decision_;
}

Logger& TraceSegment::logger() const { return *logger_; }

void TraceSegment::register_span(std::unique_ptr<SpanData> span) {
  std::lock_guard<std::mutex> lock(mutex_);
  assert(spans_.empty() || num_finished_spans_ < spans_.size());
  spans_.emplace_back(std::move(span));
}

void TraceSegment::span_finished() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++num_finished_spans_;
    assert(num_finished_spans_ <= spans_.size());
    if (num_finished_spans_ < spans_.size()) {
      return;
    }
  }
  // We don't need the lock anymore.  There's nobody left to call our methods.
  // On the other hand, there's nobody left to contend for the mutex, so it
  // doesn't make any difference.
  make_sampling_decision_if_null();
  assert(sampling_decision_);

  // All of our spans are finished.  Run the span sampler, finalize the spans,
  // and then send the spans to the collector.
  if (sampling_decision_->priority <= 0) {
    // Span sampling happens when the trace is dropped.
    for (const auto& span_ptr : spans_) {
      SpanData& span = *span_ptr;
      auto* rule = span_sampler_->match(span);
      if (!rule) {
        continue;
      }
      const SamplingDecision decision = rule->decide(span);
      if (decision.priority <= 0) {
        continue;
      }
      span.numeric_tags[tags::internal::span_sampling_mechanism] =
          *decision.mechanism;
      span.numeric_tags[tags::internal::span_sampling_rule_rate] =
          *decision.configured_rate;
      if (decision.limiter_max_per_second) {
        span.numeric_tags[tags::internal::span_sampling_limit] =
            *decision.limiter_max_per_second;
      }
    }
  }

  const SamplingDecision& decision = *sampling_decision_;

  auto& local_root = *spans_.front();
  local_root.tags.insert(trace_tags_.begin(), trace_tags_.end());
  local_root.numeric_tags[tags::internal::sampling_priority] =
      decision.priority;
  if (hostname_) {
    local_root.tags[tags::internal::hostname] = *hostname_;
  }
  if (decision.origin == SamplingDecision::Origin::LOCAL) {
    if (decision.mechanism == int(SamplingMechanism::AGENT_RATE) ||
        decision.mechanism == int(SamplingMechanism::DEFAULT)) {
      local_root.numeric_tags[tags::internal::agent_sample_rate] =
          *decision.configured_rate;
    } else if (decision.mechanism == int(SamplingMechanism::RULE)) {
      local_root.numeric_tags[tags::internal::rule_sample_rate] =
          *decision.configured_rate;
      if (decision.limiter_effective_rate) {
        local_root.numeric_tags[tags::internal::rule_limiter_sample_rate] =
            *decision.limiter_effective_rate;
      }
    }
  }

  // Origin is repeated on all spans.
  if (origin_) {
    for (const auto& span_ptr : spans_) {
      SpanData& span = *span_ptr;
      span.tags[tags::internal::origin] = *origin_;
    }
  }

  const auto result = collector_->send(std::move(spans_), trace_sampler_);
  if (auto* error = result.if_error()) {
    logger_->log_error(
        error->with_prefix("Error sending spans to collector: "));
  }
}

void TraceSegment::override_sampling_priority(int priority) {
  SamplingDecision decision;
  decision.priority = priority;
  decision.mechanism = int(SamplingMechanism::MANUAL);
  decision.origin = SamplingDecision::Origin::LOCAL;

  std::lock_guard<std::mutex> lock(mutex_);
  sampling_decision_ = decision;
  update_decision_maker_trace_tag();
}

void TraceSegment::make_sampling_decision_if_null() {
  // Depending on the context, `mutex_` might need already to be locked.

  if (sampling_decision_) {
    return;
  }

  const SpanData& local_root = *spans_.front();
  sampling_decision_ = trace_sampler_->decide(local_root);

  update_decision_maker_trace_tag();
}

void TraceSegment::update_decision_maker_trace_tag() {
  // Depending on the context, `mutex_` might need already to be locked.

  assert(sampling_decision_);

  if (sampling_decision_->priority <= 0) {
    trace_tags_.erase(tags::internal::decision_maker);
  } else {
    trace_tags_[tags::internal::decision_maker] =
        "-" + std::to_string(*sampling_decision_->mechanism);
  }
}

void TraceSegment::inject(DictWriter& writer, const SpanData& span) {
  // If the only injection style is `NONE`, then don't do anything.
  if (injection_styles_.size() == 1 &&
      injection_styles_[0] == PropagationStyle::NONE) {
    return;
  }

  // The sampling priority can change (it can be overridden on another thread),
  // and trace tags might change when that happens ("_dd.p.dm").
  // So, we lock here, make a sampling decision if necessary, and then copy the
  // decision and trace tags before unlocking.
  int sampling_priority;
  std::unordered_map<std::string, std::string> trace_tags;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    make_sampling_decision_if_null();
    assert(sampling_decision_);
    sampling_priority = sampling_decision_->priority;
    trace_tags = trace_tags_;
  }

  for (const auto style : injection_styles_) {
    switch (style) {
      case PropagationStyle::DATADOG:
        writer.set("x-datadog-trace-id", std::to_string(span.trace_id));
        writer.set("x-datadog-parent-id", std::to_string(span.span_id));
        writer.set("x-datadog-sampling-priority",
                   std::to_string(sampling_priority));
        if (origin_) {
          writer.set("x-datadog-origin", *origin_);
        }
        inject_trace_tags(writer, trace_tags, tags_header_max_size_,
                          spans_.front()->tags, *logger_);
        break;
      case PropagationStyle::B3:
        writer.set("x-b3-traceid", hex(span.trace_id));
        writer.set("x-b3-spanid", hex(span.span_id));
        writer.set("x-b3-sampled", std::to_string(int(sampling_priority > 0)));
        if (origin_) {
          writer.set("x-datadog-origin", *origin_);
        }
        inject_trace_tags(writer, trace_tags, tags_header_max_size_,
                          spans_.front()->tags, *logger_);
        break;
      case PropagationStyle::W3C:
        writer.set("traceparent",
                   encode_traceparent(span.trace_id, full_w3c_trace_id_hex_,
                                      span.span_id, sampling_priority));
        // TODO: handle oversized things.
        writer.set("tracestate",
                   encode_tracestate(sampling_priority, origin_, trace_tags,
                                     additional_datadog_w3c_tracestate_,
                                     additional_w3c_tracestate_));
        break;
      default:
        assert(style == PropagationStyle::NONE);
        break;
    }
  }
}

}  // namespace tracing
}  // namespace datadog
