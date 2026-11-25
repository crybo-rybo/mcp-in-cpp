// The client the official conformance suite tests in its client mode. The
// suite starts a server for one scenario, runs this program with the server's
// URL as its last argument, and names the scenario, the protocol revision,
// and any data the scenario needs in the environment. The program does what
// the scenario asks of a client, and exits with 1 when a step fails.
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <iostream>
#include <mcp/client.hpp>
#include <mcp/http_client.hpp>
#include <mcp/httplib_client.hpp>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using scrydantic::JsonKind;
using scrydantic::JsonView;

[[nodiscard]] std::string environment(const char* const name) {
  const char* const value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string{value};
}

// The era the suite asks for: 2026-07-28 is stateless, and the revisions
// before it use the handshake.
[[nodiscard]] mcp::Era era_of(const std::string_view version) {
  if (version.empty()) {
    return mcp::Era::automatic;
  }
  return version == mcp::modern_protocol_version ? mcp::Era::modern : mcp::Era::legacy;
}

bool report(const std::string_view step, const mcp::ClientError& error) {
  std::cerr << step << " failed: " << error.message << '\n';
  return false;
}

template <typename T>
bool check(const std::string_view step, const mcp::Reply<T>& reply) {
  return reply.has_value() || report(step, reply.error());
}

// Calls a tool and says whether that worked; a tool's own failure counts as
// a call that worked.
bool call(mcp::Client& client, const std::string_view name,
          const std::string_view arguments = "{}") {
  return check("tools/call " + std::string{name},
               client.call_tool(name, scrydantic::Json{std::string{arguments}}));
}

const mcp::Tool* find(const std::vector<mcp::Tool>& tools,
                      const std::string_view name) {
  for (const mcp::Tool& tool : tools) {
    if (tool.name == name) {
      return &tool;
    }
  }
  return nullptr;
}

// The tool calls a scenario's context lists: each one's name and arguments.
std::vector<std::pair<std::string, std::string>> context_calls() {
  std::vector<std::pair<std::string, std::string>> calls;
  const scrydantic::Result<JsonView> context =
      JsonView::parse(environment("MCP_CONFORMANCE_CONTEXT"));
  if (!context || context->kind() != JsonKind::object) {
    return calls;
  }
  const std::optional<JsonView> list = context->find("toolCalls");
  for (std::size_t index = 0; list.has_value() && index < list->size(); ++index) {
    const JsonView entry = *list->at(index);
    const std::optional<JsonView> name = entry.find("name");
    const std::optional<JsonView> arguments = entry.find("arguments");
    if (name.has_value() && name->string().has_value()) {
      calls.emplace_back(std::string{*name->string()},
                         arguments.has_value() ? arguments->to_json().text : "{}");
    }
  }
  return calls;
}

bool run(mcp::Client& client, const std::string_view scenario) {
  const mcp::Reply<std::vector<mcp::Tool>> tools = client.list_tools();
  if (!check("tools/list", tools)) {
    return false;
  }
  if (scenario == "json-schema-ref-no-deref" || scenario == "request-metadata") {
    return true;
  }
  if (scenario == "json-schema-2020-12-preservation") {
    // The schema goes back exactly as the client received it.
    const mcp::Tool* const focal = find(*tools, "json_schema_2020_12_tool");
    if (focal == nullptr) {
      std::cerr << "the focal tool is not listed\n";
      return false;
    }
    return call(client, "json_schema_echo",
                R"({"schema":)" + focal->input_schema.text + "}");
  }
  if (scenario == "http-custom-headers") {
    bool ok = true;
    for (const auto& [name, arguments] : context_calls()) {
      ok = call(client, name, arguments) && ok;
    }
    return ok;
  }
  if (scenario == "http-standard-headers" || scenario == "http-invalid-tool-headers") {
    // Every tool the client keeps is called; one whose x-mcp-header marks
    // break the rules is not listed.
    bool ok = true;
    for (const mcp::Tool& tool : *tools) {
      ok = call(client, tool.name) && ok;
    }
    if (scenario == "http-standard-headers") {
      // The headers of the other requests that name something: every resource
      // is read, and every prompt rendered.
      const mcp::Reply<std::vector<mcp::Resource>> resources = client.list_resources();
      ok = check("resources/list", resources) && ok;
      for (std::size_t index = 0; resources && index < resources->size(); ++index) {
        ok = check("resources/read", client.read_resource((*resources)[index].uri)) &&
             ok;
      }
      const mcp::Reply<std::vector<mcp::Prompt>> prompts = client.list_prompts();
      ok = check("prompts/list", prompts) && ok;
      for (std::size_t index = 0; prompts && index < prompts->size(); ++index) {
        ok = check("prompts/get", client.get_prompt((*prompts)[index].name)) && ok;
      }
    }
    return ok;
  }
  if (scenario == "sep-2322-client-request-state") {
    return call(client, "test_mrtr_echo_state") && call(client, "test_mrtr_no_state") &&
           call(client, "test_mrtr_no_result_type");
  }
  if (scenario == "elicitation-sep1034-client-defaults") {
    return call(client, "test_client_elicitation_defaults");
  }
  if (scenario.starts_with("auth/")) {
    return call(client, "test-tool");
  }
  // initialize, tools_call, sse-retry, and any other: the first tool.
  if (tools->empty()) {
    return true;
  }
  return call(client, tools->front().name, R"({"a":2,"b":3})");
}

} // namespace

int main(const int argc, const char* const argv[]) {
  if (argc < 2) {
    std::cerr << "usage: mcp_conformance_client URL\n";
    return 2;
  }
  const std::string url = argv[argc - 1];
  const std::string scenario = environment("MCP_CONFORMANCE_SCENARIO");
  const std::string version = environment("MCP_CONFORMANCE_PROTOCOL_VERSION");

  // Set once the client has opened, for the elicitation handler, which calls
  // another tool between two rounds of a call.
  mcp::Client* opened = nullptr;
  // A call gives up before the suite's own 30 seconds do, so that the client
  // says why.
  mcp::ClientOptions options{.name = "mcp-in-cpp-conformance",
                             .version = "0.2.0",
                             .era = era_of(version),
                             .timeout = std::chrono::seconds{20}};
  options.elicitation = [&opened, &scenario](const mcp::FormRequest&) {
    if (scenario == "sep-2322-client-request-state" && opened != nullptr) {
      // A call made while another waits for input carries none of its state.
      static_cast<void>(opened->call_tool("test_mrtr_unrelated"));
    }
    // Accepting with nothing filled in sends the form's defaults.
    return mcp::Answer<scrydantic::Json>{
        .action = mcp::Action::accept,
        .content = scrydantic::Json{scenario == "sep-2322-client-request-state"
                                        ? R"({"confirmed":true})"
                                        : "{}"}};
  };
  options.sampling =
      [](const mcp::SamplingRequest&) -> mcp::Result<mcp::SamplingResult> {
    return mcp::SamplingResult{.role = mcp::Role::assistant,
                               .content = {mcp::TextContent{.text = "conformance"}},
                               .model = "none",
                               .stop_reason = "endTurn"};
  };
  options.roots = [] { return std::vector<mcp::Root>{{.uri = "file:///conformance"}}; };

  mcp::HttplibClient http;
  mcp::Reply<mcp::Client> client = mcp::connect(url, http, options);
  if (!client) {
    report("opening", client.error());
    return 1;
  }
  opened = &*client;
  const bool ok = run(*client, scenario);
  opened = nullptr;
  return ok ? 0 : 1;
}
