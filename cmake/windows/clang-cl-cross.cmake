# Toolchain para compilar el plugin de Windows x64 desde macOS/Linux con
# clang-cl + lld-link y el SDK de Windows/CRT de MSVC descargado con xwin.
#   -DXWIN_DIR=<ruta a la salida de "xwin splat">
#   -DLLVM_BIN=<carpeta con clang-cl, llvm-lib, llvm-rc, llvm-mt>
#   -DLLD_BIN=<carpeta con lld-link>
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(CMAKE_SYSTEM_VERSION 10.0)

set(CMAKE_C_COMPILER "${LLVM_BIN}/clang-cl")
set(CMAKE_CXX_COMPILER "${LLVM_BIN}/clang-cl")
set(CMAKE_LINKER "${LLD_BIN}/lld-link")
set(CMAKE_AR "${LLVM_BIN}/llvm-lib")
set(CMAKE_RC_COMPILER "${LLVM_BIN}/llvm-rc")
set(CMAKE_MT "${LLVM_BIN}/llvm-mt")
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)

set(_inc "/imsvc${XWIN_DIR}/crt/include /imsvc${XWIN_DIR}/sdk/include/ucrt /imsvc${XWIN_DIR}/sdk/include/um /imsvc${XWIN_DIR}/sdk/include/shared")
set(CMAKE_C_FLAGS_INIT "--target=x86_64-pc-windows-msvc -fms-compatibility-version=19.40 ${_inc}")
set(CMAKE_CXX_FLAGS_INIT "--target=x86_64-pc-windows-msvc -fms-compatibility-version=19.40 ${_inc} /FI${CMAKE_CURRENT_LIST_DIR}/clang-cl-compat.h")

set(_lib "/libpath:${XWIN_DIR}/crt/lib/x86_64 /libpath:${XWIN_DIR}/sdk/lib/um/x86_64 /libpath:${XWIN_DIR}/sdk/lib/ucrt/x86_64")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_lib}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_lib}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_lib}")

set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES XWIN_DIR LLVM_BIN LLD_BIN)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# El SDK de xwin no incluye CRT de depuración: siempre /MD (Release).
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
