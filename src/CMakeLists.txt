# Core module target
add_library(phanes_core STATIC)

if(WIN32)
  set(PHANES_IO_BACKEND io/oring.cpp)
else()
  set(PHANES_IO_BACKEND io/uring.cpp)
endif()

target_sources(phanes_core
  PUBLIC
    FILE_SET CXX_MODULES
    FILES
      core/core.ixx
      builder/builder.ixx
      builder/scheduler.cpp
      builder/deque.cpp
      analyzer/analyzer.ixx
      analyzer/hasher.cpp
      parser/parser.ixx
      view/view.ixx
      view/executor.ixx
      ${PHANES_IO_BACKEND}
  PRIVATE
      builder/builder.cpp
      analyzer/analyzer.cpp
      analyzer/duplicates.cpp
      parser/parser.cpp
      view/view.cpp
)

# Executable
add_executable(phanes
  main.cpp
)

target_link_libraries(phanes
  PRIVATE phanes_core
)

target_compile_options(phanes PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/wd4996>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wno-deprecated-declarations>
)

target_compile_options(phanes_core PRIVATE
    $<$<CXX_COMPILER_ID:Clang>:-Wno-odr>
)
