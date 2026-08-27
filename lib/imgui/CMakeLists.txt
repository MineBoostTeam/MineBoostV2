# Dear ImGui (vendored) -- see MINEBOOST_README.md for what's here and why.
#
# Built as its own small static library, same convention as lib/sha256,
# lib/tiniergltf, etc. -- kept deliberately independent of IrrlichtMt/the
# rest of the engine (only depends on OpenGL, resolved via its own bundled
# loader) so it can be dropped in/updated/dropped again without touching
# anything else.

project(imgui CXX)

add_library(imgui STATIC
	${CMAKE_CURRENT_SOURCE_DIR}/imgui.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/imgui_draw.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/imgui_tables.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/imgui_widgets.cpp
	# ImGui::ShowDemoWindow() -- used by MineBoostV2 as a build-verification
	# aid while this integration is new. See the note on this in
	# MINEBOOST_README.md if considering dropping it later.
	${CMAKE_CURRENT_SOURCE_DIR}/imgui_demo.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

# INTERFACE include dirs: anything linking against this target (just
# EngineCommon/the client target, see src/CMakeLists.txt) automatically
# picks up both directories without needing its own include_directories()
# call.
target_include_directories(imgui PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/backends
)

# Deliberately no IMGUI_IMPL_OPENGL_LOADER_* definitions here: leaving
# them all unset lets imgui_impl_opengl3.cpp fall through to its own
# bundled loader (backends/imgui_impl_opengl3_loader.h), independent of
# MineBoostV2's own OpenGLProcedures GL wrapper (irr/include/mt_opengl.h)
# -- see MINEBOOST_README.md for why that's the right choice here.

# Android has no desktop GL headers to load against -- IrrlichtMt talks to
# it over OpenGL ES there (see irr/src/COGLES*), so ImGui's OpenGL3
# backend needs to be told to include the GLES3 headers/API instead of
# trying (and failing) to use its desktop-GL loader. Every other
# currently-supported platform (Windows/Linux/macOS) uses desktop GL, so
# this is Android-only.
if(ANDROID)
	target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_ES3)
endif()

# Same C++ standard as the rest of the project (see CMAKE_CXX_STANDARD in
# the root CMakeLists.txt) -- ImGui only actually requires C++11, this
# just avoids a second standard silently in play within the same build.
set_target_properties(imgui PROPERTIES
	CXX_STANDARD 17
	CXX_STANDARD_REQUIRED TRUE
)
