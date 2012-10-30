#FIXME: we can't use ${ENGINE_SRC_ROOT_DIR} as it is defined after this file is parsed

set(lobbyTestAppSources
	TestConnection.cpp
	main.cpp
	../../../System/Net/Socket.cpp
	../../../System/SafeCStrings.c
)
add_executable(lobby-test EXCLUDE_FROM_ALL ${lobbyTestAppSources})
target_link_libraries(lobby-test lobby log log_sinkConsole)

