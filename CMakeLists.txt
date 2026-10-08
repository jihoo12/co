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

# Layout: build/bin/coc and build/lib/libco_rt.a, mirroring the install tree,
# so the compiler can find its runtime at <exe>/../lib.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

add_library(co_rt STATIC runtime/runtime.c)
set_target_properties(co_rt PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(co_rt PRIVATE -O2 -Wall)

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
)
target_include_directories(coc PRIVATE src ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
target_compile_definitions(coc PRIVATE ${LLVM_DEFINITIONS_LIST})
target_compile_options(coc PRIVATE -Wall -Wno-unused-parameter)
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(coc PRIVATE -fno-rtti)
endif()

if(LLVM_LINK_LLVM_DYLIB)
  target_link_libraries(coc PRIVATE LLVM)
else()
  llvm_map_components_to_libnames(llvm_libs core support passes target targetparser mc native)
  target_link_libraries(coc PRIVATE ${llvm_libs})
endif()
add_dependencies(coc co_rt)

install(TARGETS coc RUNTIME DESTINATION bin)
install(TARGETS co_rt ARCHIVE DESTINATION lib)

enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME language-tests
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_tests.py $<TARGET_FILE:coc> ${CMAKE_SOURCE_DIR}/tests)
