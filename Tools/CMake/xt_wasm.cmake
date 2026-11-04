# Emscripten helpers (Docs/design/52-web-wasm.md §52.2 "Node 版 CLI 旗標"; Docs/tasks/T-520.md item 1).
#
# xt_wasm_node_cli(<target> <export_name>): links <target> as a Node.js command-line program, the T-503
# flags of Docs/design/24-design-import.md §24.13 made a function. One <target>.js + <target>.wasm pair,
# copied by Tools/Scripts/build_wasm.sh into Code/Tools/wasm/dist/ and loaded by Code/Tools/wasm/bin/<target>.js
# (bin/_run.js: creates the module and calls main(); T-579), or Code/Tools/import-wasm/bin/ for xtframe-import:
#   NODERAWFS          reads and writes the host file system directly (real paths, no virtual FS staging);
#   ENVIRONMENT=node   Node only (the browser module of §52.3 has its own flags);
#   MODULARIZE + EXPORT_NAME + INVOKE_RUN=0 + callMain
#                      the wrapper creates the module and calls main() itself;
#   EXIT_RUNTIME       exit(n) and main()'s return value reach the wrapper;
#   ALLOW_MEMORY_GROWTH, STACK_SIZE=4MB
#                      the depth limits of 24 §24.12 were measured against a 1 MB stack (Emscripten's
#                      default is 64 KB).
# No threads (-sUSE_PTHREADS stays off, §52.2 "執行緒"). Call it only under if(EMSCRIPTEN).
function(xt_wasm_node_cli target export_name)
    target_link_options(${target} PRIVATE
        -sNODERAWFS=1 -sENVIRONMENT=node -sMODULARIZE=1 -sEXPORT_NAME=${export_name}
        -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=4MB -sEXIT_RUNTIME=1 -sINVOKE_RUN=0
        -sEXPORTED_RUNTIME_METHODS=callMain)
endfunction()
