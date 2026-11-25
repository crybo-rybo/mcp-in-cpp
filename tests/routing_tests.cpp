// What Streamable HTTP mirrors into headers under 2026-07-28, and how a
// client reads an event stream: the parts of the client's HTTP transport that
// need no server.
#include "event_stream.hpp"
#include "routing.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <expected>
#include <mcp/http.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

// Each header as `Name: value`, in order.
std::vector<std::string> lines(const std::vector<mcp::HttpHeader>& headers) {
  std::vector<std::string> out;
  for (const mcp::HttpHeader& header : headers) {
    out.push_back(header.name + ": " + header.value);
  }
  return out;
}

// The events a stream gives for these parts, each as `type: data`.
std::vector<std::string> events_of(const std::vector<std::string_view>& parts,
                                   const std::size_t most = 1024) {
  mcp::EventStream stream{most};
  std::vector<std::string> events;
  for (const std::string_view part : parts) {
    stream.feed(part, [&events](const mcp::EventStream::Event& event) {
      events.push_back(std::string{event.type} + ": " + std::string{event.data});
    });
  }
  return events;
}

} // namespace

TEST_CASE("a header value is sent as it is when a header can hold it") {
  CHECK(mcp::routing::header_value("us-west1") == "us-west1");
  CHECK(mcp::routing::header_value("us west 1") == "us west 1");
  CHECK(mcp::routing::header_value("") == "");
  CHECK(mcp::routing::header_value("file:///path/to/file%20name.txt") ==
        "file:///path/to/file%20name.txt");
}

TEST_CASE("a header value a header cannot hold is sent in Base64") {
  // The spec's own examples.
  CHECK(mcp::routing::header_value("Hello, 世界") == "=?base64?SGVsbG8sIOS4lueVjA==?=");
  CHECK(mcp::routing::header_value(" padded ") == "=?base64?IHBhZGRlZCA=?=");
  CHECK(mcp::routing::header_value("line1\nline2") == "=?base64?bGluZTEKbGluZTI=?=");
  CHECK(mcp::routing::header_value("=?base64?literal?=") ==
        "=?base64?PT9iYXNlNjQ/bGl0ZXJhbD89?=");
  // A tab, a space at one end only, and each length of the last group.
  CHECK(mcp::routing::header_value("\tindented") == "=?base64?CWluZGVudGVk?=");
  CHECK(mcp::routing::header_value("Oslo, Norge ") == "=?base64?T3NsbywgTm9yZ2Ug?=");
  CHECK(mcp::routing::header_value(" a") == "=?base64?IGE=?=");
  CHECK(mcp::routing::header_value(" ab") == "=?base64?IGFi?=");
  CHECK(mcp::routing::header_value("\x7F") == "=?base64?fw==?=");
}

TEST_CASE("the arguments a tool marks are found through properties alone") {
  const auto params = mcp::routing::params_of(
      R"({"type":"object","properties":{)"
      R"("region":{"type":"string","x-mcp-header":"Region"},)"
      R"("query":{"type":"string"},)"
      R"("where":{"type":"object","properties":{)"
      R"("zone":{"type":"integer","x-mcp-header":"Zone"}}},)"
      R"("loud":{"type":"boolean","x-mcp-header":"Loud","default":true}}})");
  REQUIRE(params.has_value());
  REQUIRE(params->size() == 3);
  // Keys are visited in order.
  CHECK((*params)[0].header == "Loud");
  CHECK((*params)[0].path == std::vector<std::string>{"loud"});
  CHECK((*params)[1].header == "Region");
  CHECK((*params)[2].header == "Zone");
  CHECK((*params)[2].path == std::vector<std::string>{"where", "zone"});
}

TEST_CASE("what looks like a mark but is data or a name is not one") {
  const auto params = mcp::routing::params_of(
      R"({"type":"object","properties":{)"
      R"("x-mcp-header":{"type":"string"},)"
      R"("config":{"type":"object","default":{"x-mcp-header":"Nope"},)"
      R"("examples":[{"x-mcp-header":"Nope"}]}}})");
  REQUIRE(params.has_value());
  CHECK(params->empty());
  // A schema without marks has none, and so has a schema of no object.
  CHECK(mcp::routing::params_of(R"({"type":"object"})")->empty());
  CHECK(mcp::routing::params_of("true")->empty());
}

TEST_CASE("a mark that breaks the rules makes the tool unusable") {
  const auto property = [](const std::string_view schema) {
    return std::string{R"({"type":"object","properties":{"value":)"} +
           std::string{schema} + "}}";
  };
  const std::vector<std::string> broken{
      // The conformance suite's invalid tools.
      property(R"({"type":"string","x-mcp-header":""})"),
      property(R"({"type":"object","x-mcp-header":"Data"})"),
      property(R"({"type":"array","items":{"type":"string"},"x-mcp-header":"Items"})"),
      property(R"({"type":"null","x-mcp-header":"Nil"})"),
      property(R"({"type":"number","x-mcp-header":"Score"})"),
      R"({"type":"object","properties":{"a":{"type":"string","x-mcp-header":"Region"},)"
      R"("b":{"type":"string","x-mcp-header":"Region"}}})",
      R"({"type":"object","properties":{"a":{"type":"string","x-mcp-header":"MyField"},)"
      R"("b":{"type":"string","x-mcp-header":"myfield"}}})",
      property(R"({"type":"string","x-mcp-header":"My Region"})"),
      property(R"({"type":"string","x-mcp-header":"Region:Primary"})"),
      property(R"({"type":"string","x-mcp-header":"Région"})"),
      property(R"({"type":"string","x-mcp-header":"Region\t1"})"),
      // A name that is not a string, and a property with no single type.
      property(R"({"type":"string","x-mcp-header":7})"),
      property(R"({"x-mcp-header":"Untyped"})"),
      property(R"({"type":["string","null"],"x-mcp-header":"Either"})"),
      // A mark that is not reached through properties alone.
      R"({"type":"object","x-mcp-header":"Root"})",
      R"({"type":"object","properties":{"list":{"type":"array",)"
      R"("items":{"type":"string","x-mcp-header":"Item"}}}})",
      R"({"type":"object","anyOf":[{"properties":{"a":{"type":"string",)"
      R"("x-mcp-header":"A"}}}]})",
      R"({"type":"object","properties":{"a":{"$ref":"#/$defs/a"}},)"
      R"("$defs":{"a":{"type":"string","x-mcp-header":"A"}}})",
      R"({"type":"object","if":{"properties":{"a":{"type":"string",)"
      R"("x-mcp-header":"A"}}}})",
  };
  for (const std::string& schema : broken) {
    INFO(schema);
    const auto params = mcp::routing::params_of(schema);
    CHECK_FALSE(params.has_value());
  }
}

TEST_CASE("a call mirrors each marked argument that has a value") {
  const auto params = mcp::routing::params_of(
      R"({"type":"object","properties":{)"
      R"("region":{"type":"string","x-mcp-header":"Region"},)"
      R"("priority":{"type":"integer","x-mcp-header":"Priority"},)"
      R"("verbose":{"type":"boolean","x-mcp-header":"Verbose"},)"
      R"("debug":{"type":"boolean","x-mcp-header":"Debug"},)"
      R"("empty":{"type":"string","x-mcp-header":"Empty"},)"
      R"("greeting":{"type":"string","x-mcp-header":"Greeting"},)"
      R"("where":{"type":"object","properties":{)"
      R"("zone":{"type":"integer","x-mcp-header":"Zone"}}}}})");
  REQUIRE(params.has_value());
  CHECK(lines(mcp::routing::headers_for(
            *params, R"({"region":"us-west1","priority":-7,"verbose":null,)"
                     R"("debug":false,"empty":"","greeting":"Hello, 世界",)"
                     R"("where":{"zone":42},"query":"SELECT 1"})")) ==
        std::vector<std::string>{"Mcp-Param-Debug: false", "Mcp-Param-Empty: ",
                                 "Mcp-Param-Greeting: =?base64?SGVsbG8sIOS4lueVjA==?=",
                                 "Mcp-Param-Priority: -7", "Mcp-Param-Region: us-west1",
                                 "Mcp-Param-Zone: 42"});
  // Absent, null, and a value of no allowed type have no header.
  CHECK(mcp::routing::headers_for(*params, R"({"where":7,"region":["a"]})").empty());
  CHECK(lines(mcp::routing::headers_for(*params, R"({"debug":true})")) ==
        std::vector<std::string>{"Mcp-Param-Debug: true"});
}

TEST_CASE("an event stream is read across any line break and any split") {
  // LF, CRLF and CR, the last split between two parts.
  CHECK(events_of({"data: a\n\n", "data: b\r\n\r\n", "data: c\r\r"}) ==
        std::vector<std::string>{"message: a", "message: b", "message: c"});
  // A CRLF split between two parts is one line break, not two.
  CHECK(events_of({"data: a\r", "\ndata: b\r", "\n\r", "\n"}) ==
        std::vector<std::string>{"message: a\nb"});
  // One byte at a time.
  const std::string whole = "event: message\r\ndata: {\"id\":1}\r\n\r\n";
  std::vector<std::string_view> bytes;
  for (std::size_t at = 0; at < whole.size(); ++at) {
    bytes.push_back(std::string_view{whole}.substr(at, 1));
  }
  CHECK(events_of(bytes) == std::vector<std::string>{"message: {\"id\":1}"});
}

TEST_CASE("an event stream's fields are read as the HTML standard has them") {
  // Comments, fields without a space or a colon, and a field it ignores.
  CHECK(events_of({": keepalive\n\ndata:x\nfoo: bar\ndata\ndata:  y\n\n"}) ==
        std::vector<std::string>{"message: x\n\n y"});
  // A named event, and the type does not outlive its event.
  CHECK(events_of({"event: ping\ndata: 1\n\ndata: 2\n\n"}) ==
        std::vector<std::string>{"ping: 1", "message: 2"});
  // An event without data is not one, nor is the last event when the stream
  // ends inside it. A byte order mark may open the stream.
  CHECK(events_of({"\xEF\xBB\xBF", "id: 7\nevent: x\n\ndata: kept\n\ndata: lost"}) ==
        std::vector<std::string>{"message: kept"});
  // An event whose data is empty is still one.
  CHECK(events_of({"id: 1\nretry: 500\ndata: \n\n"}) ==
        std::vector<std::string>{"message: "});
}

TEST_CASE("an event stream keeps its last id and its retry") {
  mcp::EventStream stream{1024};
  const auto ignore = [](const mcp::EventStream::Event&) {};
  CHECK(stream.feed("id: 41\nretry: 500\ndata: x\n\n", ignore));
  CHECK(stream.last_id() == "41");
  CHECK(stream.retry() == 500ms);
  // A retry that is not all digits is ignored, and an id with NUL too.
  CHECK(stream.feed(std::string_view{"retry: 5s\nid: a\0b\n\n", 19}, ignore));
  CHECK(stream.last_id() == "41");
  CHECK(stream.retry() == 500ms);
}

TEST_CASE("UTF-8 split between two parts reaches the event whole") {
  // 世 is E4 B8 96.
  CHECK(events_of({"data: \xE4", "\xB8", "\x96\n\n"}) ==
        std::vector<std::string>{"message: 世"});
}

TEST_CASE("an event stream that grows past its bound is given up") {
  mcp::EventStream stream{16};
  int events = 0;
  const auto count = [&events](const mcp::EventStream::Event&) { ++events; };
  CHECK(stream.feed("data: short\n\n", count));
  CHECK(events == 1);
  // A line with no end, and then data spread over many short lines.
  CHECK_FALSE(stream.feed(std::string(17, 'x'), count));
  mcp::EventStream lines_stream{16};
  CHECK_FALSE(lines_stream.feed("data: 12345\ndata: 12345\ndata: 12345\n", count));
  // Nothing more is read.
  CHECK_FALSE(lines_stream.feed("\n", count));
  CHECK(events == 1);
}
