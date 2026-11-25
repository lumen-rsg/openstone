cmake_minimum_required(VERSION 3.24)
project(OpenStone VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(OPENSTONE_BUILD_DESKTOP "Build the Qt Quick desktop harness" ON)
option(OPENSTONE_ENABLE_LLVM_ORC "Compile DSP Code using LLVM ORC" OFF)
set(OPENSTONE_FLOWSTONE_ROOT "" CACHE PATH "Optional FlowStone build 5483 root for bundled import tests")
option(OPENSTONE_BUILD_ASSEMBLER_AUDIT "Build the compile-only Assembler audit probe" OFF)

add_library(openstone_editor STATIC
    src/editor/Area.cpp
    src/editor/AudioControls.cpp
    src/editor/Settings.cpp
    src/editor/StateMachine.cpp
    src/editor/SystemActions.cpp
    src/editor/EmbeddedFonts.cpp
    src/editor/HttpRequest.cpp
    src/editor/Network.cpp
    src/editor/Media.cpp
    src/editor/Sequencer.cpp
    src/editor/RubyCode.cpp
    src/editor/SfzParser.cpp
    src/editor/PanelUtilities.cpp
    src/editor/Keyboard.cpp
    src/editor/PresentationControls.cpp
    src/editor/FileUtilities.cpp
    src/editor/ArrayTools.cpp
    src/editor/Brushes.cpp
    src/editor/BitmapOperations.cpp
    src/editor/Bus.cpp
    src/editor/Drawing.cpp
    src/editor/DspCode.cpp
    src/editor/CodeSource.cpp
    src/editor/DropList.cpp
    src/editor/DropFiles.cpp
    src/editor/DragFiles.cpp
    src/editor/EditControl.cpp
    src/editor/GeneralProcessing.cpp
    src/editor/MouseMessages.cpp
    src/editor/ModuleSync.cpp
    src/editor/ModulePorts.cpp
    src/editor/MakeModule.cpp
    src/editor/MemoryOperations.cpp
    src/editor/WaveFile.cpp
    src/editor/NoteRouting.cpp
    src/editor/MidiOperations.cpp
    src/editor/PluginHost.cpp
    src/editor/PresetText.cpp
    src/editor/LegacyPreset.cpp
    src/editor/MidiDevices.cpp
    src/editor/SerialDevices.cpp
    src/editor/Session.cpp
    src/editor/EditHistory.cpp
    src/editor/Schematic.cpp
    src/editor/StringOperations.cpp
    src/editor/StringConversions.cpp
    src/editor/Tick.cpp
    src/editor/View.cpp
    src/editor/ViewEffects.cpp
    src/editor/ViewLoops.cpp
    src/editor/ViewRegion.cpp
    src/editor/Window.cpp
    src/editor/Wireless.cpp
)
target_include_directories(openstone_editor PUBLIC src/editor)

add_library(openstone_green STATIC
    src/green/Arithmetic.cpp
    src/green/ArrayOperations.cpp
    src/green/Fourier.cpp
    src/green/Memory.cpp
    src/green/Runtime.cpp
    src/green/Value.cpp
    src/yellow/Bitmap.cpp
    src/yellow/Color.cpp
    src/yellow/ColorMatrix.cpp
    src/yellow/Font.cpp
    src/yellow/MouseCursor.cpp
    src/yellow/PaintTool.cpp
    src/yellow/StringFormat.cpp
)
target_include_directories(openstone_green PUBLIC src)
find_package(Iconv REQUIRED)
target_link_libraries(openstone_editor PUBLIC openstone_green PRIVATE Iconv::Iconv)

add_subdirectory(third_party/softfloat)

add_library(openstone_stream STATIC
    src/stream/AudioProcessor.cpp
    src/stream/DspCode.cpp
    src/stream/DspInterpreter.cpp
    src/stream/DspX87.cpp
    src/stream/Runtime.cpp
    src/stream/DeZipper.cpp
    src/stream/StreamCounter.cpp
    src/stream/Oscillator.cpp
    src/stream/PolyRenderer.cpp
    src/stream/VoiceBank.cpp
    src/stream/Wave.cpp
    src/stream/PolyRuntime.cpp
)
target_include_directories(openstone_stream PUBLIC src)
target_link_libraries(openstone_stream PUBLIC openstone_green PRIVATE openstone_softfloat)

target_compile_definitions(openstone_stream PRIVATE OPENSTONE_HAS_LLVM_ORC=$<BOOL:${OPENSTONE_ENABLE_LLVM_ORC}>)
if(OPENSTONE_ENABLE_LLVM_ORC)
    enable_language(C) # LLVM's CMake package probes its C dependencies.
    find_package(LLVM 21.1 REQUIRED CONFIG)
    target_sources(openstone_stream PRIVATE src/stream/DspOrc.cpp)
    target_include_directories(openstone_stream SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    if(TARGET LLVM)
        target_link_libraries(openstone_stream PRIVATE LLVM)
    else()
        llvm_map_components_to_libnames(OPENSTONE_LLVM_LIBS orcjit native passes)
        target_link_libraries(openstone_stream PRIVATE ${OPENSTONE_LLVM_LIBS})
    endif()
endif()

target_link_libraries(openstone_editor PUBLIC openstone_stream)

if(OPENSTONE_BUILD_ASSEMBLER_AUDIT)
    add_executable(openstone_assembler_probe tools/AssemblerCompileProbe.cpp)
    target_link_libraries(openstone_assembler_probe PRIVATE openstone_stream)
endif()

include(CTest)
if(BUILD_TESTING)
    if(OPENSTONE_BUILD_ASSEMBLER_AUDIT)
        find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
        add_test(NAME assembler_compile_audit COMMAND ${Python3_EXECUTABLE}
            -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/AssemblerAuditTest.py
            --probe $<TARGET_FILE:openstone_assembler_probe>)
    endif()
    add_executable(openstone_green_tests tests/green/RuntimeTest.cpp)
    target_link_libraries(openstone_green_tests PRIVATE openstone_green)
    add_test(NAME green_runtime COMMAND openstone_green_tests)
    add_executable(openstone_schematic_tests tests/editor/SchematicTest.cpp)
    target_link_libraries(openstone_schematic_tests PRIVATE openstone_editor)
    target_compile_definitions(openstone_schematic_tests PRIVATE OPENSTONE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME schematic COMMAND openstone_schematic_tests)
    add_executable(openstone_stream_tests tests/stream/RuntimeTest.cpp)
    target_link_libraries(openstone_stream_tests PRIVATE openstone_stream)
    add_test(NAME stream_runtime COMMAND openstone_stream_tests)
    add_executable(openstone_assembler_sse2_tests tests/stream/AssemblerSse2Test.cpp)
    target_link_libraries(openstone_assembler_sse2_tests PRIVATE openstone_stream)
    target_compile_definitions(openstone_assembler_sse2_tests PRIVATE OPENSTONE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME assembler_sse2 COMMAND openstone_assembler_sse2_tests)
    add_executable(openstone_assembler_xmm_entry_tests tests/stream/AssemblerXmmEntryTest.cpp)
    target_link_libraries(openstone_assembler_xmm_entry_tests PRIVATE openstone_stream)
    add_test(NAME assembler_xmm_entry COMMAND openstone_assembler_xmm_entry_tests)
    add_executable(openstone_assembler_raw_xmm_tests tests/stream/AssemblerRawXmmTest.cpp)
    target_link_libraries(openstone_assembler_raw_xmm_tests PRIVATE openstone_stream)
    target_compile_definitions(openstone_assembler_raw_xmm_tests PRIVATE OPENSTONE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME assembler_raw_xmm COMMAND openstone_assembler_raw_xmm_tests)
    add_executable(openstone_assembler_comiss_tests tests/stream/AssemblerComissTest.cpp)
    target_link_libraries(openstone_assembler_comiss_tests PRIVATE openstone_stream)
    add_test(NAME assembler_comiss COMMAND openstone_assembler_comiss_tests)
    add_executable(openstone_assembler_integer_flags_tests tests/stream/AssemblerIntegerFlagsTest.cpp)
    target_link_libraries(openstone_assembler_integer_flags_tests PRIVATE openstone_stream)
    add_test(NAME assembler_integer_flags COMMAND openstone_assembler_integer_flags_tests)
    add_executable(openstone_assembler_engine_constant_tests tests/stream/AssemblerEngineConstantTest.cpp)
    target_link_libraries(openstone_assembler_engine_constant_tests PRIVATE openstone_stream)
    target_compile_definitions(openstone_assembler_engine_constant_tests PRIVATE OPENSTONE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME assembler_engine_constants COMMAND openstone_assembler_engine_constant_tests)
    add_executable(openstone_assembler_stage_directive_tests tests/stream/AssemblerStageDirectiveTest.cpp)
    target_link_libraries(openstone_assembler_stage_directive_tests PRIVATE openstone_stream)
    target_compile_definitions(openstone_assembler_stage_directive_tests PRIVATE OPENSTONE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME assembler_stage_directives COMMAND openstone_assembler_stage_directive_tests)
    add_executable(openstone_assembler_entry_binding_tests tests/stream/AssemblerEntryBindingTest.cpp)
    target_link_libraries(openstone_assembler_entry_binding_tests PRIVATE openstone_stream)
    target_compile_definitions(openstone_assembler_entry_binding_tests PRIVATE OPENSTONE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME assembler_entry_bindings COMMAND openstone_assembler_entry_binding_tests)
    add_executable(openstone_assembler_frame_binding_tests tests/stream/AssemblerFrameBindingTest.cpp)
    target_link_libraries(openstone_assembler_frame_binding_tests PRIVATE openstone_stream)
    target_compile_definitions(openstone_assembler_frame_binding_tests PRIVATE OPENSTONE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME assembler_frame_bindings COMMAND openstone_assembler_frame_binding_tests)
    add_executable(openstone_assembler_engine_pointer_tests tests/stream/AssemblerEnginePointerTest.cpp)
    target_link_libraries(openstone_assembler_engine_pointer_tests PRIVATE openstone_stream)
    target_compile_definitions(openstone_assembler_engine_pointer_tests PRIVATE OPENSTONE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME assembler_engine_pointers COMMAND openstone_assembler_engine_pointer_tests)
    add_executable(openstone_assembler_engine_table_tests tests/stream/AssemblerEngineTableTest.cpp)
    target_link_libraries(openstone_assembler_engine_table_tests PRIVATE openstone_stream)
    target_compile_definitions(openstone_assembler_engine_table_tests PRIVATE OPENSTONE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME assembler_engine_tables COMMAND openstone_assembler_engine_table_tests)
    add_executable(openstone_assembler_loop_flag_tests tests/stream/AssemblerLoopFlagTest.cpp)
    target_link_libraries(openstone_assembler_loop_flag_tests PRIVATE openstone_stream)
    target_compile_definitions(openstone_assembler_loop_flag_tests PRIVATE OPENSTONE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME assembler_loop_flags COMMAND openstone_assembler_loop_flag_tests)
    add_executable(openstone_assembler_input_write_tests tests/stream/AssemblerInputWriteTest.cpp)
    target_link_libraries(openstone_assembler_input_write_tests PRIVATE openstone_stream)
    target_compile_definitions(openstone_assembler_input_write_tests PRIVATE OPENSTONE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME assembler_input_writes COMMAND openstone_assembler_input_write_tests)
    add_executable(openstone_assembler_compact_stage_tests tests/stream/AssemblerCompactStageTest.cpp)
    target_link_libraries(openstone_assembler_compact_stage_tests PRIVATE openstone_stream)
    target_compile_definitions(openstone_assembler_compact_stage_tests PRIVATE OPENSTONE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME assembler_compact_stages COMMAND openstone_assembler_compact_stage_tests)
    add_executable(openstone_assembler_descriptor_alias_tests tests/stream/AssemblerDescriptorAliasTest.cpp)
    target_link_libraries(openstone_assembler_descriptor_alias_tests PRIVATE openstone_stream)
    target_compile_definitions(openstone_assembler_descriptor_alias_tests PRIVATE OPENSTONE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME assembler_descriptor_aliases COMMAND openstone_assembler_descriptor_alias_tests)
    add_executable(openstone_assembler_mem_limit_tests tests/stream/AssemblerMemLimitTest.cpp)
    target_link_libraries(openstone_assembler_mem_limit_tests PRIVATE openstone_stream)
    target_compile_definitions(openstone_assembler_mem_limit_tests PRIVATE OPENSTONE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME assembler_mem_limit COMMAND openstone_assembler_mem_limit_tests)
    add_executable(openstone_assembler_descriptor_packet_tests tests/stream/AssemblerDescriptorPacketTest.cpp)
    target_link_libraries(openstone_assembler_descriptor_packet_tests PRIVATE openstone_stream)
    target_compile_definitions(openstone_assembler_descriptor_packet_tests PRIVATE OPENSTONE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME assembler_descriptor_packets COMMAND openstone_assembler_descriptor_packet_tests)
    add_executable(openstone_assembler_x87_memory_view_tests tests/stream/AssemblerX87MemoryViewTest.cpp)
    target_link_libraries(openstone_assembler_x87_memory_view_tests PRIVATE openstone_stream)
    target_compile_definitions(openstone_assembler_x87_memory_view_tests PRIVATE OPENSTONE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME assembler_x87_memory_views COMMAND openstone_assembler_x87_memory_view_tests)
    add_executable(openstone_assembler_extra_gpr_tests tests/stream/AssemblerExtraGprTest.cpp)
    target_link_libraries(openstone_assembler_extra_gpr_tests PRIVATE openstone_stream)
    target_compile_definitions(openstone_assembler_extra_gpr_tests PRIVATE OPENSTONE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME assembler_extra_gprs COMMAND openstone_assembler_extra_gpr_tests)
    add_executable(openstone_assembler_byte_divide_tests tests/stream/AssemblerByteDivideTest.cpp)
    target_link_libraries(openstone_assembler_byte_divide_tests PRIVATE openstone_stream)
    add_test(NAME assembler_byte_divide COMMAND openstone_assembler_byte_divide_tests)
    add_executable(openstone_assembler_byte_exchange_tests tests/stream/AssemblerByteExchangeTest.cpp)
    target_link_libraries(openstone_assembler_byte_exchange_tests PRIVATE openstone_stream)
    add_test(NAME assembler_byte_exchange COMMAND openstone_assembler_byte_exchange_tests)
    add_executable(openstone_assembler_byte_shift_tests tests/stream/AssemblerByteShiftTest.cpp)
    target_link_libraries(openstone_assembler_byte_shift_tests PRIVATE openstone_stream)
    add_test(NAME assembler_byte_shift COMMAND openstone_assembler_byte_shift_tests)
    add_executable(openstone_assembler_byte_logical_tests tests/stream/AssemblerByteLogicalTest.cpp)
    target_link_libraries(openstone_assembler_byte_logical_tests PRIVATE openstone_stream)
    add_test(NAME assembler_byte_logical COMMAND openstone_assembler_byte_logical_tests)
    add_executable(openstone_assembler_byte_step_tests tests/stream/AssemblerByteStepTest.cpp)
    target_link_libraries(openstone_assembler_byte_step_tests PRIVATE openstone_stream)
    add_test(NAME assembler_byte_step COMMAND openstone_assembler_byte_step_tests)
    add_executable(openstone_assembler_byte_arithmetic_tests tests/stream/AssemblerByteArithmeticTest.cpp)
    target_link_libraries(openstone_assembler_byte_arithmetic_tests PRIVATE openstone_stream)
    add_test(NAME assembler_byte_arithmetic COMMAND openstone_assembler_byte_arithmetic_tests)
    add_executable(openstone_assembler_byte_register_tests tests/stream/AssemblerByteRegisterTest.cpp)
    target_link_libraries(openstone_assembler_byte_register_tests PRIVATE openstone_stream)
    target_compile_definitions(openstone_assembler_byte_register_tests PRIVATE OPENSTONE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME assembler_byte_register COMMAND openstone_assembler_byte_register_tests)
    add_executable(openstone_assembler_divide_tests tests/stream/AssemblerDivideTest.cpp)
    target_link_libraries(openstone_assembler_divide_tests PRIVATE openstone_stream)
    add_test(NAME assembler_divide COMMAND openstone_assembler_divide_tests)
    add_executable(openstone_assembler_exchange_tests tests/stream/AssemblerExchangeTest.cpp)
    target_link_libraries(openstone_assembler_exchange_tests PRIVATE openstone_stream)
    add_test(NAME assembler_exchange COMMAND openstone_assembler_exchange_tests)
    add_executable(openstone_assembler_scalar_convert_tests tests/stream/AssemblerScalarConvertTest.cpp)
    target_link_libraries(openstone_assembler_scalar_convert_tests PRIVATE openstone_stream)
    add_test(NAME assembler_scalar_convert COMMAND openstone_assembler_scalar_convert_tests)
    add_executable(openstone_dsp_declaration_tests tests/stream/DspDeclarationTest.cpp)
    target_link_libraries(openstone_dsp_declaration_tests PRIVATE openstone_stream)
    target_compile_definitions(openstone_dsp_declaration_tests PRIVATE OPENSTONE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME dsp_declarations COMMAND openstone_dsp_declaration_tests)
    add_executable(openstone_poly_tests tests/stream/PolyRuntimeTest.cpp)
    target_link_libraries(openstone_poly_tests PRIVATE openstone_stream)
    add_test(NAME poly_runtime COMMAND openstone_poly_tests)
endif()

if(OPENSTONE_BUILD_DESKTOP)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Quick QuickControls2 ShaderTools Svg SerialPort Network Multimedia)
    if(NOT TARGET Qt6::GuiPrivate)
        find_package(Qt6 6.8 REQUIRED COMPONENTS GuiPrivate)
    endif()
    qt_standard_project_setup(REQUIRES 6.8)
    find_package(ZLIB REQUIRED)
    add_library(openstone_flowstone_file src/desktop/FlowStoneFile.cpp)
    target_include_directories(openstone_flowstone_file PUBLIC src/desktop)
    target_link_libraries(openstone_flowstone_file PUBLIC openstone_editor Qt6::Core PRIVATE ZLIB::ZLIB Qt6::Gui)

    find_package(PortAudio CONFIG QUIET NAMES portaudio PortAudio)
    if(TARGET portaudio_static)
        set(OPENSTONE_PORTAUDIO portaudio_static)
    elseif(TARGET portaudio)
        set(OPENSTONE_PORTAUDIO portaudio)
    elseif(TARGET PortAudio::PortAudio)
        set(OPENSTONE_PORTAUDIO PortAudio::PortAudio)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(PORTAUDIO REQUIRED IMPORTED_TARGET portaudio-2.0)
        set(OPENSTONE_PORTAUDIO PkgConfig::PORTAUDIO)
    endif()

    find_package(RtMidi CONFIG QUIET)
    if(TARGET RtMidi::rtmidi)
        set(OPENSTONE_RTMIDI RtMidi::rtmidi)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(RTMIDI REQUIRED IMPORTED_TARGET rtmidi>=5)
        set(OPENSTONE_RTMIDI PkgConfig::RTMIDI)
    endif()

    qt_add_executable(OpenStone WIN32 MACOSX_BUNDLE src/desktop/main.cpp)
    if(APPLE)
        set(OPENSTONE_LICENSE_DIR "$<TARGET_BUNDLE_CONTENT_DIR:OpenStone>/Resources/licenses")
    else()
        set(OPENSTONE_LICENSE_DIR "$<TARGET_FILE_DIR:OpenStone>/licenses")
    endif()
    add_custom_command(TARGET OpenStone POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OPENSTONE_LICENSE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/softfloat/LICENSE.txt"
            "${OPENSTONE_LICENSE_DIR}/SoftFloat-LICENSE.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
            "${OPENSTONE_LICENSE_DIR}/OpenStone-GPL-3.0.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
            "${OPENSTONE_LICENSE_DIR}/THIRD_PARTY_NOTICES.md"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/OFL-Nunito.txt"
            "${OPENSTONE_LICENSE_DIR}/Nunito-OFL.txt"
        VERBATIM)

    target_link_libraries(OpenStone PRIVATE openstone_flowstone_file openstone_editor Qt6::Quick Qt6::QuickControls2 Qt6::GuiPrivate Qt6::Svg Qt6::SerialPort Qt6::Network Qt6::Multimedia
        ${OPENSTONE_PORTAUDIO} ${OPENSTONE_RTMIDI})
    if(APPLE)
        find_library(OPENSTONE_COREMIDI CoreMIDI REQUIRED)
        find_library(OPENSTONE_IOKIT IOKit REQUIRED)
        find_library(OPENSTONE_COREFOUNDATION CoreFoundation REQUIRED)
        target_link_libraries(OpenStone PRIVATE ${OPENSTONE_COREMIDI} ${OPENSTONE_IOKIT} ${OPENSTONE_COREFOUNDATION})
    endif()
    if(WIN32)
        include(CheckCXXSourceCompiles)
        set(CMAKE_REQUIRED_LIBRARIES ${OPENSTONE_PORTAUDIO})
        check_cxx_source_compiles("
            #include <pa_asio.h>
            int main() { return PaAsio_ShowControlPanel(0, nullptr); }
        " OPENSTONE_PORTAUDIO_ASIO)
        unset(CMAKE_REQUIRED_LIBRARIES)
        if(OPENSTONE_PORTAUDIO_ASIO)
            target_compile_definitions(OpenStone PRIVATE OPENSTONE_PORTAUDIO_ASIO)
        endif()
        target_link_libraries(OpenStone PRIVATE shell32)
    endif()
    set_target_properties(OpenStone PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER org.openstone.editor
        MACOSX_BUNDLE_BUNDLE_NAME OpenStone
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/src/desktop/Info.plist.in"
    )
    if(APPLE AND TARGET Qt6::QDarwinMicrophonePermissionPlugin)
        qt_import_plugins(OpenStone INCLUDE Qt6::QDarwinMicrophonePermissionPlugin)
    endif()

    add_library(openstone_panel_radial_brush STATIC src/desktop/PanelRadialBrush.cpp)
    target_link_libraries(openstone_panel_radial_brush PUBLIC openstone_editor Qt6::Gui)
    target_link_libraries(OpenStone PRIVATE openstone_panel_radial_brush)

    add_library(openstone_panel_text_layout STATIC src/desktop/PanelTextLayout.cpp)
    target_link_libraries(openstone_panel_text_layout PUBLIC openstone_editor Qt6::Gui)

    add_library(openstone_ruby_process STATIC src/desktop/RubyProcess.cpp)
    target_link_libraries(openstone_ruby_process PUBLIC openstone_editor Qt6::Core PRIVATE openstone_panel_text_layout)
    target_link_libraries(OpenStone PRIVATE openstone_ruby_process openstone_panel_text_layout)

    set(OPENSTONE_QML_FILES
        src/desktop/qml/Main.qml
        src/desktop/qml/controls/ColorPicker.qml
        src/desktop/qml/controls/ColorSwatch.qml
        src/desktop/qml/controls/ConnectorTypePicker.qml
        src/desktop/qml/controls/DspSwatch.qml
        src/desktop/qml/controls/EditorMenu.qml
        src/desktop/qml/controls/EditorMenuItem.qml
        src/desktop/qml/controls/FlatButton.qml
        src/desktop/qml/controls/OpenStoneMark.qml
        src/desktop/qml/controls/SignalConnectorIcon.qml
        src/desktop/qml/editors/DspCodeEditor.qml
        src/desktop/qml/editors/RubyCodeEditor.qml
        src/desktop/qml/library/LibraryBrowser.qml
        src/desktop/qml/library/LibraryConnector.qml
        src/desktop/qml/library/LibraryPreview.qml
        src/desktop/qml/panels/DropListMenu.qml
        src/desktop/qml/panels/DropListPopup.qml
        src/desktop/qml/panels/FrontPanel.qml
        src/desktop/qml/panels/PanelDrawing.qml
        src/desktop/qml/panels/PanelEditControl.qml
        src/desktop/qml/panels/PanelFileDrop.qml
        src/desktop/qml/panels/PanelPopupList.qml
        src/desktop/qml/panels/PanelViewEffect.qml
        src/desktop/qml/panels/ViewClip.qml
        src/desktop/qml/schematic/Breadcrumb.qml
        src/desktop/qml/schematic/GreenNode.qml
        src/desktop/qml/schematic/GreenWire.qml
        src/desktop/qml/schematic/NodeActionBar.qml
        src/desktop/qml/schematic/NodeContextMenu.qml
        src/desktop/qml/schematic/PrimitiveDialogs.qml
        src/desktop/qml/schematic/SchematicCanvas.qml
        src/desktop/qml/schematic/SchematicWindows.qml
        src/desktop/qml/theme/Theme.qml
    )
    # Keep component URLs stable within the OpenStone module, including the
    # recursive panel loaders and the FrontPanel URL used by bitmap rendering.
    foreach(openstone_qml_file IN LISTS OPENSTONE_QML_FILES)
        get_filename_component(openstone_qml_name "${openstone_qml_file}" NAME)
        set_source_files_properties("${openstone_qml_file}" PROPERTIES
            QT_RESOURCE_ALIAS "${openstone_qml_name}")
    endforeach()
    set_source_files_properties(src/desktop/qml/theme/Theme.qml PROPERTIES
        QT_QML_SINGLETON_TYPE TRUE)
    qt_add_qml_module(OpenStone
        URI OpenStone
        OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/qml/OpenStone"
        QML_FILES ${OPENSTONE_QML_FILES}
        RESOURCES
            assets/fonts/Nunito.ttf
            assets/fonts/OFL-Nunito.txt
        SOURCES
            src/desktop/AudioDevice.cpp
            src/desktop/AudioDevice.hpp
            src/desktop/MidiDevice.cpp
            src/desktop/MidiDevice.hpp
            src/desktop/BitmapIO.cpp
            src/desktop/BitmapIO.hpp
            src/desktop/CanvasGrid.cpp
            src/desktop/CanvasGrid.hpp
            src/desktop/DspSyntax.cpp
            src/desktop/DspSyntax.hpp
            src/desktop/LibraryModel.hpp
            src/desktop/PanelBitmap.cpp
            src/desktop/PanelBitmap.hpp
            src/desktop/PanelBrushFill.cpp
            src/desktop/PanelBrushFill.hpp
            src/desktop/PanelText.cpp
            src/desktop/PanelText.hpp
            src/desktop/SchematicModel.cpp
            src/desktop/KeyboardInput.cpp
            src/desktop/SchematicModel.hpp
            src/desktop/SchematicFile.cpp
            src/desktop/SchematicFile.hpp
            src/desktop/SerialDevice.cpp
            src/desktop/SerialDevice.hpp
            src/desktop/SettingsStore.cpp
            src/desktop/SettingsStore.hpp
            src/desktop/HttpTransfer.cpp
            src/desktop/Network.cpp
            src/desktop/Media.cpp
            src/desktop/RubySound.cpp
            src/desktop/HttpTransfer.hpp
            src/desktop/DiskSerial.cpp
            src/desktop/SystemInfo.cpp
            src/desktop/SystemInfo.hpp
    )
    qt_add_resources(OpenStone openstone_ruby_support
        PREFIX "/openstone" BASE "src/desktop" FILES src/desktop/ruby_node.rb)
    qt_add_shaders(OpenStone openstone_view_effect_shaders
        PREFIX "/openstone"
        FILES shaders/view-gaussian.frag shaders/view-composite.frag shaders/view-clip.frag
    )
    target_include_directories(OpenStone PRIVATE src/desktop)

    if(BUILD_TESTING)
        add_executable(openstone_settings_tests tests/desktop/SettingsTest.cpp src/desktop/SettingsStore.cpp)
        target_include_directories(openstone_settings_tests PRIVATE src/desktop)
        target_link_libraries(openstone_settings_tests PRIVATE openstone_editor Qt6::Core)
        add_test(NAME settings_persistence COMMAND openstone_settings_tests)
        add_executable(openstone_flowstone_file_tests tests/desktop/FlowStoneFileTest.cpp src/desktop/SchematicFile.cpp)
        target_link_libraries(openstone_flowstone_file_tests PRIVATE openstone_flowstone_file openstone_panel_radial_brush Qt6::Core ZLIB::ZLIB)
        add_test(NAME flowstone_file COMMAND openstone_flowstone_file_tests)
        if(OPENSTONE_FLOWSTONE_ROOT)
            find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
            add_test(NAME flowstone_bundled_import COMMAND ${Python3_EXECUTABLE}
                -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/FlowStoneBundledImportTest.py
                --flowstone-root ${OPENSTONE_FLOWSTONE_ROOT}
                --probe $<TARGET_FILE:openstone_flowstone_file_tests>
                --ruby-probe $<TARGET_FILE:openstone_ruby_tests>
                --output ${CMAKE_CURRENT_BINARY_DIR}/flowstone-bundled-import)
        endif()
        add_executable(openstone_sequencer_project_tests tests/desktop/SequencerProjectTest.cpp src/desktop/SchematicFile.cpp)
        target_include_directories(openstone_sequencer_project_tests PRIVATE src/editor src/desktop)
        target_link_libraries(openstone_sequencer_project_tests PRIVATE openstone_flowstone_file openstone_editor Qt6::Core)
        add_test(NAME sequencer_project COMMAND openstone_sequencer_project_tests)
        add_executable(openstone_ruby_tests tests/desktop/RubyCodeTest.cpp src/desktop/SchematicFile.cpp)
        target_include_directories(openstone_ruby_tests PRIVATE src/desktop)
        target_link_libraries(openstone_ruby_tests PRIVATE openstone_flowstone_file openstone_ruby_process Qt6::Gui)
        qt_add_resources(openstone_ruby_tests ruby_test_support
            PREFIX "/openstone" BASE "src/desktop" FILES src/desktop/ruby_node.rb)
        add_test(NAME ruby_runtime COMMAND openstone_ruby_tests)
        set_tests_properties(ruby_runtime PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()
