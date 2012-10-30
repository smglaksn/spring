SET(sources_engine_System_Log
	Backend.cpp
	DefaultFilter.cpp
	DefaultFormatter.cpp
	FramePrefixer.cpp
	LogSinkHandler.cpp
	LogUtil.c
	../../System/SafeCStrings.c
)

SET(sources_engine_System_Log_sinkStream
	StreamSink.cpp
)
SET(sources_engine_System_Log_sinkConsole
	ConsoleSink.cpp
)
SET(sources_engine_System_Log_sinkFile
	FileSink.cpp
)
SET(sources_engine_System_Log_sinkOutputDebugString
	OutputDebugStringSink.cpp
)
add_definitions(${PIC_FLAG})

add_library(log STATIC ${sources_engine_System_Log})
add_library(log_sinkStream STATIC ${sources_engine_System_Log_sinkStream})
add_library(log_sinkConsole STATIC ${sources_engine_System_Log_sinkConsole})
add_library(log_sinkFile STATIC ${sources_engine_System_Log_sinkFile})

add_library(log_sinkOutputDebugString STATIC ${sources_engine_System_Log_sinkOutputDebugString})
target_link_libraries(log_sinkOutputDebugString log)

