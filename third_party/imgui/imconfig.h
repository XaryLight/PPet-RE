//-----------------------------------------------------------------------------
// USER CONFIGURATION FILE FOR IMGUI
//-----------------------------------------------------------------------------
// This file is included at the top of imgui.h and can be used to configure
// the library at compile time.
//
// You can use this file to:
//
//  1) Disable some features of ImGui, to reduce the code size, or
//  2) Change the rendering API used by ImGui (see below), or
//  3) Configure any ImGui functionality (e.g. disable branch folding).
//
// This file is designed to be included multiple times, with different
// IMGUI_USER_CONFIG settings, to allow different configurations within the
// same program. If you want to include this file without any IMPLEMENTATION
// section, simply don't provide an IMGUI_USER_CONFIG macro.
//-----------------------------------------------------------------------------

#pragma once

//----------------------------------------
// [OPTIONAL] CUSTOMIZE IMGUI WITH YOUR OWN FUNCTIONS
//----------------------------------------
// You can use this section to replace the default rendering backend, or
// provide your own font loader.
//
// Uncomment and define the macro to opt in:
//
//   #define IMGUI_USER_CONFIG "my_config.h"
//
//--------------------------------------------------------------------------

//-----------------------------
// [OPTIONAL] USE DYNAMICALLY LOADED GL BINDINGS
//-----------------------------
// The ImGui OpenGL3 backend can use a dynamically loaded GL loader instead of
// relying on the system's gl.h / opengl32.lib symbols. This project already
// uses GLEW for its own rendering, but ImGui's backend ships with its own
// minimal loader (imgl3w) which is enabled by default and works fine.
//--------------------------------------------------------------------------

//-----------------------------
// [OPTIONAL] PLATFORM DEFINITIONS
//-----------------------------

//---------------------------------------------------------------------------
// End of file
//---------------------------------------------------------------------------
