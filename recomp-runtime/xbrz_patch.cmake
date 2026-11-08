# Patches PPSSPP's copy of xBRZ (ext/xbrz) to build outside PPSSPP: its
# header includes PPSSPP's platform configuration only to pick a stdint header.
function(recomp_patch_xbrz source destination)
    file(READ "${source}" text)
    foreach(from IN ITEMS
            "#include \"ppsspp_config.h\"\n"
            "#if PPSSPP_PLATFORM(IOS)\n#include <stdint.h>\n#else\n"
            "#include <cstdint> //uint32_t\n#endif\n")
        string(FIND "${text}" "${from}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR "xBRZ patch expected '${from}' in ${source}")
        endif()
    endforeach()
    string(REPLACE "#include \"ppsspp_config.h\"\n" "" text "${text}")
    string(REPLACE "#if PPSSPP_PLATFORM(IOS)\n#include <stdint.h>\n#else\n#include <cstdint> //uint32_t\n#endif\n"
        "#include <cstdint> //uint32_t\n" text "${text}")
    file(WRITE "${destination}" "${text}")
endfunction()
