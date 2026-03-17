# Core module target
add_library(phanes_core)

target_sources(phanes_core
  PUBLIC
    FILE_SET CXX_MODULES
    FILES
      core/core.ixx
      builder/builder.ixx
      builder/scheduler.cpp 
      builder/deque.cpp 
      analyzer/analyzer.ixx
      parser/parser.ixx
      view/view.ixx
      view/executor.ixx
  PRIVATE
      builder/builder.cpp
      analyzer/analyzer.cpp
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
