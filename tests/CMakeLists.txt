find_package(Qt6 REQUIRED COMPONENTS Test)

qt_add_executable(test_models
    test_models.cpp
    ${CMAKE_SOURCE_DIR}/src/model/MessageModel.cpp
    ${CMAKE_SOURCE_DIR}/src/model/RoomListModel.cpp
    ${CMAKE_SOURCE_DIR}/src/model/MemberListModel.cpp
    ${CMAKE_SOURCE_DIR}/src/model/ServerListModel.cpp
    ${CMAKE_SOURCE_DIR}/src/util/MarkdownParser.cpp
)

target_include_directories(test_models PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(test_models PRIVATE
    bsfchat_protocol
    Qt6::Core Qt6::Test
)

add_test(NAME test_models COMMAND test_models)

# Codec round-trip tests — only meaningful where a software H.264
# backend exists (openh264: macOS/Linux desktop).
if(BSFCHAT_ENABLE_VOICE AND BSFCHAT_HAVE_OPENH264)
    qt_add_executable(test_video_codec
        test_video_codec.cpp
        ${CMAKE_SOURCE_DIR}/src/voice/video/VideoCodecFactory.cpp
        ${CMAKE_SOURCE_DIR}/src/voice/video/FrameConverter.cpp
        ${CMAKE_SOURCE_DIR}/src/voice/video/OpenH264Encoder.cpp
        ${CMAKE_SOURCE_DIR}/src/voice/video/OpenH264Decoder.cpp
    )
    target_include_directories(test_video_codec PRIVATE
        ${CMAKE_SOURCE_DIR}/src ${libyuv_SOURCE_DIR}/include)
    target_compile_definitions(test_video_codec PRIVATE BSFCHAT_HAVE_OPENH264)
    target_link_libraries(test_video_codec PRIVATE
        Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Test
        yuv openh264::openh264
    )
    # Include the VideoToolbox backend on macOS so the round-trip and
    # cross-backend (VT ↔ openh264, i.e. mac ↔ linux) tests cover the
    # hardware path.
    if(APPLE AND NOT IOS)
        target_sources(test_video_codec PRIVATE
            ${CMAKE_SOURCE_DIR}/src/voice/video/MacVTEncoder.mm
            ${CMAKE_SOURCE_DIR}/src/voice/video/MacVTDecoder.mm
        )
        target_compile_definitions(test_video_codec PRIVATE BSFCHAT_HAVE_VIDEOTOOLBOX)
        target_link_libraries(test_video_codec PRIVATE
            "-framework VideoToolbox" "-framework CoreMedia" "-framework CoreVideo")
    endif()
    add_test(NAME test_video_codec COMMAND test_video_codec)
endif()
