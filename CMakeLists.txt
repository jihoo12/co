cmake_minimum_required(VERSION 3.20)
project(co LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# The runtime is compiled to LLVM bitcode and embedded in coc, which links it
# into every program before optimizing, so runtime calls inline into user code
# and coc needs no files besides itself. The clang that compiles it must not be
# newer than the LLVM that reads the bitcode.
math(EXPR CO_LLVM_NEXT "${LLVM_VERSION_MAJOR} + 1")
if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_VERSION VERSION_LESS ${CO_LLVM_NEXT})
  set(CO_CLANG ${CMAKE_C_COMPILER} CACHE FILEPATH "clang used to compile the runtime to LLVM bitcode")
endif()
find_program(CO_CLANG NAMES clang-${LLVM_VERSION_MAJOR} clang HINTS ${LLVM_TOOLS_BINARY_DIR} REQUIRED)
message(STATUS "Compiling the runtime with ${CO_CLANG}")

set(CO_RUNTIME_BC ${CMAKE_BINARY_DIR}/runtime.bc)
set(CO_RUNTIME_CPP ${CMAKE_BINARY_DIR}/runtime_bitcode.cpp)
add_custom_command(
  OUTPUT ${CO_RUNTIME_BC}
  COMMAND ${CO_CLANG} -c -emit-llvm -O2 -std=c11 -Wall -Werror -fPIC
          -fno-stack-protector
          -I${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/runtime/runtime.c -o ${CO_RUNTIME_BC}
  DEPENDS runtime/runtime.c runtime/co_abi.h
  COMMENT "Compiling the co runtime to LLVM bitcode"
  VERBATIM)
add_custom_command(
  OUTPUT ${CO_RUNTIME_CPP}
  COMMAND ${CMAKE_COMMAND} -DIN=${CO_RUNTIME_BC} -DOUT=${CO_RUNTIME_CPP} -DNAME=runtimeBitcode
          -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS ${CO_RUNTIME_BC} cmake/embed.cmake
  VERBATIM)

add_executable(coc
  src/main.cpp
  src/diag.cpp
  src/lexer.cpp
  src/parser.cpp
  src/types.cpp
  src/sema.cpp
  src/mir.cpp
  src/mir_build.cpp
  src/borrowck.cpp
  src/codegen.cpp
  ${CO_RUNTIME_CPP}
)
target_include_directories(coc PRIVATE src runtime ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
target_compile_definitions(coc PRIVATE ${LLVM_DEFINITIONS_LIST})
target_compile_options(coc PRIVATE -Wall -Wno-unused-parameter)
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(coc PRIVATE -fno-rtti)
endif()

if(LLVM_LINK_LLVM_DYLIB)
  target_link_libraries(coc PRIVATE LLVM)
else()
  llvm_map_components_to_libnames(llvm_libs core support passes target targetparser mc native bitreader linker ipo)
  target_link_libraries(coc PRIVATE ${llvm_libs})
endif()
install(TARGETS coc RUNTIME DESTINATION bin)

enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME language-tests
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_tests.py $<TARGET_FILE:coc> ${CMAKE_SOURCE_DIR}/tests)
