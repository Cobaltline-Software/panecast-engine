# PanecastEngine: a headless AirPlay receiver process (GPL-3.0), built from UxPlay's lib/.
# It has no renderer: decrypted video and decoded audio go to the host app over a named pipe.
cmake_minimum_required( VERSION 3.13 )
project( panecast_engine C )

set( CMAKE_C_STANDARD 99 )
set( CMAKE_C_EXTENSIONS ON )
set( NO_MARCH_NATIVE ON )

set( UXPLAY_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/.. )
add_subdirectory( ${UXPLAY_ROOT}/lib/mdnsd    mdnsd )
add_subdirectory( ${UXPLAY_ROOT}/lib/llhttp   llhttp )
add_subdirectory( ${UXPLAY_ROOT}/lib/playfair playfair )
add_subdirectory( ${UXPLAY_ROOT}/lib          airplay )

if( NOT FFMPEG_PREFIX )
  message( FATAL_ERROR "Set -DFFMPEG_PREFIX to the minimal static FFmpeg install (see build-windows.sh)" )
endif()

add_executable( PanecastEngine panecast_engine.c )
target_include_directories( PanecastEngine PRIVATE ${UXPLAY_ROOT}/lib ${FFMPEG_PREFIX}/include )
target_compile_options( PanecastEngine PRIVATE -municode -Wall -O2 )
target_link_options( PanecastEngine PRIVATE -municode -static-libgcc )
target_link_libraries( PanecastEngine
  airplay
  ${FFMPEG_PREFIX}/lib/libavcodec.a
  ${FFMPEG_PREFIX}/lib/libswresample.a
  ${FFMPEG_PREFIX}/lib/libavutil.a
  bcrypt ws2_32 iphlpapi )
