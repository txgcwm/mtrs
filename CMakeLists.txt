cmake_minimum_required(VERSION 2.8.11)

include(CheckIncludeFile)
include(CheckFunctionExists)
include(CheckSymbolExists)
include(CheckIncludeFiles)
include(CheckStructHasMember)

project(mtrs)
set(projname mtrs)

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/builds/exe)
set(LIBRARY_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/builds/lib)
SET (FFMPEG_VERSION ffmpeg-2.1.1)


if (UNIX)
	SET (FFMPEG_LIB_PATH "${PROJECT_SOURCE_DIR}/sdk/${FFMPEG_VERSION}/centos")
	include_directories(${PROJECT_SOURCE_DIR}/sdk/${FFMPEG_VERSION}/include)
	link_directories(${FFMPEG_LIB_PATH})

	add_definitions( -D__STDC_LIMIT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_CONSTANT_MACROS)
	add_definitions( -D__USE_MISC)

	SET (CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -frtti -g -g3 -O0")
	SET (CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g -g3 -O0")
	
	SET (CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")
	SET (CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3")

	SET (CMAKE_CXX_STANDARD_LIBRARIES "-lpthread -lrt -lz ${CMAKE_CXX_STANDARD_LIBRARIES}")
	SET (CMAKE_C_STANDARD_LIBRARIES "-lpthread -lrt -lz ${CMAKE_C_STANDARD_LIBRARIES}")
	
	SET (CMAKE_CXX_STANDARD_LIBRARIES "${FFMPEG_LIB_PATH}/libavfilter.a ${FFMPEG_LIB_PATH}/libpostproc.a ${FFMPEG_LIB_PATH}/libswresample.a ${FFMPEG_LIB_PATH}/libavcodec.a ${FFMPEG_LIB_PATH}/libavformat.a ${FFMPEG_LIB_PATH}/libavutil.a ${FFMPEG_LIB_PATH}/libswscale.a ${CMAKE_CXX_STANDARD_LIBRARIES}")
	SET (CMAKE_C_STANDARD_LIBRARIES "${FFMPEG_LIB_PATH}/libavfilter.a ${FFMPEG_LIB_PATH}/libpostproc.a ${FFMPEG_LIB_PATH}/libswresample.a ${FFMPEG_LIB_PATH}/libavcodec.a ${FFMPEG_LIB_PATH}/libavformat.a ${FFMPEG_LIB_PATH}/libavutil.a ${FFMPEG_LIB_PATH}/libswscale.a ${CMAKE_C_STANDARD_LIBRARIES}")	
	
#	SET (CMAKE_CXX_STANDARD_LIBRARIES "-lavcodec -lavutil -lavformat ${CMAKE_CXX_STANDARD_LIBRARIES}")
#	SET (CMAKE_C_STANDARD_LIBRARIES "-lavcodec -lavutil -lavformat ${CMAKE_C_STANDARD_LIBRARIES}")
endif(UNIX)

if (WIN32)
endif (WIN32)

set(sources 
	main.cpp
	mpthread.cpp
	mpthread.h
	msgqueue.cpp
	msgqueue.h
	mtrs_audio.cpp
	mtrs_comm.h
	mtrs.cpp
	mtrs.h
	mtrs_internal.cpp
	mtrs_internal.h
	mtrs_video.cpp
 )
 
#link_directories(${PROJECT_SOURCE_DIR}/builds/lib)
#include_directories(BEFORE ${PROJECT_SOURCE_DIR}/Source/curl-7.28.0/include)
 
add_executable(${projname} ${sources})

#SET (Libraries
#	avcodec
#	avfilter
#	avutil
#	avformat
#)
#target_link_libraries(${projname} ${Libraries})

#SET (Libraries
#	Base
#	libcurl
#)
#target_link_libraries(${projname} ${Libraries})
