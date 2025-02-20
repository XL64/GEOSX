#
# Specify list of tests
#

set( gtest_geosx_tests
     testXML.cpp
   )

set( gtest_tests_with_input
     testXMLFile.cpp
   )

set( input_files
     basic_input
     include_input
   )

set( multi_files
     multifile_input/fieldspec.xml
     multifile_input/mesh.xml
     multifile_input/outputs.xml
     multifile_input/solver.xml
   )

#
# Add gtest C++ based tests
#
set( dependencyList gtest optionparser )

if ( GEOSX_BUILD_SHARED_LIBS )
  set (dependencyList ${dependencyList} geosx_core)
else()
  set (dependencyList ${dependencyList} ${geosx_core_libs} )
endif()

if ( ENABLE_CUDA )
  set( dependencyList ${dependencyList} cuda )
endif()

# create test executables
foreach(test ${gtest_geosx_tests} ${gtest_tests_with_input})
    get_filename_component( test_name ${test} NAME_WE )
    blt_add_executable( NAME ${test_name}
                        SOURCES ${test}
                        OUTPUT_DIR ${TEST_OUTPUT_DIRECTORY}
                        DEPENDS_ON ${dependencyList} "-lz"
                      )
endforeach()

# add tests that don't require input files
foreach(test ${gtest_geosx_tests})
  get_filename_component( test_name ${test} NAME_WE )
  blt_add_test( NAME ${test_name}
                COMMAND ${test_name} )
endforeach()

# add tests that do require input files
foreach( test ${gtest_tests_with_input} )
  get_filename_component( test_name ${test} NAME_WE )

  # test a single input file
  foreach( input ${input_files} )
    blt_add_test( NAME ${test_name}_${input}
                  COMMAND ${test_name} -i ${CMAKE_CURRENT_LIST_DIR}/${input}.xml )
  endforeach()

  # test multiple input files
  foreach( file ${multi_files} )
    set( args ${args} -i ${CMAKE_CURRENT_LIST_DIR}/${file} )
  endforeach()
  blt_add_test( NAME ${test_name}_multiple_input
                COMMAND ${test_name} ${args} )
endforeach()