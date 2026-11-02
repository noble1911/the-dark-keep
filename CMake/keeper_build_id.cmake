# Run at build time (see Source/CMakeLists.txt): names the commit being built, so that clients of different
# commits refuse to play together, whichever configure step last ran. Only touches the header when it changes.
execute_process(
  COMMAND git rev-parse HEAD
  WORKING_DIRECTORY ${SOURCE_DIR}
  OUTPUT_VARIABLE commit
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
  RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  # A source tarball: only the version number tells builds apart.
  set(commit "")
endif()
set(content "#pragma once\n#define KEEPER_BUILD_ID \"${commit}\"\n")
set(existing "")
if(EXISTS ${OUTPUT})
  file(READ ${OUTPUT} existing)
endif()
if(NOT existing STREQUAL content)
  file(WRITE ${OUTPUT} "${content}")
endif()
