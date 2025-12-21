add_library (
    common STATIC
    alternatePlayerstate.h
    cdefs.h
    cmd.cpp
    cmd.h
    cm_load.cpp
    cm_local.h
    cm_patch.cpp
    cm_patch.h
    cm_polylib.cpp
    cm_polylib.h
    cm_public.h
    cm_test.cpp
    cm_trace.cpp
    common.cpp
    crypto.cpp
    crypto.h
    cvar.cpp
    cvar.h
    files.cpp
    files.h
    huffman.cpp
    huffman.h
    ioapi.cpp
    ioapi.h
    json.cpp
    json.h
    md4.cpp
    md4.h
    md5.cpp
    msg.cpp
    msg.h
    net_chan.cpp
    net.h
    net_ip.cpp
    parse.cpp
    puff.cpp
    puff.h
    q3_lauxlib.cpp
    q3_lauxlib.h
    qcommon.h
    qfiles.h
    q_math.c
    q_platform.h
    q_shared.c
    q_shared.h
    surfaceflags.h
    unzip.cpp
    unzip.h
    vm.cpp
    vm.h
    vm_interpreted.cpp
    vm_js.js
    vm_local.h
    vm_x86.cpp

)

include_directories( ${RESTCLIENT_INCLUDES_DIR} )

set ( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14" )
