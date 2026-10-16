cmake_minimum_required(VERSION 3.16)
project(mooncake CXX C)

# indicates cmake is invoked from top-level dir
set(GLOBAL_CONFIG "true")

include(mooncake-common/FindJsonCpp.cmake)
include(mooncake-common/FindGLOG.cmake)
include(mooncake-common/common.cmake)

# Experimental single-GPU-per-process LUCA Cross v2 transport.
option(USE_LUCA "Build MACA-derived asynchronous LUCA Cross v2 transport" OFF)
set(LUCA_ROOT "/opt/luca" CACHE PATH "LUCA native SDK root")
if (USE_LUCA)
  if (NOT USE_CUDA)
    message(FATAL_ERROR "USE_LUCA requires USE_CUDA=ON for allocation-range queries")
  endif()
  if (USE_MNNVL OR USE_ASCEND OR USE_ASCEND_DIRECT OR USE_ASCEND_HETEROGENEOUS)
    message(FATAL_ERROR "Experimental LUCA transport requires MNNVL/Ascend options OFF")
  endif()
  find_path(LUCA_NATIVE_INCLUDE_DIR lcr/lc_runtime_api.h
            HINTS "${LUCA_ROOT}/include" NO_DEFAULT_PATH)
  find_library(LUCA_NATIVE_LIBRARY NAMES lcruntime
               HINTS "${LUCA_ROOT}/lib" "${LUCA_ROOT}/lib64" NO_DEFAULT_PATH)
  if (NOT LUCA_NATIVE_INCLUDE_DIR OR NOT LUCA_NATIVE_LIBRARY)
    message(FATAL_ERROR "LUCA native headers/library not found under LUCA_ROOT")
  endif()
  include_directories(${LUCA_NATIVE_INCLUDE_DIR})
  add_compile_definitions(USE_LUCA)
endif()
# unit test
if (BUILD_UNIT_TESTS)
  enable_testing()
endif()

option(WITH_STORE "build mooncake store library and sample code" ON)
option(WITH_P2P_STORE "build p2p store library and sample code" OFF)
option(WITH_RUST_EXAMPLE "build the Rust interface and sample code for the transfer engine" OFF)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/extern/pybind11)
set(PYTHON_EXECUTABLE "/opt/conda/bin/python3.12")
execute_process(
    COMMAND ${PYTHON_EXECUTABLE} -c "import sys; print(sys.path[-1])"
    OUTPUT_VARIABLE PYTHON_SYS_PATH
)
string(STRIP ${PYTHON_SYS_PATH} PYTHON_SYS_PATH)

if (USE_ETCD)
  add_compile_definitions(USE_ETCD)
  if (USE_ETCD_LEGACY)
    add_compile_definitions(USE_ETCD_LEGACY)
    message(STATUS "etcd as metadata server support is enabled (etcd-cpp-api-v3)")
  else()
    message(STATUS "etcd as metadata server support is enabled (go package)")
  endif()
endif()
option(STORE_USE_ETCD "build mooncake store with etcd" OFF)
if (STORE_USE_ETCD)
  add_compile_definitions(STORE_USE_ETCD)
endif()

add_subdirectory(mooncake-common)
include_directories(mooncake-common/etcd)
include_directories(mooncake-common/include)

add_subdirectory(mooncake-transfer-engine)
include_directories(mooncake-transfer-engine/include)

if (WITH_STORE)
  message(STATUS "Mooncake Store will be built")
  add_subdirectory(mooncake-store)
  include_directories(mooncake-store/include)
endif()

add_subdirectory(mooncake-integration)

if (WITH_P2P_STORE)
  add_subdirectory(mooncake-p2p-store)
  message(STATUS "P2P Store will be built")
endif()
