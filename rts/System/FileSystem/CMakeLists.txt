SET(sources_engine_System_FileSystem
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
add_definitions(${PIC_FLAG})

add_library(engine-filesystem STATIC ${sources_engine_System_FileSystem})
target_link_libraries(engine-filesystem 7zip ${SPRING_MINIZIP_LIBRARY} ${ZLIB_LIBRARY} ${Boost_REGEX_LIBRARY} ${Boost_THREAD_LIBRARY})

