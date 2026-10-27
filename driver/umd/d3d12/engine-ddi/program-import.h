// SPDX-License-Identifier: MIT
#pragma once
#include <string>

namespace engine_ddi::partial {
// Shader and partial-program exports share one name space. The destination is
// an unpublished state object: its caller discards it if any import fails.
template<typename Shaders, typename Programs>
bool import_named(Shaders& shaders, Programs& programs, const Shaders& source_shaders,
                  const Programs& source_programs, const std::wstring& source,
                  const std::wstring& destination) {
    if (shaders.find(destination) != shaders.end() || programs.find(destination) != programs.end()) return false;
    const auto shader = source_shaders.find(source);
    const auto program = source_programs.find(source);
    if ((shader != source_shaders.end()) == (program != source_programs.end())) return false;
    if (shader != source_shaders.end()) return shaders.emplace(destination, shader->second).second;
    return programs.emplace(destination, program->second).second;
}
}
