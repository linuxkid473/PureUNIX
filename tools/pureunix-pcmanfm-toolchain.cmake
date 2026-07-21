# CMake cross-compilation toolchain file for cross-building pcmanfm-qt
# itself against PureUnix's own cross toolchain. Reuses
# tools/pureunix-libfmqt-toolchain.cmake verbatim (same real
# i686-elf-gcc/g++ + newlib + libstdc++ + Qt6/GLib/MenuCache/libexif/
# lxqt-build-tools setup that already successfully cross-built libfm-qt6.a)
# and layers on top of it the one new real dependency pcmanfm-qt itself
# needs directly: libfm-qt6.a (its own real, exported CMake package
# config, third_party/libfm-qt/i686-elf/share/cmake/fm-qt6/). Qt6DBus/
# LayerShellQt/XCB are all deliberately NOT vendored anywhere and made
# optional by third_party/pcmanfm-qt/patches/0001 — this toolchain file
# doesn't need to (and can't) point at them.
include("${CMAKE_CURRENT_LIST_DIR}/pureunix-libfmqt-toolchain.cmake")

set(LIBFMQT_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/libfm-qt/i686-elf")

list(APPEND CMAKE_FIND_ROOT_PATH "${LIBFMQT_DIR}")
list(APPEND CMAKE_PREFIX_PATH "${LIBFMQT_DIR}")

# tools/pureunix-qt-toolchain.cmake's own CMAKE_EXE_LINKER_FLAGS_INIT only
# ever had to satisfy CMake's internal try_compile() checks before now
# (CMAKE_TRY_COMPILE_TARGET_TYPE is STATIC_LIBRARY there, and every real
# consumer so far — Qt6 itself, libfm-qt6.a — built only static libraries,
# never linked a real final executable through this toolchain file).
# pcmanfm-qt is the first one that does, and i686-elf-gcc was built
# --without-headers (see that file's own comment) — it never
# automatically links libc/libstdc++ the way a normal hosted GCC would.
# Every other real executable in this repo's own top-level Makefile
# already spells out the exact same real incantation by hand:
# `-Wl,--start-group -lstdc++ -lsupc++ -lc -lm -Wl,--end-group -lgcc`
# (see e.g. the NEWLIB_CXX_ELFS build rule). CMAKE_EXE_LINKER_FLAGS_INIT
# is placed before the target's own object files/link libraries on the
# real link command line, so only the opening `--start-group` goes here;
# CMAKE_CXX_STANDARD_LIBRARIES (a real, standard CMake variable
# documented to be appended AFTER a target's own target_link_libraries()
# output) supplies the closing half, so the whole real dependency chain
# (Qt6::Widgets, fm-qt6 and everything it transitively links) ends up
# sandwiched inside one real --start-group/--end-group pair exactly like
# the Makefile's own version.
string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " -Wl,--start-group")

# PUREUNIX_QPA_STATIC_LIB (Makefile's own libpureunix-qpa.a target,
# archiving the exact same 4 object files user/qtwindowtest.elf/
# qtwidgetstest.elf link directly): pcmanfm-qt's real upstream main()
# (pcmanfm/pcmanfm.cpp) has no idea PureUnix or its QPA plugin exist, so
# without this it links fine but fails at runtime with Qt's real
# "no Qt platform plugin could be initialized" error, exactly like
# qtwindowtest/qtwidgetstest would without their own Q_IMPORT_PLUGIN(...)
# + "-platform pureunix" argv (see patches/0002-static-qpa-plugin.patch,
# which adds the same two things to pcmanfm.cpp). Placed inside the same
# --start-group/--end-group pair as -lstdc++ etc. below so its own
# QtGui/QtCore symbol references resolve against whatever's already in
# the group (Qt6::Gui et al, pulled in by the target's own
# target_link_libraries()) regardless of link order.
set(PUREUNIX_QPA_STATIC_LIB "${CMAKE_CURRENT_LIST_DIR}/../build/user/qpa_pureunix/libpureunix-qpa.a")
set(CMAKE_CXX_STANDARD_LIBRARIES "${PUREUNIX_QPA_STATIC_LIB} -lstdc++ -lsupc++ -lc -lm -Wl,--end-group -lgcc")
