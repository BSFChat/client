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
