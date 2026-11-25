# T-890 (Docs/design/59-visual-feedback.md §59.3 note "階段 V3 的做法"): the GPU capture renderer that
# `xtframe-editor --service` injects (Code/Editor/App/main.cpp), end to end over `--stdio`: a document with a
# custom() effect (Tests/xt/view_capture.xt and its .xtfx, copied to OUT) captured with renderer "gpu" is drawn
# by wgpu and its shader ran; with XT_CAPTURE_NO_GPU=1 "auto" is the software renderer and "gpu" is refused
# with the reason. Without a WebGPU adapter it prints `SKIP: no WebGPU adapter` and passes.
#   cmake -DEDITOR=<xtframe-editor> -DSOURCE=<Tests/xt> -DOUT=<dir> -P editor_service_gpu_capture.cmake
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
file(COPY "${SOURCE}/view_capture.xt" "${SOURCE}/view_capture_fx.xtfx" DESTINATION "${OUT}")
set(doc "${OUT}/view_capture.xt")

# request(<var> <no-gpu ON|OFF> <args JSON>): one `view.capture` over --stdio in a process of its own;
# <var> = the response line.
function(request var nogpu args)
    file(WRITE "${OUT}/in.jsonl" "{\"id\": 1, \"op\": \"view.capture\", \"args\": ${args}}\n")
    if(nogpu)
        set(env XT_CAPTURE_NO_GPU=1)
    else()
        set(env --unset=XT_CAPTURE_NO_GPU)
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${env} "${EDITOR}" --service --stdio "${doc}"
                    INPUT_FILE "${OUT}/in.jsonl" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
                    TIMEOUT 240)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "xtframe-editor --service --stdio: exit ${rc}: ${out}${err}")
    endif()
    string(STRIP "${out}" out)
    string(JSON type ERROR_VARIABLE jerr TYPE "${out}")
    if(jerr OR NOT type STREQUAL "OBJECT")
        message(FATAL_ERROR "view.capture ${args}: the response is not one JSON object (${jerr}): ${out}${err}")
    endif()
    set(${var} "${out}" PARENT_SCOPE)
endfunction()

# expect(<json> <expected> <path>...): the member at <path> as text equals <expected>.
function(expect json expected)
    string(JSON value ERROR_VARIABLE err GET "${json}" ${ARGN})
    if(err OR NOT "${value}" STREQUAL "${expected}")
        message(FATAL_ERROR "${ARGN}: '${value}' (${err}), expected '${expected}': ${json}")
    endif()
endfunction()

# 1. renderer "gpu": drawn by wgpu, the custom effect ran.
request(gpu OFF "{\"renderer\": \"gpu\", \"maxEdge\": 64}")
string(JSON ok GET "${gpu}" ok)
if(NOT ok)
    string(JSON message GET "${gpu}" error message)
    if(message MATCHES "no WebGPU adapter|cannot load wgpu_native")
        message("SKIP: no WebGPU adapter (${message})")
        return()
    endif()
    message(FATAL_ERROR "view.capture renderer gpu failed: ${gpu}")
endif()
expect("${gpu}" "wgpu" result stamp renderer)
string(JSON backend GET "${gpu}" result stamp backend)
if(backend STREQUAL "")
    message(FATAL_ERROR "the stamp names no backend: ${gpu}")
endif()
expect("${gpu}" "custom" result stamp effects 0 kind)
expect("${gpu}" "card" result stamp effects 0 layer)
expect("${gpu}" "ON" result stamp effects 0 executed)
message("gpu: renderer wgpu, backend '${backend}', the custom effect ran")

# 2. XT_CAPTURE_NO_GPU=1: "auto" is the software renderer, "gpu" is refused with the reason.
request(auto OFF "{\"renderer\": \"auto\", \"maxEdge\": 64}")
expect("${auto}" "wgpu" result stamp renderer)
request(auto_off ON "{\"renderer\": \"auto\", \"maxEdge\": 64}")
expect("${auto_off}" "ON" ok)
expect("${auto_off}" "sw" result stamp renderer)
expect("${auto_off}" "OFF" result stamp effects 0 executed)
request(gpu_off ON "{\"renderer\": \"gpu\", \"maxEdge\": 64}")
expect("${gpu_off}" "OFF" ok)
expect("${gpu_off}" "kUnsupported" error code)
string(JSON message GET "${gpu_off}" error message)
if(NOT message MATCHES "renderer \"gpu\" is not available: .+")
    message(FATAL_ERROR "the refusal does not say why: ${gpu_off}")
endif()
message("XT_CAPTURE_NO_GPU=1: auto -> sw; gpu -> kUnsupported (${message})")
message("editor service gpu capture: PASS")
