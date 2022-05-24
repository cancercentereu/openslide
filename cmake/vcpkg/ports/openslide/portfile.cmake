vcpkg_from_github(
  OUT_SOURCE_PATH SOURCE_PATH
  REPO Nico-Curti/openslide
  REF 834114112616e6983ef3bb5905199d1ee2b32363
  SHA512 02d4ddba2354b248d9627f0924836d65f0c47c0b9b60f8acb18d6265302ec5672416f072750d4ff458310d4b4fef1d63607533e75827e96defb10cdf076ee971
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
  FEATURES
    "python"  PYTHON_WRAP
    "java"    BUILD_JAVA
)

set(ENABLE_PYTHON OFF)
if("python" IN_LIST FEATURES)
  x_vcpkg_get_python_packages(PYTHON_VERSION "3" PACKAGES numpy cython OUT_PYTHON_VAR "PYTHON3")
  set(ENABLE_PYTHON ON)
  set(ENV{PYTHON} "${PYTHON3}")
endif()

set(BUILD_JAVA OFF)
if ("java" IN_LIST FEATURES)
  set(BUILD_JAVA ON)
endif()


vcpkg_cmake_configure(
  SOURCE_PATH "${SOURCE_PATH}"
  OPTIONS ${FEATURE_OPTIONS}
    -DINSTALL_BIN_DIR:STRING=bin
    -DINSTALL_LIB_DIR:STRING=lib
    -DCMAKE_VERBOSE_MAKEFILE:BOOL=OFF
    -DPYTHON_WRAP:BOOL=${ENABLE_PYTHON}
    -DBUILD_JAVA:BOOL=${BUILD_JAVA}
    -DBUILD_TEST:BOOL=OFF
    -DBUILD_DOCS:BOOL=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_fixup_pkgconfig()
file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
