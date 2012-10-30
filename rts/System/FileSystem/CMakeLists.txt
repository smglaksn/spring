
set(sources_engine_System_FileSystem
	IArchive.cpp
	ArchiveLoader.cpp
	BufferedArchive.cpp
	CacheDir.cpp
	DataDirLocater.cpp
	DataDirsAccess.cpp
	DirArchive.cpp
	FileFilter.cpp
	FileHandler.cpp
	FileSystem.cpp
	FileSystemAbstraction.cpp
	FileSystemInitializer.cpp
	PoolArchive.cpp
	SevenZipArchive.cpp
	SimpleParser.cpp
	VFSHandler.cpp
	ZipArchive.cpp
)
include_directories(${CMAKE_SOURCE_DIR}/rts)
include_directories(${CMAKE_SOURCE_DIR}/rts/lib)
### Assemble defines
#add_definitions(-DUSE_GML)
# USE_GML requires this
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mno-tls-direct-seg-refs")
# USE_GML_SIM requires USE_GML
#add_definitions(-DUSE_GML_SIM)
add_definitions(${PIC_FLAG})

add_library(engine-filesystem STATIC ${sources_engine_System_FileSystem})
target_link_libraries(engine-filesystem 7zip gml ${SPRING_MINIZIP_LIBRARY} ${ZLIB_LIBRARY} ${Boost_REGEX_LIBRARY} ${Boost_THREAD_LIBRARY})

