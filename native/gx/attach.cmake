# Add only our library target to the pinned donor dependency build. The donor
# executable and direct-title entry point are not linked into this library.
set(_gcnllr_services_source "${CMAKE_CURRENT_LIST_DIR}")
cmake_language(DEFER CALL include "${_gcnllr_services_source}/CMakeLists.txt")
