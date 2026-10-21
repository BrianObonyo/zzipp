set(CMAKE_AUTOMOC ON)

qt_add_executable(zzipp-gui WIN32 MACOSX_BUNDLE main.cpp MainWindow.cpp MainWindow.hpp)
target_link_libraries(zzipp-gui PRIVATE zzipp_core Qt6::Widgets)
target_compile_definitions(zzipp-gui PRIVATE ZZIPP_VERSION="${PROJECT_VERSION}")
install(TARGETS zzipp-gui BUNDLE DESTINATION . RUNTIME DESTINATION bin)

if(ZZIPP_BUILD_TESTS)
  add_test(NAME gui_selftest
           COMMAND zzipp-gui --selftest ${CMAKE_CURRENT_BINARY_DIR}/selftest)
  set_tests_properties(gui_selftest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 240)
endif()
