if (UNIX AND NOT APPLE)
    set (TESTS_ADDITIONAL_LIBS
        -lX11
    )
endif()

# Enable the test-only HTTP seam (plan.md 2.4).
#
# This whole directory is only added when TOGGL_BUILD_TESTS is ON (see the top
# level CMakeLists.txt), and the definition is additionally refused for a
# production build - so what ships never contains the seam. See the long
# comment at the top of src/https_client.h for what it compiles to.
#
# It has to go on TogglDesktopLibrary and be PUBLIC: TogglClient::GetInstance()
# is an inline function that context.cc inlines, so the library and the test
# executables must agree on whether the seam exists. Defining it on the test
# executables alone would be an ODR violation and the seam would silently not
# apply to anything inside the library.
if(NOT TOGGL_PRODUCTION_BUILD)
    target_compile_definitions(TogglDesktopLibrary PUBLIC TOGGL_TEST_HTTP_SEAM=1)
endif()

include_directories(
    ${TOGGLDESKTOPLIBRARY_INCLUDE_DIR}
    ${OAUTH_INCLUDE_DIR}
    ${BUGSNAG_INCLUDE_DIR}
    ${LUA_INCLUDE_DIRS}
    ${JSONCPP_INCLUDE_DIRS}
    ${POCO_INCLUDE_DIRS}
    ${GTEST_INCLUDE_DIRS}
)

set(API_TEST_SOURCE_FILES
    test_data.cc
    toggl_api_test.cc
)
add_executable(TogglApiTest ${API_TEST_SOURCE_FILES})
target_link_libraries(TogglApiTest PRIVATE
    TogglDesktopLibrary
    ${JSONCPP_LIBRARIES}
    ${LUA_LIBRARIES}
    PocoCrypto PocoDataSQLite PocoNetSSL PocoFoundation
    gtest_main gtest
    ${TESTS_ADDITIONAL_LIBS}
)

set(APP_TEST_SOURCE_FILES
    test_data.cc
    app_test.cc
    http_seam_test.cc
)
add_executable(TogglAppTest ${APP_TEST_SOURCE_FILES})
# PocoNet is listed explicitly: http_seam_test.cc drives a real Context, whose
# headers inline Poco::Net symbols, and the linker will not pull them in
# transitively through PocoNetSSL.
target_link_libraries(TogglAppTest PRIVATE
    TogglDesktopLibrary
    ${JSONCPP_LIBRARIES}
    ${LUA_LIBRARIES}
    PocoCrypto PocoDataSQLite PocoNetSSL PocoNet PocoFoundation
    gtest_main gtest
    ${TESTS_ADDITIONAL_LIBS}
)

set(ONLINE_TEST_SOURCE_FILES
    online_test.cc
    online_test_app.cpp
    online_test_dispatcher.cc
    online_test_model.cc
)
add_executable(TogglOnlineTest ${ONLINE_TEST_SOURCE_FILES})
target_link_libraries(TogglOnlineTest PRIVATE
    TogglDesktopLibrary
    ${JSONCPP_LIBRARIES}
    ${LUA_LIBRARIES}
    PocoCrypto PocoDataSQLite PocoNetSSL PocoFoundation
    gtest_main gtest
    ${TESTS_ADDITIONAL_LIBS}
)
