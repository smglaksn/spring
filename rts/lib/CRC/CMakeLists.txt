# Spring supplied CMake build file

SET(crcSources
		CRC.cpp
	)

ADD_LIBRARY(crc STATIC EXCLUDE_FROM_ALL ${crcSources})
SET_TARGET_PROPERTIES(crc PROPERTIES COMPILE_FLAGS "${PIC_FLAG}")

