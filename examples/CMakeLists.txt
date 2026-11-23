# A complete server: a few annotated functions, served over stdio.
add_executable(mcp_weather_example weather.cpp)
target_link_libraries(mcp_weather_example PRIVATE mcp::mcp mcp_project_options)
