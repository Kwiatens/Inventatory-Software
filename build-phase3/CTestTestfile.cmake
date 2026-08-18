# CMake generated Testfile for 
# Source directory: C:/Users/pawci/Documents/GitHub/Inventatory-Project/Inventatory-Software
# Build directory: C:/Users/pawci/Documents/GitHub/Inventatory-Project/Inventatory-Software/build-phase3
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test([=[inventatory_core]=] "C:/Users/pawci/Documents/GitHub/Inventatory-Project/Inventatory-Software/build-phase3/Debug/inventatory_tests.exe")
  set_tests_properties([=[inventatory_core]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/pawci/Documents/GitHub/Inventatory-Project/Inventatory-Software/CMakeLists.txt;152;add_test;C:/Users/pawci/Documents/GitHub/Inventatory-Project/Inventatory-Software/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test([=[inventatory_core]=] "C:/Users/pawci/Documents/GitHub/Inventatory-Project/Inventatory-Software/build-phase3/Release/inventatory_tests.exe")
  set_tests_properties([=[inventatory_core]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/pawci/Documents/GitHub/Inventatory-Project/Inventatory-Software/CMakeLists.txt;152;add_test;C:/Users/pawci/Documents/GitHub/Inventatory-Project/Inventatory-Software/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test([=[inventatory_core]=] "C:/Users/pawci/Documents/GitHub/Inventatory-Project/Inventatory-Software/build-phase3/MinSizeRel/inventatory_tests.exe")
  set_tests_properties([=[inventatory_core]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/pawci/Documents/GitHub/Inventatory-Project/Inventatory-Software/CMakeLists.txt;152;add_test;C:/Users/pawci/Documents/GitHub/Inventatory-Project/Inventatory-Software/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test([=[inventatory_core]=] "C:/Users/pawci/Documents/GitHub/Inventatory-Project/Inventatory-Software/build-phase3/RelWithDebInfo/inventatory_tests.exe")
  set_tests_properties([=[inventatory_core]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/pawci/Documents/GitHub/Inventatory-Project/Inventatory-Software/CMakeLists.txt;152;add_test;C:/Users/pawci/Documents/GitHub/Inventatory-Project/Inventatory-Software/CMakeLists.txt;0;")
else()
  add_test([=[inventatory_core]=] NOT_AVAILABLE)
endif()
subdirs("_deps/ftxui-build")
