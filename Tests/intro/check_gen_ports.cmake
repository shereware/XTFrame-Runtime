# intro_gen_ports (T-596): the header `xtframe-pack --gen-ports cpp` wrote for hud.xt names the UI's
# namespace and its `hp` port, as Docs/intro/for-programmers.md says.
#   cmake -DFILE=<hud_ports.h> -P check_gen_ports.cmake
file(READ "${FILE}" text)
foreach(needle "namespace xtui" "namespace hud" "PortId hp = ")
    string(FIND "${text}" "${needle}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "intro_gen_ports: '${needle}' not in ${FILE}:\n${text}")
    endif()
endforeach()
message(STATUS "intro_gen_ports: ${FILE} declares xtui::hud::hp")
