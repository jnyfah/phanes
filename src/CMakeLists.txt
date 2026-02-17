# Core module target
add_library(phanes_core)

target_sources(phanes_core
  PUBLIC
    FILE_SET CXX_MODULES
    FILES
      core/core.ixx
      builder/builder.ixx
  PRIVATE
      builder/builder.cpp
)

# Executable
add_executable(phanes
  main.cpp
)

target_link_libraries(phanes
  PRIVATE phanes_core
)
