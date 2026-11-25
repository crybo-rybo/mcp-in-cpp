// What a client reads from the results of a server's resources, prompts and
// completion, and from its log messages, in either era. Each reader takes a
// result as the JSON text the server wrote and a view of it: what the client
// does not interpret, such as annotations, is kept as that text.
#pragma once

#include <mcp/channel.hpp>
#include <mcp/client.hpp>
#include <mcp/content.hpp>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <string_view>
#include <vector>

namespace mcp::features {

// The entries of one page of resources/list, added to `resources`.
[[nodiscard]] Reply<void> read_resources(std::string_view text,
                                         const scrydantic::JsonView& result,
                                         std::vector<Resource>& resources);

// The entries of one page of resources/templates/list, added to `templates`.
[[nodiscard]] Reply<void> read_templates(std::string_view text,
                                         const scrydantic::JsonView& result,
                                         std::vector<ResourceTemplate>& templates);

// The entries of one page of prompts/list, added to `prompts`.
[[nodiscard]] Reply<void> read_prompts(std::string_view text,
                                       const scrydantic::JsonView& result,
                                       std::vector<Prompt>& prompts);

// The contents of resources/read.
[[nodiscard]] Reply<std::vector<ResourceContents>>
read_contents(const scrydantic::JsonView& result);

// The result of prompts/get.
[[nodiscard]] Reply<PromptResult> read_prompt(const scrydantic::JsonView& result);

// The result of completion/complete.
[[nodiscard]] Reply<Completion> read_completion(const scrydantic::JsonView& result);

// The message of notifications/message, from its params as the server wrote
// them, or nothing when they are not one.
[[nodiscard]] std::optional<LogMessage> read_log(std::string_view text,
                                                 const scrydantic::JsonView& params);

} // namespace mcp::features
