set(CMAKE_CXX_COMPILER "C:/Program Files/LLVM/bin/clang-cl.exe"
    CACHE FILEPATH "clang-cl compiler")

if(NOT CMAKE_RC_COMPILER)
    file(GLOB rc_candidates
        "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/rc.exe")
    list(SORT rc_candidates COMPARE NATURAL ORDER DESCENDING)
    list(GET rc_candidates 0 latest_rc)
    set(CMAKE_RC_COMPILER "${latest_rc}" CACHE FILEPATH "Windows resource compiler")
endif()
