file(GLOB XPORT_SOURCES "*.cpp")

add_subdirectory(rdma_transport)
add_library(transport OBJECT ${XPORT_SOURCES} $<TARGET_OBJECTS:rdma_transport>)
target_link_libraries(transport PRIVATE JsonCpp::JsonCpp yalantinglibs::yalantinglibs glog::glog pthread)
if (USE_LUCA)
  target_sources(transport PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/luca_transport/luca_transport.cpp)
  target_include_directories(transport PRIVATE ${LUCA_NATIVE_INCLUDE_DIR} /usr/local/cuda/include)
endif()

if (USE_TCP)
  add_subdirectory(tcp_transport)
  target_sources(transport PUBLIC $<TARGET_OBJECTS:tcp_transport>)
endif()

if (USE_NVMEOF)
  add_subdirectory(nvmeof_transport)
  target_sources(transport PUBLIC $<TARGET_OBJECTS:nvmeof_transport>)
endif()

if (USE_CXL)
  add_subdirectory(cxl_transport)
  target_sources(transport PUBLIC $<TARGET_OBJECTS:cxl_transport>)
endif()

if (USE_ASCEND OR USE_ASCEND_DIRECT)
  add_subdirectory(ascend_transport)
  target_sources(transport PUBLIC $<TARGET_OBJECTS:ascend_transport>)
endif()

if (USE_ASCEND_HETEROGENEOUS)
  add_subdirectory(ascend_transport)
  target_sources(transport PUBLIC $<TARGET_OBJECTS:ascend_transport>)
endif()

if (USE_MNNVL)
  add_subdirectory(nvlink_transport)
  target_sources(transport PUBLIC $<TARGET_OBJECTS:nvlink_transport>)
endif()
