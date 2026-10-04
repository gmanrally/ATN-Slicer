# CPack pre-build step (CPACK_PRE_BUILD_SCRIPTS): Authenticode-sign the staged
# install tree before NSIS packs it, so every .exe / .dll the installer lays down
# carries the publisher's signature. Files already signed by their own vendor are
# left alone, and so is the bundled ATNEngine.exe when the engine build signed it.
#
# Inert unless the ATN_SIGN environment variable names the signing script at
# package time -- a build without a signing identity packages exactly as before.

if (NOT DEFINED ENV{ATN_SIGN} OR "$ENV{ATN_SIGN}" STREQUAL "")
    message (STATUS "ATN: staged files not signed (ATN_SIGN is not set)")
    return ()
endif ()

message (STATUS "ATN: signing staged binaries in ${CPACK_TEMPORARY_DIRECTORY}")
execute_process (
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$ENV{ATN_SIGN}"
            "${CPACK_TEMPORARY_DIRECTORY}" -Recurse
    RESULT_VARIABLE _atn_sign_rc)
if (NOT _atn_sign_rc EQUAL 0)
    message (FATAL_ERROR "ATN: signing the staged files failed (exit ${_atn_sign_rc}) -- no installer was built")
endif ()
