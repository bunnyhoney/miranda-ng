file(GLOB SOURCES "src/*.h" "src/libgadu/*.h" "res/*.rc"
"src/avatar.cpp"
"src/core.cpp"
"src/dialogs.cpp"
"src/dynstuff.cpp"
"src/filetransfer.cpp"
"src/stdafx.cpp"
"src/gg_proto.cpp"
"src/groupchat.cpp"
"src/icolib.cpp"
"src/image.cpp"
"src/import.cpp"
"src/keepalive.cpp"
"src/links.cpp"
"src/oauth.cpp"
"src/ownerinfo.cpp"
"src/popups.cpp"
"src/services.cpp"
"src/sessions.cpp"
"src/gg.cpp"
"src/token.cpp"
"src/userutils.cpp"
"src/libgadu/common.cpp"
"src/libgadu/dcc.cpp"
"src/libgadu/dcc7.cpp"
"src/libgadu/events.cpp"
"src/libgadu/http.cpp"
"src/libgadu/libgadu.cpp"
"src/libgadu/pthread.cpp"
"src/libgadu/pubdir.cpp"
"src/libgadu/pubdir50.cpp"
"src/libgadu/resolver.cpp"
"src/libgadu/sha1.cpp"
"src/libgadu/win32.cpp"
)
set(TARGET GG)
include_directories("src/libgadu")
include(${CMAKE_SOURCE_DIR}/cmake/plugin.cmake)
set_target_properties(${TARGET} PROPERTIES COMPILE_DEFINITIONS "GG_EXPORTS")
add_subdirectory(proto_gg)