// What a client reads from a server's question, and what it writes as the
// answer: elicitation/create, sampling/createMessage and roots/list, in either
// era. The handlers themselves run in the client.
#pragma once

#include <expected>
#include <mcp/ask.hpp>
#include <mcp/channel.hpp>
#include <mcp/client.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::answer {

// Why the client does not answer a question: the JSON-RPC error a session's
// server is told, and the error a call fails with under 2026-07-28, where the
// client sends nothing more.
struct Refusal {
  int code{};
  std::string message;
  Fault fault{Fault::protocol};
  std::string reason;
};

template <typename T> using Answered = std::expected<T, Refusal>;

// A question no handler is set for, or one no server may ask.
[[nodiscard]] Refusal not_declared(std::string_view method);
// A handler that threw, with what it said.
[[nodiscard]] Refusal threw(std::string_view handler, std::string_view what);
// A sampling handler that refused, with its reason.
[[nodiscard]] Refusal refused(std::string_view reason);

// The form of an elicitation/create, from its params as the server wrote them.
[[nodiscard]] Answered<FormRequest> read_form(std::string_view params);
// The result of elicitation/create for the handler's answer to a form of this
// schema, with the schema's defaults in accepted content.
[[nodiscard]] Answered<std::string> write_form(const Answer<scrydantic::Json>& answer,
                                               std::string_view schema);
// `content`, a JSON object on one line, with each property of the form's
// schema that it leaves out and that has a `default`, at that default.
[[nodiscard]] std::string with_defaults(std::string content, std::string_view schema);

// The request of a sampling/createMessage, from its params.
[[nodiscard]] Answered<SamplingRequest> read_sampling(std::string_view params);
// The result of sampling/createMessage. Only 2025-11-25 and later take more
// than one block, which `several` says.
[[nodiscard]] Answered<std::string> write_sampling(const SamplingResult& result,
                                                   bool several);

// The result of roots/list.
[[nodiscard]] Answered<std::string> write_roots(const std::vector<Root>& roots);

} // namespace mcp::answer
