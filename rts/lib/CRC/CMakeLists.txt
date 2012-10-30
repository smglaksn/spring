# Spring supplied CMake build file

SET(crcSources
		CRC.cpp
	)

ADD_LIBRARY(crc STATIC EXCLUDE_FROM_ALL ${crcSources})
TARGET_LINK_LIBRARIES(crc 7zip)
SET_TARGET_PROPERTIES(crc PROPERTIES COMPILE_FLAGS "${PIC_FLAG}")

