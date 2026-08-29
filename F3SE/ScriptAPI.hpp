#pragma once

#include <string_view>

#include "Kore.hpp"

/*
* ScriptAPI.hpp simply contains a few definitions that other source files might need
*/

namespace ScriptAPI
{
    void HandleYields();

    void ExecuteScript(Kore::lua_State* L, std::string_view Script);
}