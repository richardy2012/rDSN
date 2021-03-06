add_definitions(-DDSN_IN_CORE)
set(DSN_IN_CORE "TRUE")
set(MY_PROJ_NAME dsn.core.tests)

# Source files under CURRENT project directory will be automatically included.
# You can manually set MY_PROJ_SRC to include source files under other directories.
set(MY_PROJ_SRC "")

# Search mode for source files under CURRENT project directory?
# "GLOB_RECURSE" for recursive search
# "GLOB" for non-recursive search
set(MY_SRC_SEARCH_MODE "GLOB")

set(MY_PROJ_INC_PATH
    ${GTEST_INCLUDE_DIR} 
    ../core ../tools/common ../tools/simulator ../tools/hpc ../tools/nfs 
    ../../dev/cpp
    )
    
set(MY_BOOST_PACKAGES system)

set(MY_PROJ_LIBS gtest
                 dsn.dev.cpp.core.use
                 dsn.corelib
                 dsn.cli
                 dsn.dev.utility
)

set(MY_PROJ_LIB_PATH "${GTEST_LIB_DIR}")

# Extra files that will be installed
set(MY_BINPLACES "${CMAKE_CURRENT_SOURCE_DIR}/config-bad-section.ini"
                 "${CMAKE_CURRENT_SOURCE_DIR}/config-dup-key.ini"
                 "${CMAKE_CURRENT_SOURCE_DIR}/config-dup-section.ini"
                 "${CMAKE_CURRENT_SOURCE_DIR}/config-empty.ini"
                 "${CMAKE_CURRENT_SOURCE_DIR}/config-no-key.ini"
                 "${CMAKE_CURRENT_SOURCE_DIR}/config-no-section.ini"
                 "${CMAKE_CURRENT_SOURCE_DIR}/config-null-section.ini"
                 "${CMAKE_CURRENT_SOURCE_DIR}/config-sample.ini"
                 "${CMAKE_CURRENT_SOURCE_DIR}/config-test-corrupt-message.ini"
                 "${CMAKE_CURRENT_SOURCE_DIR}/config-test.ini"
                 "${CMAKE_CURRENT_SOURCE_DIR}/config-test-fastrun.ini"
                 "${CMAKE_CURRENT_SOURCE_DIR}/config-test-posix-aio.ini"
                 "${CMAKE_CURRENT_SOURCE_DIR}/config-test-sim.ini"
                 "${CMAKE_CURRENT_SOURCE_DIR}/config-unmatch-section.ini"
                 "${CMAKE_CURRENT_SOURCE_DIR}/command.txt"
                 "${CMAKE_CURRENT_SOURCE_DIR}/nfs_test_file1"
                 "${CMAKE_CURRENT_SOURCE_DIR}/nfs_test_file2"
                 "${CMAKE_CURRENT_SOURCE_DIR}/run.sh"
                 "${CMAKE_CURRENT_SOURCE_DIR}/clear.sh"
                 "${CMAKE_CURRENT_SOURCE_DIR}/clear.cmd"
                 "${CMAKE_CURRENT_SOURCE_DIR}/gtest.filter"
)

dsn_add_executable()
add_dependencies(${MY_PROJ_NAME} googletest)
