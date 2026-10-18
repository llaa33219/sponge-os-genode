TARGET = qt6_base.cmake_target

# Sponge OS: the compiler wrappers live in the Sponge repository. GENODE_DIR
# is not exported to per-target sub-makes, so derive the wrapper directory
# from this file's own location instead:
#   <genode>/repos/libports/src/qt6/base/ -> <genode>/repos/sponge/tool/
SPONGE_TOOL_DIR := $(abspath $(dir $(realpath $(call select_from_repositories,src/qt6/base/target.mk))))/../../../../sponge/tool

ifeq ($(CONTRIB_DIR),)
QT6_BASE_DIR       = $(call select_from_repositories,src/lib/qt6_base)
else
QT6_BASE_PORT_DIR := $(call select_from_ports,qt6_base)
QT6_BASE_DIR       = $(QT6_BASE_PORT_DIR)/src/lib/qt6_base
endif

LIBS = qt6_cmake ldso_so_support libc libm stdcxx qt6_component egl mesa base qoost

INSTALL_LIBS = lib/libQt6Concurrent.lib.so \
               lib/libQt6Core.lib.so \
               lib/libQt6Gui.lib.so \
               lib/libQt6Network.lib.so \
               lib/libQt6OpenGL.lib.so \
               lib/libQt6OpenGLWidgets.lib.so \
               lib/libQt6PrintSupport.lib.so \
               lib/libQt6Sql.lib.so \
               lib/libQt6Test.lib.so \
               lib/libQt6Widgets.lib.so \
               lib/libQt6Xml.lib.so \
               plugins/platforms/libqgenode.lib.so \
               plugins/imageformats/libqjpeg.lib.so \
               plugins/sqldrivers/libqsqlite.lib.so

BUILD_ARTIFACTS = $(notdir $(INSTALL_LIBS)) \
                  qt6_libqgenode.tar \
                  qt6_libqjpeg.tar \
                  qt6_libqsqlite.tar

cmake_prepared.tag: build_dependencies/lib/ld.lib.so

build: cmake_prepared.tag

	@#
	@# run cmake
	@#

	@# -DFEATURE_libstdcpp_assertions=OFF: prevent undefined reference to __glibcxx_assert_fail
	@# -DFEATURE_version_tagging=OFF:      version tags like @@Qt_6 are not supported by the abi_symbols tool
	@# -DFEATURE_intelcet=OFF:             not supported on ARM
	@# -DFEATURE_stack_protector=OFF:      not supported on Genode

	$(VERBOSE)cmake \
		-G "Unix Makefiles" \
		-DQT_SILENCE_CMAKE_GENERATOR_WARNING=ON \
		-DCMAKE_SYSTEM_NAME="Genode" \
		-DCMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES="" \
		-DCMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES="" \
		-DCMAKE_AR="$(AR)" \
		-DCMAKE_C_COMPILER="$(SPONGE_TOOL_DIR)/genode-x86-gcc-wrapper" \
		-DCMAKE_C_FLAGS="$(GENODE_CMAKE_CFLAGS) -DPCRE2_DISABLE_JIT -D__GNUCLIKE_BUILTIN_STDARG -include sys/cdefs.h" \
		-DCMAKE_CXX_COMPILER="$(SPONGE_TOOL_DIR)/genode-x86-g++-wrapper" \
		-DCMAKE_CXX_FLAGS="$(GENODE_CMAKE_CFLAGS) -D__GNUCLIKE_BUILTIN_STDARG -include sys/cdefs.h -I$(QT6_BASE_DIR)/src/3rdparty/md4c" \
		-DCMAKE_EXE_LINKER_FLAGS="$(GENODE_CMAKE_LFLAGS_APP)" \
		-DCMAKE_SHARED_LINKER_FLAGS="$(GENODE_CMAKE_LFLAGS_SHLIB)" \
		-DCMAKE_MODULE_LINKER_FLAGS="$(GENODE_CMAKE_LFLAGS_SHLIB)" \
		-DQT_QMAKE_TARGET_MKSPEC=$(QT_PLATFORM) \
		-DCMAKE_INSTALL_PREFIX=/qt \
		-DQT_HOST_PATH="$(QT_TOOLS_DIR)" \
		-DBUILD_WITH_PCH=OFF \
		-DCMAKE_BUILD_TYPE=RelWithDebInfo \
		-DQT_BUILD_EXAMPLES=OFF \
		-DQT_QPA_DEFAULT_PLATFORM=genode \
		-DQT_GENERATE_SBOM=OFF \
		-DFEATURE_cxx20=ON \
		-DFEATURE_relocatable=OFF \
		-DFEATURE_evdev=OFF \
		-DFEATURE_icu=OFF \
		-DFEATURE_system_harfbuzz=OFF \
		-DFEATURE_backtrace=OFF \
		-DFEATURE_glib=OFF \
		-DFEATURE_zstd=OFF \
		-DFEATURE_system_pcre2=OFF \
		-DFEATURE_system_zlib=OFF \
		-DFEATURE_dbus=OFF \
		-DFEATURE_libudev=OFF \
		-DFEATURE_opengl_desktop=ON \
		-DFEATURE_egl=ON \
		-DFEATURE_xkbcommon=OFF \
		-DFEATURE_networkinterface=OFF \
		-DFEATURE_vulkan=OFF \
		-DFEATURE_reduce_relocations=OFF \
		-DFEATURE_pkg_config=OFF \
		-DFEATURE_libstdcpp_assertions=OFF \
		-DFEATURE_version_tagging=OFF \
		-DFEATURE_intelcet=OFF \
		-DFEATURE_stack_protector=OFF \
		-DFEATURE_system_doubleconversion=OFF \
		-DFEATURE_system_md4c=OFF \
		-DFEATURE_textmarkdownreader=OFF \
		-DFEATURE_textmarkdownwriter=OFF \
		$(QT6_BASE_DIR) \
		$(QT6_OUTPUT_FILTER)

	@#
	@# build
	@#

	$(VERBOSE)$(MAKE) VERBOSE=$(MAKE_VERBOSE)

	@#
	@# install into local 'install' directory
	@#

	$(VERBOSE)$(MAKE) VERBOSE=$(MAKE_VERBOSE) DESTDIR=install install

	@#
	@# remove shared library existence checks since many libs are not
	@# present and not needed at build time
	@#

	$(VERBOSE)find $(CURDIR)/install/qt/lib/cmake -name "*.cmake" \
	          -exec sed -i "/list(APPEND _IMPORT_CHECK_TARGETS /d" {} \;

	@#
	@# strip libs and create symlinks in 'bin' and 'debug' directories
	@#

	$(VERBOSE)for LIB in $(INSTALL_LIBS); do \
		cd $(CURDIR)/install/qt/$$(dirname $${LIB}) && \
			$(OBJCOPY) --only-keep-debug $$(basename $${LIB}) $$(basename $${LIB}).debug && \
			$(STRIP) $$(basename $${LIB}) -o $$(basename $${LIB}).stripped && \
			$(OBJCOPY) --add-gnu-debuglink=$$(basename $${LIB}).debug $$(basename $${LIB}).stripped; \
		ln -sf $(CURDIR)/install/qt/$${LIB}.stripped $(PWD)/bin/$$(basename $${LIB}); \
		ln -sf $(CURDIR)/install/qt/$${LIB}.stripped $(PWD)/debug/$$(basename $${LIB}); \
		ln -sf $(CURDIR)/install/qt/$${LIB}.debug $(PWD)/debug/; \
	done

	@#
	@# create tar archives
	@#

	$(VERBOSE)tar chf $(PWD)/bin/qt6_libqgenode.tar $(TAR_OPT) --transform='s/\.stripped//' -C install qt/plugins/platforms/libqgenode.lib.so.stripped
	$(VERBOSE)tar chf $(PWD)/bin/qt6_libqjpeg.tar   $(TAR_OPT) --transform='s/\.stripped//' -C install qt/plugins/imageformats/libqjpeg.lib.so.stripped
	$(VERBOSE)tar chf $(PWD)/bin/qt6_libqsqlite.tar $(TAR_OPT) --transform='s/\.stripped//' -C install qt/plugins/sqldrivers/libqsqlite.lib.so.stripped

.PHONY: build

QT6_TARGET_DEPS = build
