cmake_minimum_required(VERSION 3.16)
project(ishtaria-admin VERSION 0.1.7 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(TVISION REQUIRED IMPORTED_TARGET tvision)
pkg_check_modules(LIBPQ REQUIRED IMPORTED_TARGET libpq)
find_program(MSGFMT_EXECUTABLE msgfmt REQUIRED)

set(ISHTARIA_ADMIN_LOCALEDIR "${CMAKE_INSTALL_PREFIX}/share/locale" CACHE PATH "Catalog directory")

add_library(ishtariaadmin STATIC source/Db.cpp source/Ops.cpp)
target_include_directories(ishtariaadmin PUBLIC include)
target_link_libraries(ishtariaadmin PUBLIC PkgConfig::LIBPQ)

set(mo_file "${CMAKE_BINARY_DIR}/locale/cs/LC_MESSAGES/ishtaria-admin.mo")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/locale/cs/LC_MESSAGES")
add_custom_command(OUTPUT "${mo_file}"
    COMMAND "${MSGFMT_EXECUTABLE}" --check -o "${mo_file}" "${CMAKE_SOURCE_DIR}/po/cs.po"
    DEPENDS "${CMAKE_SOURCE_DIR}/po/cs.po" COMMENT "Compiling po/cs.po")
add_custom_target(translations ALL DEPENDS "${mo_file}")
install(FILES "${mo_file}" DESTINATION share/locale/cs/LC_MESSAGES RENAME ishtaria-admin.mo)

add_executable(ishtaria-admin source/main.cpp source/App.cpp source/i18n.cpp)
target_compile_definitions(ishtaria-admin PRIVATE ISHTARIA_ADMIN_LOCALEDIR="${ISHTARIA_ADMIN_LOCALEDIR}"
    ISHTARIA_ADMIN_VERSION="${PROJECT_VERSION}")
target_link_libraries(ishtaria-admin PRIVATE ishtariaadmin PkgConfig::TVISION)
install(TARGETS ishtaria-admin RUNTIME DESTINATION bin)
install(FILES debian/ishtaria-admin.1 DESTINATION share/man/man1)

enable_testing()
add_executable(ops_test tests/ops_test.cpp)
target_link_libraries(ops_test PRIVATE ishtariaadmin)
add_test(NAME ops COMMAND tests/run_ops_test.sh $<TARGET_FILE:ops_test>
    "${CMAKE_SOURCE_DIR}/../ishtaria-server/target/debug/ishtaria-server"
    "${CMAKE_SOURCE_DIR}/../ishtaria-worldgen/examples/planet-seed42-256.pgm"
    "${CMAKE_SOURCE_DIR}/../ishtaria-server/etc/server.toml"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(ops PROPERTIES SKIP_RETURN_CODE 77)
