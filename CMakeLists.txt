# CMakeList.txt: projekt CMake dla elementu whatismyip-tb, dołącz źródło i zdefiniuj
# tutaj logikę specyficzną dla projektu.
#
cmake_minimum_required (VERSION 3.10)

# Włącz przeładowywanie kodu XAML na gorąco dla kompilatorów MSVC, jeśli jest to obsługiwane.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project ("whatismyip-tb")

# Dodaj źródło do pliku wykonywalnego tego projektu.
add_executable (whatismyip-tb WIN32 "whatismyip-tb.cpp" "whatismyip-tb.h" "whatismyip-tb.rc" "resource.h")
target_compile_definitions(whatismyip-tb PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0600 NTDDI_VERSION=0x06000000)
target_link_libraries(whatismyip-tb PRIVATE winhttp iphlpapi shell32 user32 ole32 ws2_32)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET whatismyip-tb PROPERTY CXX_STANDARD 20)
endif()

# TODO: Dodaj testy i zainstaluj elementy docelowe w razie potrzeby.
