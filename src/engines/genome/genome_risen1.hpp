#pragma once
// Risen (2009; the 2023 x64 release): the Genome module's route for it (genome_risen1.cpp). Its engine is the older
// generation (modular DLLs that export their C++ names, a constraint-solver camera): none of the ELEX / Risen 3 code
// shapes apply, so GenomeEngine hands every call to these functions in that game.
#include "engine.hpp"

#include <string>
#include <vector>

namespace gyro4all::genome::risen1 {

// <root>\bin\<exe> next to Game.dll, Engine.dll and SharedBase.dll, with <root>\data\compiled and <root>\data\ini.
bool detect(const std::wstring& exe_path);
// The game's own DLLs that import what the core hooks: Engine.dll (XInput, D3D9, the main message pump), GUI2.dll (its
// PeekMessageW), Game.dll (the gameplay code).
const wchar_t* module_prefix();
void configure();
std::vector<ViewInfo> views();
EngineFrame read_frame();
bool apply(double pitch, double yaw);
Engine::Recenter recenter_support(std::string& detail);
bool recenter(double fraction);
std::string status_scene();
void status_details();

}  // namespace gyro4all::genome::risen1
