# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "D:/B-Work/ESP/Software/v6.1/esp-idf/components/bootloader/subproject")
  file(MAKE_DIRECTORY "D:/B-Work/ESP/Software/v6.1/esp-idf/components/bootloader/subproject")
endif()
file(MAKE_DIRECTORY
  "E:/B-Poject/ESP/ESP32-P4-WIFI6-Touch-LCD-3.5-main/ESP32-P4-WIFI6-Touch-LCD-3.5-main/examples/esp-idf/12_esp32-p4-eye/build/bootloader"
  "E:/B-Poject/ESP/ESP32-P4-WIFI6-Touch-LCD-3.5-main/ESP32-P4-WIFI6-Touch-LCD-3.5-main/examples/esp-idf/12_esp32-p4-eye/build/bootloader-prefix"
  "E:/B-Poject/ESP/ESP32-P4-WIFI6-Touch-LCD-3.5-main/ESP32-P4-WIFI6-Touch-LCD-3.5-main/examples/esp-idf/12_esp32-p4-eye/build/bootloader-prefix/tmp"
  "E:/B-Poject/ESP/ESP32-P4-WIFI6-Touch-LCD-3.5-main/ESP32-P4-WIFI6-Touch-LCD-3.5-main/examples/esp-idf/12_esp32-p4-eye/build/bootloader-prefix/src/bootloader-stamp"
  "E:/B-Poject/ESP/ESP32-P4-WIFI6-Touch-LCD-3.5-main/ESP32-P4-WIFI6-Touch-LCD-3.5-main/examples/esp-idf/12_esp32-p4-eye/build/bootloader-prefix/src"
  "E:/B-Poject/ESP/ESP32-P4-WIFI6-Touch-LCD-3.5-main/ESP32-P4-WIFI6-Touch-LCD-3.5-main/examples/esp-idf/12_esp32-p4-eye/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "E:/B-Poject/ESP/ESP32-P4-WIFI6-Touch-LCD-3.5-main/ESP32-P4-WIFI6-Touch-LCD-3.5-main/examples/esp-idf/12_esp32-p4-eye/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "E:/B-Poject/ESP/ESP32-P4-WIFI6-Touch-LCD-3.5-main/ESP32-P4-WIFI6-Touch-LCD-3.5-main/examples/esp-idf/12_esp32-p4-eye/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
