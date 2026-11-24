// What a call has asked the client, and what the client answered.
#pragma once

#include <cstddef>
#include <functional>
#include <mcp/context.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mcp::detail {

struct Inquiry {
  // Whether the call is served under 2026-07-28, where a question that has no
  // answer yet ends the round with input_required.
  bool modern{};
  // Whether elicitation/create takes a mode: every revision but 2025-06-18.
  bool form_mode{true};
  // What the client declared it can be asked.
  bool elicitation{};
  bool sampling{};
  bool roots{};

  std::mutex mutex;
  // How many questions without a key of their own the call has asked.
  std::size_t unnamed{};
  // Under 2026-07-28: the answers this round has, by key, as JSON text, from
  // the request's inputResponses and from its requestState.
  std::unordered_map<std::string, std::string> answers;
  // Under 2026-07-28: the questions asked without an answer, in order, each a
  // request object as JSON text.
  std::vector<std::pair<std::string, std::string>> unanswered;
  // The capabilities a question needed that the client lacks, as the JSON
  // object of `requiredCapabilities`.
  std::optional<std::string> missing;
  // Under 2026-07-28: why an answer in inputResponses is not a response to its
  // question, which makes the request invalid.
  std::optional<std::string> invalid;
  // In a handshake session: sends a request to the client and waits for the
  // result, as JSON text.
  std::function<Result<std::string>(std::string_view method, std::string_view params)>
      round_trip;
};

} // namespace mcp::detail
