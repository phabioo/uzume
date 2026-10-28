# uzume_media_deps: INTERFACE target with the media dependencies on both platforms
# (LibDataChannel, FFmpeg avcodec/swscale/avutil, Opus).
#  Linux:   libdatachannel from scripts/fetch-libdatachannel.sh (CMAKE_PREFIX_PATH / LibDataChannel_ROOT),
#           FFmpeg and Opus from apt via pkg-config.
#  Windows: everything from vcpkg (client/vcpkg.json).
add_library(uzume_media_deps INTERFACE)

find_package(LibDataChannel CONFIG REQUIRED)
target_link_libraries(uzume_media_deps INTERFACE LibDataChannel::LibDataChannel)

if(WIN32)
  # vcpkg's FFMPEG_LIBRARIES does not reliably contain swscale: find every library explicitly.
  find_package(FFMPEG REQUIRED)
  set(FB_FFMPEG_WIN_LIBS "")
  foreach(_comp avcodec avutil swscale)
    find_library(FB_FFMPEG_${_comp}_LIB NAMES ${_comp}
      HINTS ${FFMPEG_LIBRARY_DIRS} ${FFMPEG_ROOT_DIR}/lib
      PATH_SUFFIXES lib)
    if(NOT FB_FFMPEG_${_comp}_LIB)
      message(FATAL_ERROR "FFmpeg library '${_comp}' not found (vcpkg ffmpeg)")
    endif()
    list(APPEND FB_FFMPEG_WIN_LIBS ${FB_FFMPEG_${_comp}_LIB})
  endforeach()
  find_package(Opus CONFIG REQUIRED)
  target_include_directories(uzume_media_deps INTERFACE ${FFMPEG_INCLUDE_DIRS})
  target_link_directories(uzume_media_deps INTERFACE ${FFMPEG_LIBRARY_DIRS})
  target_link_libraries(uzume_media_deps INTERFACE ${FB_FFMPEG_WIN_LIBS} Opus::opus)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FB_FFMPEG REQUIRED IMPORTED_TARGET libavcodec libswscale libavutil)
  pkg_check_modules(FB_OPUS REQUIRED IMPORTED_TARGET opus)
  target_link_libraries(uzume_media_deps INTERFACE PkgConfig::FB_FFMPEG PkgConfig::FB_OPUS)
endif()

uzume_add_test(media_deps tests/media_deps_test.cpp LIBS uzume_media_deps)

# Audio helper without media dependencies (also used by uzume_ui for the output resampler).
add_library(uzume_audioutil STATIC audioresampler.cpp)
target_include_directories(uzume_audioutil PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uzume_audioutil PUBLIC Qt6::Core)
uzume_warnings(uzume_audioutil)

# uzume_media: Session media path (ADR 0006 D5): H.264/Opus encode and decode, SessionHost (owner) and
# SessionViewer. Signaling is decoupled (Qt signals/slots), the Hub clients live in uzume_core (network/).
add_library(uzume_media STATIC
  bitratecontroller.cpp
  hoststate.cpp
  qualitytier.cpp
  videoencoder.cpp
  videodecoder.cpp
  cudadriver.cpp
  cudaglcapture.cpp
  d3d11glbridge.cpp
  d3d11encodecapture.cpp
  d3d11displayring.cpp
  opuscodec.cpp
  opusdepacketizer.cpp
  mediacaps.cpp
  sessionhost.cpp
  sessionviewer.cpp
  connectiontest.cpp
)
target_include_directories(uzume_media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uzume_media PUBLIC uzume_core uzume_audioutil uzume_media_deps Qt6::Gui)
target_link_libraries(uzume_media PRIVATE ${CMAKE_DL_LIBS})  # cudadriver.cpp: dlopen("libcuda.so.1") (ADR 0019)
if(WIN32)
  # GL-D3D11 bridge and display ring (ADR 0027): D3D11/DXGI import libs (System32 DLLs) and the WGL entry points.
  target_link_libraries(uzume_media PRIVATE d3d11 dxgi opengl32)
endif()
uzume_warnings(uzume_media)

uzume_add_test(media_viewer_reports tests/viewerreports_test.cpp LIBS uzume_media)
uzume_add_test(media_opus_depacketizer tests/opusdepacketizer_test.cpp LIBS uzume_media)
uzume_add_test(media_loopback tests/loopback_test.cpp LIBS uzume_media)
if(BUILD_TESTING)
  # Real-time encode and WebRTC timing (frame watchdog): alone under ctest -j, an oversubscribed CPU made it miss its deadlines.
  set_tests_properties(media_loopback PROPERTIES RUN_SERIAL TRUE)
endif()
uzume_add_test(media_bitrate tests/bitratecontroller_test.cpp LIBS uzume_media)
uzume_add_test(media_host_state tests/hoststate_test.cpp LIBS uzume_media)
uzume_add_test(media_quality_tier tests/qualitytier_test.cpp LIBS uzume_media)
uzume_add_test(media_rtcutil tests/rtcutil_test.cpp LIBS uzume_media)
uzume_add_test(media_connectiontest tests/connectiontest_test.cpp LIBS uzume_media)
uzume_add_test(media_stream_timestamp tests/stream_timestamp_test.cpp LIBS uzume_media)

# Zero-copy display (ADR 0027). media_display_slots: the pure slot state machine, every platform. media_d3d11_ring
# (Windows only): producer and consumer on two WARP devices; SKIP/77 when WARP lacks shared keyed-mutex resources, so it
# is in the Windows skip allow-list of ci.yml. The WGL interop itself is verified only on real GPUs.
uzume_add_test(media_display_slots tests/display_slots_test.cpp LIBS uzume_media)
if(BUILD_TESTING AND WIN32)
  uzume_add_test(media_d3d11_ring tests/d3d11_ring_test.cpp LIBS uzume_media d3d11 dxgi)
  set_tests_properties(media_d3d11_ring PROPERTIES SKIP_RETURN_CODE 77)
endif()

# GPU-direct encoding (ADR 0019): CUDA driver loader, CudaGlCapture and the CUDA input of VideoEncoder.
# media_cuda: pure (loader, error codes, encoder helpers). media_cuda_gl_capture: the real CudaGlCapture on a fake CUDA
# API (testutil/fake_cuda.h) and a GL 3.3 context; SKIP/77 without one, except on Linux CI where it must run.
# media_cuda_gl_hw: the same with the real driver; SKIP/77 without a GL 3.3 context, an NVIDIA GPU or a CUDA driver,
# also on CI. Run test_media_cuda_gl_hw by hand on a machine with an NVIDIA GPU.
if(BUILD_TESTING)
  uzume_add_test(media_cuda tests/cuda_test.cpp LIBS uzume_media)
  uzume_add_test(media_gpu_backends tests/gpu_backends_test.cpp LIBS uzume_media)  # ADR 0027 D6: encoder mapping, backend rules
  uzume_add_test(media_cuda_gl_capture tests/cuda_gl_capture_test.cpp LIBS uzume_media Qt6::Gui)
  set_tests_properties(media_cuda_gl_capture PROPERTIES SKIP_RETURN_CODE 77)
  uzume_add_test(media_cuda_gl_hw tests/cuda_gl_hw_test.cpp LIBS uzume_media Qt6::Gui)
  set_tests_properties(media_cuda_gl_hw PROPERTIES SKIP_RETURN_CODE 77)
endif()
