# External AI control bridge: connects the game to the AI server over WebSocket.
# Fork-specific code lives here to keep upstream files nearly untouched.

include(FetchContent)

set(USE_TLS             OFF CACHE BOOL "" FORCE)
set(USE_ZLIB            OFF CACHE BOOL "" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(ixwebsocket
  GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
  GIT_TAG        v12.0.1
  GIT_SHALLOW    TRUE
  SYSTEM)
FetchContent_MakeAvailable(ixwebsocket)

file(GLOB AIBRIDGE_SOURCES CONFIGURE_DEPENDS "*.h" "*.cpp")

add_library(AiBridge STATIC ${AIBRIDGE_SOURCES})
target_include_directories(AiBridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# rapidjson 1.1 trips modern clang warnings; treat it as a system header
target_include_directories(AiBridge SYSTEM BEFORE PRIVATE ${PROJECT_SOURCE_DIR}/lib/rapidjson/include)
target_link_libraries(AiBridge PUBLIC OpenGothic PRIVATE ixwebsocket::ixwebsocket)

if(NOT MSVC)
  target_compile_options(AiBridge PRIVATE -Wall -Wconversion -Wno-strict-aliasing -Werror)
endif()
