cmake_minimum_required(VERSION 4.2)
project(FTLRecomp VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "FTLRecomp requires an x64 toolchain; use the windows-vs2026 preset")
endif()
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64|x64)$" AND NOT CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
  message(FATAL_ERROR "This project currently targets x86-64")
endif()
set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "Supported configurations" FORCE)
set(CMAKE_CXX_STANDARD 23)
if("cxx_std_26" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
  set(CMAKE_CXX_STANDARD 26)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# No C++ modules are used. Avoid unnecessary scanning and make the compilation
# database usable by clang-tidy without first building module-map response files.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/SDL3-3.2.26 EXCLUDE_FROM_ALL)
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/freetype-VER-2-13-3 EXCLUDE_FROM_ALL)
add_library(pugixml STATIC third_party/pugixml.cpp)
target_include_directories(pugixml SYSTEM PUBLIC third_party)
add_library(ftl_codecs STATIC src/image_decode.cpp src/vorbis_decode.c)
target_include_directories(ftl_codecs SYSTEM PRIVATE third_party)
add_executable(FTLRecomp
  src/main.cpp
  src/log.cpp
  src/archive.cpp
  src/audio.cpp
  src/events.cpp
  src/run_travel.cpp
  src/run_rewards.cpp
  src/space.cpp
  src/blueprints.cpp
  src/hangar.cpp
  src/menu.cpp
  src/menu_profile.cpp
    src/menu_support.cpp
    src/menu_credits.cpp
    src/menu_stats.cpp
    src/tutorial.cpp
    src/run_tutorial.cpp
  src/render.cpp
  src/resources.cpp
  src/run_combat.cpp
  src/run_drones.cpp
  src/run_content_combat.cpp
    src/run_campaign.cpp
    src/run_campaign_map.cpp
  src/run_species.cpp
  src/run_combat_render.cpp
  src/run.cpp
  src/profile.cpp
  src/run_control.cpp
  src/run_dev.cpp
  src/run_dev_ui.cpp
    src/run_voyage.cpp
    src/run_management_info.cpp
    src/run_store.cpp
    src/run_inventory.cpp
    src/run_environment.cpp src/run_beacon_images.cpp
    src/run_sun_render.cpp
    src/run_pulsar.cpp
    src/run_beacon_ship.cpp
    src/run_event_effects.cpp
    src/run_advanced.cpp
    src/run_crew_ai.cpp
    src/run_persistence.cpp
    src/run_original_save.cpp
    src/original_save.cpp
  src/run_render.cpp
  src/run_write_error.cpp
  src/settings.cpp
  src/travel.cpp
  src/ship_render.cpp
  src/ttf_font.cpp)
target_include_directories(FTLRecomp SYSTEM PRIVATE third_party)
target_link_libraries(FTLRecomp PRIVATE SDL3::SDL3-static freetype pugixml ftl_codecs)
if(WIN32)
  # original_save.cpp: SHGetFolderPathW for the original game's save folder.
  target_link_libraries(FTLRecomp PRIVATE shell32)
endif()
set(FTL_EXECUTABLE_NAME "FTLRecomp" CACHE STRING "Native executable basename")
set_target_properties(FTLRecomp PROPERTIES
  OUTPUT_NAME "${FTL_EXECUTABLE_NAME}"
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/build/$<CONFIG>"
  PDB_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/build/$<CONFIG>")
if(MSVC)
  # VS 2026 exposes newer features through latest mode. This is not a
  # declaration of complete C++26 conformance; portable code requires C++23.
  target_compile_options(FTLRecomp PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/std:c++latest>)
  target_compile_options(FTLRecomp PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /bigobj)
else()
  target_compile_options(FTLRecomp PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(FTL_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
if(FTL_WARNINGS_AS_ERRORS)
  if(MSVC)
    target_compile_options(FTLRecomp PRIVATE /WX)
  else()
    target_compile_options(FTLRecomp PRIVATE -Werror)
  endif()
endif()
# ftl_codecs instantiates vendored single-header libraries and deliberately does
# not inherit FTLRecomp's warning-as-error policy. Its wrappers are still linted.

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_target(format-check
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/code_quality.py" format
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" VERBATIM)
add_custom_target(format
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/code_quality.py" format --fix
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" VERBATIM)
if(CMAKE_EXPORT_COMPILE_COMMANDS)
  add_custom_target(lint
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/code_quality.py"
      lint --build-dir "${CMAKE_BINARY_DIR}"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" VERBATIM)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(P10MapSeeds tools/test_p10_map_seeds.cpp src/travel.cpp)
  # travel.cpp parses prepared beacon events with pugixml.
  target_link_libraries(P10MapSeeds PRIVATE pugixml)
  if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(P10MapSeeds PRIVATE Threads::Threads)
  endif()
  target_include_directories(P10MapSeeds PRIVATE src)
  target_include_directories(P10MapSeeds SYSTEM PRIVATE third_party)
  target_include_directories(P10MapSeeds SYSTEM PRIVATE $<TARGET_PROPERTY:SDL3::SDL3-static,INTERFACE_INCLUDE_DIRECTORIES>)
  set_target_properties(P10MapSeeds PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/build/$<CONFIG>"
    PDB_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/build/$<CONFIG>")
  if(MSVC)
    target_compile_options(P10MapSeeds PRIVATE /std:c++latest /W4 /permissive- /Zc:__cplusplus /utf-8)
  else()
    target_compile_options(P10MapSeeds PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  if(FTL_WARNINGS_AS_ERRORS)
    if(MSVC)
      target_compile_options(P10MapSeeds PRIVATE /WX)
    else()
      target_compile_options(P10MapSeeds PRIVATE -Werror)
    endif()
  endif()
  # Preserve platform-specific replay evidence on the shared checkout.
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(FTL_TEST_ARTIFACT_ROOT "${PROJECT_SOURCE_DIR}/build/Linux/$<CONFIG>/test-artifacts")
  else()
    set(FTL_TEST_ARTIFACT_ROOT "${PROJECT_SOURCE_DIR}/build/$<CONFIG>/test-artifacts")
  endif()
  add_test(NAME p10_hard_map_10000_seeds
    COMMAND "$<TARGET_FILE:P10MapSeeds>" --start 0 --count 10000 --threads 1
      --output "${FTL_TEST_ARTIFACT_ROOT}/P10/hard-map-seeds-ctest.json"
    CONFIGURATIONS Debug)
  set_tests_properties(p10_hard_map_10000_seeds PROPERTIES LABELS "P10;stress" TIMEOUT 3600)
  add_test(NAME automation_bootstrap
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_automation.py"
      --exe "$<TARGET_FILE:FTLRecomp>" --data "${PROJECT_SOURCE_DIR}/build/$<CONFIG>/ftl.dat")
  add_test(NAME research_foundation
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_foundation.py")
  add_test(NAME rendered_menu
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_menu.py"
      --exe "$<TARGET_FILE:FTLRecomp>" --data "${PROJECT_SOURCE_DIR}/build/$<CONFIG>/ftl.dat" --output "${FTL_TEST_ARTIFACT_ROOT}/P01")
  add_test(NAME rendered_hangar
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_hangar.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/P02/replay")
  add_test(NAME rendered_launch
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_run.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/P03/ctest")
  add_test(NAME rendered_live_ship
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_live_ship.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/P04/ctest")
  add_test(NAME rendered_travel
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_travel.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/P05/ctest")
  add_test(NAME independent_time
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_time.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/timing/cadence")
  set_tests_properties(independent_time PROPERTIES TIMEOUT 300)
  add_test(NAME first_complete_fight
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_combat.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/P06/ctest" --parts 8)
  add_test(NAME combat_checkpoint_integrity
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_p06_closeout.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/P06/checkpoint-integrity")
  add_test(NAME combat_authored_rewards
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_p06_rewards.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/P06/authored-rewards")
  add_test(NAME battery_allocation
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_battery.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/P06/battery-ctest")
  add_test(NAME developer_commands
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_dev_commands.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/dev-console/ctest")
  add_test(NAME developer_tabbed_ui
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_dev_ui.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/dev-ui/ctest")
  # Original-game continue.sav export (docs/research/original-save-format/).
  add_test(NAME original_save_export
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_original_save.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/original-save/ctest")
  set_tests_properties(original_save_export PROPERTIES TIMEOUT 300)
  add_test(NAME voyage_transactions
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_voyage.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/P07-P08/ctest")
  add_test(NAME native_persistence_integrity
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_persistence.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/P08/persistence")
  set_tests_properties(native_persistence_integrity PROPERTIES TIMEOUT 300)
  add_test(NAME p08_controls
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_p08_controls.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/P08/controls")
  set_tests_properties(p08_controls PROPERTIES TIMEOUT 300)
  add_test(NAME p08_credits
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_p08_credits.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/P08/credits")
  set_tests_properties(p08_credits PROPERTIES TIMEOUT 120)
  add_test(NAME p08_statistics
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_p08_stats.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/P08/p08_statistics")
  set_tests_properties(p08_statistics PROPERTIES TIMEOUT 180)
  add_test(NAME p08_tutorial
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_p08_tutorial.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/P08/p08_tutorial")
  set_tests_properties(p08_tutorial PROPERTIES TIMEOUT 300)
  add_test(NAME p08_tutorial_lifecycle
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_p08_tutorial_lifecycle.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/P08/p08_tutorial_lifecycle")
  set_tests_properties(p08_tutorial_lifecycle PROPERTIES TIMEOUT 120)
  add_test(NAME p08_options
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_p08_options.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/P08/p08_options")
  set_tests_properties(p08_options PROPERTIES TIMEOUT 180)
  add_test(NAME p07_ship_transactions
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_p07_transactions.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/P07/transactions")
  add_test(NAME p07_inventory_store
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_p07_inventory_store.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/P07/stores")
  add_test(NAME p07_travel
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_p07_travel.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/P07/travel")
  add_test(NAME p07_first_sector_replay
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_p07_travel.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/P07/first-sector" --full-replay-only --seed 2)
  add_test(NAME p07_first_sector_replay_advanced
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_p07_travel.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/P07/first-sector-advanced" --full-replay-only --seed 3 --advanced)
  set_tests_properties(p07_ship_transactions p07_inventory_store p07_travel p07_first_sector_replay p07_first_sector_replay_advanced PROPERTIES TIMEOUT 600)
  add_test(NAME advanced_systems
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_advanced.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/P09/ctest")
  foreach(suite p07_hud_hover p07_original_store_pairs p07_warning_ui p07_weapon_shield_visual
          p08_popup p08_progression_live p10_settings p10_store_access fire_presentation
          travel_visual_states)
    add_test(NAME ${suite}
      COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_${suite}.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/misc/${suite}")
  endforeach()
  set_tests_properties(p07_warning_ui p07_weapon_shield_visual p08_popup p08_progression_live
    p10_settings p10_store_access fire_presentation PROPERTIES TIMEOUT 120)
  set_tests_properties(p07_hud_hover p07_original_store_pairs travel_visual_states PROPERTIES TIMEOUT 180)
  add_test(NAME menu_timing
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_menu_timing.py" --exe "$<TARGET_FILE:FTLRecomp>" --data "${PROJECT_SOURCE_DIR}/build/$<CONFIG>/ftl.dat" --output "${FTL_TEST_ARTIFACT_ROOT}/misc/menu_timing/result.json")
  set_tests_properties(menu_timing PROPERTIES TIMEOUT 60)
  # Compiles src/profile.cpp itself with the Visual Studio LLVM clang++.
  add_test(NAME p08_progression
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_p08_progression.py" --output "${FTL_TEST_ARTIFACT_ROOT}/misc/p08_progression")
  set_tests_properties(p08_progression PROPERTIES TIMEOUT 180)
  # Plays the title music through the default audio device (exclude with -LE audio).
  add_test(NAME language
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_language.py" --exe "$<TARGET_FILE:FTLRecomp>" --data "${PROJECT_SOURCE_DIR}/build/$<CONFIG>/ftl.dat" --output "${FTL_TEST_ARTIFACT_ROOT}/misc/language")
  set_tests_properties(language PROPERTIES TIMEOUT 120 LABELS "audio")
  foreach(suite drones advanced content enemy shards)
    add_test(NAME p09_${suite}
      COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_p09_${suite}.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/P09/p09_${suite}")
  endforeach()
  set_tests_properties(p09_advanced p09_enemy PROPERTIES TIMEOUT 180)
  set_tests_properties(p09_content p09_shards PROPERTIES TIMEOUT 300)
  set_tests_properties(p09_drones PROPERTIES TIMEOUT 600)
  # Parity audit of 4 October 2026 (docs/in-progress/PARITY-AUDIT-2026-10-04.md):
  # one suite per area pins each audited row's original behaviour.
  foreach(area systems environment crew weapons drones world)
    add_test(NAME audit_${area}
      COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_audit_${area}.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/audit/${area}")
  endforeach()
  set_tests_properties(audit_systems audit_environment audit_crew audit_weapons audit_drones
    audit_world PROPERTIES LABELS "audit")
  set_tests_properties(audit_environment PROPERTIES TIMEOUT 300)
  set_tests_properties(audit_systems audit_weapons audit_world PROPERTIES TIMEOUT 600)
  # Found while repairing the suites (audit rows 101-102).
  add_test(NAME mind_control
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_mind_control.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/misc/mind_control")
  add_test(NAME map_events
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_map_events.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/misc/map_events")
  set_tests_properties(mind_control PROPERTIES TIMEOUT 600)
  set_tests_properties(map_events PROPERTIES TIMEOUT 120)
  foreach(suite losses terminal boarding hacking campaign)
    add_test(NAME p10_${suite}
      COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_p10_${suite}.py" --exe "$<TARGET_FILE:FTLRecomp>" --output "${FTL_TEST_ARTIFACT_ROOT}/P10/${suite}")
  endforeach()
  set_tests_properties(p10_losses PROPERTIES TIMEOUT 120)
  set_tests_properties(p10_terminal PROPERTIES TIMEOUT 600)
  set_tests_properties(p10_boarding PROPERTIES TIMEOUT 900)
  set_tests_properties(p10_hacking PROPERTIES TIMEOUT 1200)
  # The full campaign replay takes about 45 minutes (exclude with -LE stress).
  set_tests_properties(p10_campaign PROPERTIES TIMEOUT 3600 LABELS "P10;stress")
  set_tests_properties(audit_crew PROPERTIES TIMEOUT 900)
  set_tests_properties(audit_drones PROPERTIES TIMEOUT 1500)
  # Since 5 October 2026 the suite runs its check groups in eight parallel game
  # processes (--parts 8, about 90 s on this machine), automation renders only
  # frames that are read, and the cadence matrix sends batched advance_time
  # requests. Windows retains 600 s because this machine is shared and a loaded
  # desktop stretches every part. This is an infrastructure allowance only: no
  # simulation timing, assertion or expected pixel changes with it.
  # 29 September parallel closeout: the mounted-workspace Linux Debug replay
  # exceeded 600 s under concurrent platform gates, without an assertion failure.
  # Allow 1200 s for its larger replay/JSON transport; simulation gates are unchanged.
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set_tests_properties(first_complete_fight PROPERTIES TIMEOUT 1200)
  else()
    set_tests_properties(first_complete_fight PROPERTIES TIMEOUT 600)
  endif()
  set_tests_properties(rendered_travel PROPERTIES TIMEOUT 120)
  set_tests_properties(rendered_live_ship PROPERTIES TIMEOUT 120)
  set_tests_properties(rendered_menu PROPERTIES TIMEOUT 60)
  set_tests_properties(rendered_hangar PROPERTIES TIMEOUT 120)
  set_tests_properties(rendered_launch PROPERTIES TIMEOUT 120)
endif()
