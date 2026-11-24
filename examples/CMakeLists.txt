# A complete server: a few annotated functions, served over stdio, or over
# Streamable HTTP through cpp-httplib.
add_executable(mcp_weather_example weather.cpp)
target_link_libraries(
  mcp_weather_example
  PRIVATE mcp::mcp httplib::httplib mcp_project_options
)
