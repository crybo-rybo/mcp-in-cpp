// Reading parts of a JSON text as the server wrote them.
#include "verbatim.hpp"

#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string_view>
#include <vector>

using mcp::verbatim::compact;
using mcp::verbatim::elements;
using mcp::verbatim::member;

TEST_CASE("a member is its own text, spacing, order and numbers kept") {
  constexpr std::string_view object =
      R"( { "a" : { "z": 1.50, "b": [1, {"c": "}"}] } , "b":true,"n" :null } )";
  CHECK(member(object, "a") == R"({ "z": 1.50, "b": [1, {"c": "}"}] })");
  CHECK(member(object, "b") == "true");
  CHECK(member(object, "n") == "null");
  CHECK(member(object, "z") == std::nullopt);
  CHECK(member("[1]", "a") == std::nullopt);
  CHECK(member("{}", "a") == std::nullopt);
}

TEST_CASE("strings that hold quotes, brackets and escapes are skipped whole") {
  constexpr std::string_view object =
      R"({"s":"a \"quoted\" } ] \\","t":"\\\"","u":["]",{"k":"{"}],"v":-1e-3})";
  CHECK(member(object, "s") == R"("a \"quoted\" } ] \\")");
  CHECK(member(object, "t") == R"("\\\"")");
  CHECK(member(object, "u") == R"(["]",{"k":"{"}])");
  CHECK(member(object, "v") == "-1e-3");
}

TEST_CASE("of two members of one key the last counts, and a key may be escaped") {
  CHECK(member(R"({"a":1,"a":2})", "a") == "2");
  CHECK(member(R"({"a":1})", "a") == "1");
  CHECK(member(R"({"a\"b":3})", "a\"b") == "3");
}

TEST_CASE("an array's elements are their own text") {
  CHECK(elements(R"([ 1 , "x,y" , {"a": [2, 3]}, [] ])") ==
        std::vector<std::string_view>{"1", R"("x,y")", R"({"a": [2, 3]})", "[]"});
  for (const std::string_view empty : {"[]", " [ ] "}) {
    const std::optional<std::vector<std::string_view>> none = elements(empty);
    REQUIRE(none.has_value());
    CHECK(none->empty());
  }
  CHECK_FALSE(elements("{}").has_value());
}

TEST_CASE("compact text keeps strings and numbers, and loses the spaces between") {
  CHECK(compact("{ \"a b\" :\n [ 1.50 ,\t\"x\\\" y\" ] }\r\n") ==
        R"({"a b":[1.50,"x\" y"]})");
}
