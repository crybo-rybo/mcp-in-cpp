#include "inquiry.hpp"

#include <mcp/ask.hpp>
#include <mcp/context.hpp>
#include <mutex>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp {

namespace {

[[nodiscard]] scrydantic::Error failure(std::string text) {
  return scrydantic::tool_error(std::move(text));
}

struct[[ = scrydantic::ignore_unknown, = scrydantic::skip_null ]] SampledMessage {
  Role role{Role::assistant};
  scrydantic::Json content;
  std::string model;
  [[= scrydantic::name{"stopReason"}]] std::optional<std::string> stop_reason;
};

struct[[= scrydantic::ignore_unknown]] RootList {
  std::vector<Root> roots;
};

// Marks the request invalid when a client's answer is not a response of the
// kind asked, under 2026-07-28, where the answer came with the request.
void reject_answer(const Context& context, const std::string_view what,
                   const scrydantic::Error& error) {
  if (detail::Inquiry* const inquiry = context.inquiry;
      inquiry != nullptr && inquiry->modern) {
    const std::scoped_lock lock{inquiry->mutex};
    inquiry->invalid = std::string{what} + " is not valid: " + error.message;
  }
}

} // namespace

namespace detail {

bool can_ask(const Context& context, const Need need) noexcept {
  const Inquiry* const inquiry = context.inquiry;
  if (inquiry == nullptr) {
    return false;
  }
  switch (need) {
  case Need::elicitation:
    return inquiry->elicitation;
  case Need::sampling:
    return inquiry->sampling;
  case Need::roots:
    return inquiry->roots;
  }
  return false;
}

Result<scrydantic::Json> ask(const Context& context, const Need need,
                             const std::string_view key, const std::string_view method,
                             std::string params) {
  Inquiry* const inquiry = context.inquiry;
  if (inquiry == nullptr) {
    return std::unexpected(failure("this call cannot ask the client anything"));
  }
  if (need == Need::elicitation && inquiry->form_mode && params.starts_with('{')) {
    params.insert(1, R"("mode":"form",)");
  }
  std::unique_lock lock{inquiry->mutex};
  std::string name{key};
  if (name.empty()) {
    name = "input-" + std::to_string(++inquiry->unnamed);
  }
  if (!can_ask(context, need)) {
    constexpr std::string_view required[] = {R"({"elicitation":{"form":{}}})",
                                             R"({"sampling":{}})", R"({"roots":{}})"};
    constexpr std::string_view what[] = {"elicitation", "sampling", "roots"};
    inquiry->missing = std::string{required[static_cast<std::size_t>(need)]};
    return std::unexpected(failure("the client did not declare the " +
                                   std::string{what[static_cast<std::size_t>(need)]} +
                                   " capability"));
  }
  if (inquiry->modern) {
    if (const auto found = inquiry->answers.find(name);
        found != inquiry->answers.end()) {
      if (!found->second.starts_with('{')) {
        inquiry->invalid = "inputResponses[\"" + name + "\"] must be an object";
        return std::unexpected(failure(*inquiry->invalid));
      }
      return scrydantic::Json{.text = found->second};
    }
    std::string request{R"({"method":)"};
    request += scrydantic::escape_json_string(method);
    request += R"(,"params":)";
    request += params;
    request += '}';
    inquiry->unanswered.emplace_back(std::move(name), std::move(request));
    // The function returns this, and the server answers the round with
    // input_required; the text is never sent.
    scrydantic::Error pending;
    pending.message = "the client has still to answer";
    return std::unexpected(std::move(pending));
  }
  lock.unlock();
  if (!inquiry->round_trip) {
    return std::unexpected(failure("this call cannot ask the client anything"));
  }
  Result<std::string> answer = inquiry->round_trip(method, params);
  if (!answer) {
    return std::unexpected(std::move(answer).error());
  }
  return scrydantic::Json{.text = std::move(*answer)};
}

} // namespace detail

Result<Answer<scrydantic::Json>>
Elicitation::form(const std::string_view message,
                  const scrydantic::Json& requested_schema,
                  const std::string_view key) const {
  if (context_ == nullptr) {
    return std::unexpected(failure("no client to ask"));
  }
  std::string params{R"({"message":)"};
  params += scrydantic::escape_json_string(message);
  params += R"(,"requestedSchema":)";
  params += requested_schema.text;
  params += '}';
  Result<scrydantic::Json> answer =
      detail::ask(*context_, detail::Need::elicitation, key, "elicitation/create",
                  std::move(params));
  if (!answer) {
    return std::unexpected(std::move(answer).error());
  }
  Result<detail::ElicitReply> reply = scrydantic::decode<detail::ElicitReply>(*answer);
  if (!reply) {
    reject_answer(*context_, "the answer to a form", reply.error());
    return std::unexpected(std::move(reply).error());
  }
  Answer<scrydantic::Json> result{.action = reply->action, .content = std::nullopt};
  if (reply->action == Action::accept) {
    result.content = reply->content.value_or(scrydantic::Json{.text = "{}"});
  }
  return result;
}

Result<SamplingResult> Sampling::create(const SamplingRequest& request,
                                        const std::string_view key) const {
  if (context_ == nullptr) {
    return std::unexpected(failure("no client to ask"));
  }
  Result<scrydantic::Json> params = scrydantic::encode(request);
  if (!params) {
    return std::unexpected(std::move(params).error());
  }
  Result<scrydantic::Json> answer =
      detail::ask(*context_, detail::Need::sampling, key, "sampling/createMessage",
                  std::move(params->text));
  if (!answer) {
    return std::unexpected(std::move(answer).error());
  }
  Result<SampledMessage> message = scrydantic::decode<SampledMessage>(*answer);
  if (!message) {
    reject_answer(*context_, "the model's message", message.error());
    return std::unexpected(std::move(message).error());
  }
  // The content is one block, or since 2025-11-25 a list of them.
  Result<Content> content = message->content.text.starts_with('[')
                                ? scrydantic::decode<Content>(message->content)
                                : scrydantic::decode<ContentBlock>(message->content)
                                      .transform([](ContentBlock block) {
                                        return Content{std::move(block)};
                                      });
  if (!content) {
    return std::unexpected(std::move(content).error());
  }
  return SamplingResult{.role = message->role,
                        .content = std::move(*content),
                        .model = std::move(message->model),
                        .stop_reason = message->stop_reason.value_or(std::string{})};
}

Result<std::vector<Root>> Roots::list(const std::string_view key) const {
  if (context_ == nullptr) {
    return std::unexpected(failure("no client to ask"));
  }
  Result<scrydantic::Json> answer =
      detail::ask(*context_, detail::Need::roots, key, "roots/list", "{}");
  if (!answer) {
    return std::unexpected(std::move(answer).error());
  }
  Result<RootList> list = scrydantic::decode<RootList>(*answer);
  if (!list) {
    reject_answer(*context_, "the list of roots", list.error());
    return std::unexpected(std::move(list).error());
  }
  return std::move(list->roots);
}

} // namespace mcp
