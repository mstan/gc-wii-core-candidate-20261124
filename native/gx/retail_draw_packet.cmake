# SPDX-License-Identifier: MIT
set(_gcnllr_gx_donor "${GXRUNTIME_DIR}/graphics/aurora/lib/gx")
set(_gcnllr_draw_packet_header "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/retail_pipeline.hpp")
set(_gcnllr_draw_packet_source "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/retail_pipeline.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_FILE}" "${_gcnllr_gx_donor}/pipeline.hpp" "${_gcnllr_gx_donor}/pipeline.cpp")

include("${CMAKE_CURRENT_LIST_DIR}/source_replace.cmake")

file(READ "${_gcnllr_gx_donor}/pipeline.hpp" _gcnllr_draw_packet)
gcnllr_draw_packet_replace(_gcnllr_draw_packet "#include \"../gfx/common.hpp\""
  "#include \"${_gcnllr_gx_donor}/../gfx/common.hpp\"")
gcnllr_draw_packet_replace(_gcnllr_draw_packet "#include \"gx.hpp\""
  "#include \"${_gcnllr_gx_donor}/gx.hpp\"")
gcnllr_draw_packet_replace(_gcnllr_draw_packet "  gfx::Range uniformRange;"
  "  gfx::Range uniformRange;\n  uint32_t firstInstance;")
file(CONFIGURE OUTPUT "${_gcnllr_draw_packet_header}" CONTENT "${_gcnllr_draw_packet}" @ONLY)

file(READ "${_gcnllr_gx_donor}/pipeline.cpp" _gcnllr_draw_render)
gcnllr_draw_packet_replace(_gcnllr_draw_render "#include \"pipeline.hpp\""
  "#include \"${_gcnllr_draw_packet_header}\"")
gcnllr_draw_packet_replace(_gcnllr_draw_render "pass.Draw(data.vtxCount, data.instanceCount);"
  "pass.Draw(data.vtxCount, data.instanceCount, 0, data.firstInstance);")
gcnllr_draw_packet_replace(_gcnllr_draw_render "pass.DrawIndexed(data.indexCount, data.instanceCount);"
  "pass.DrawIndexed(data.indexCount, data.instanceCount, 0, 0, data.firstInstance);")
file(CONFIGURE OUTPUT "${_gcnllr_draw_packet_source}" CONTENT "${_gcnllr_draw_render}" @ONLY)
get_target_property(_gcnllr_draw_packet_sources aurora_gx SOURCES)
list(FIND _gcnllr_draw_packet_sources lib/gx/pipeline.cpp _gcnllr_draw_packet_index)
if(_gcnllr_draw_packet_index EQUAL -1)
  message(FATAL_ERROR "Pinned retail draw render target is missing")
endif()
list(REMOVE_ITEM _gcnllr_draw_packet_sources lib/gx/pipeline.cpp)
list(APPEND _gcnllr_draw_packet_sources "${_gcnllr_draw_packet_source}")
set_property(TARGET aurora_gx PROPERTY SOURCES "${_gcnllr_draw_packet_sources}")
set_source_files_properties("${_gcnllr_draw_packet_source}" TARGET_DIRECTORY aurora_gx
  PROPERTIES INCLUDE_DIRECTORIES "${_gcnllr_gx_donor}")
