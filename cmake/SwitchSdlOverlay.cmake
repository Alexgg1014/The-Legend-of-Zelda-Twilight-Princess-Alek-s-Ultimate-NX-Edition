# SwitchSdlOverlay
#
# The SDL3 the Switch build links is devkitPro's own Switch branch
# (github.com/devkitPro/SDL, branch switch-sdl-3.4, pinned in SwitchDeps.cmake),
# which already carries the console's audio, timer, clock, power, filesystem and
# threading support. Two parts of it do not fit this program, and this script
# swaps them for ours before SDL is configured (and makes two small changes to the
# audio driver, described below):
#
#   video     devkitPro's driver draws through EGL/OpenGL ES, which Dawn does not
#             use, and it also feeds touch, mouse and keyboard into SDL's event
#             queue, which would duplicate the touch bridge in src/dusk. Ours is a
#             window with no GL (platforms/switch/sdl/src/video/switch).
#   joystick  devkitPro's driver exposes eight pads at once; the game reads
#             player one, so ours exposes the merged "default" pad
#             (platforms/switch/sdl/src/joystick/switch).
#
# Run as FetchContent's PATCH_COMMAND:
#   cmake -DSDL_SOURCE_DIR=<dir> -DOVERLAY_DIR=<dir> -P SwitchSdlOverlay.cmake
# Safe to run on a tree it has already changed.

if(NOT EXISTS "${SDL_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "SwitchSdlOverlay: not an SDL tree: '${SDL_SOURCE_DIR}'")
endif()
if(NOT IS_DIRECTORY "${OVERLAY_DIR}/src")
  message(FATAL_ERROR "SwitchSdlOverlay: no overlay at '${OVERLAY_DIR}'")
endif()

# 1. devkitPro's video directory goes, ours comes in. SDL's build globs
#    src/video/switch/*.c, so a leftover file would be compiled.
file(GLOB _their_video "${SDL_SOURCE_DIR}/src/video/switch/*")
if(_their_video)
  file(REMOVE ${_their_video})
endif()
file(COPY "${OVERLAY_DIR}/src/" DESTINATION "${SDL_SOURCE_DIR}/src")

# 2. devkitPro's audio driver is kept, with two changes.
#
#    Channels. It opens the audio renderer with the channel count it is asked for
#    and only knows how to mix one or two, while the game may ask for 6 or 8 (its
#    surround settings). The console outputs stereo, so ask SDL for at most two
#    and let SDL's own stream conversion do the downmix.
#
#    Waiting. Its WaitDevice returns at once, so SDL mixes the next chunk before
#    there is a free buffer to put it in, and PlayDevice then has to drop it when
#    both are still busy: an audible glitch. SDL's contract for WaitDevice is to
#    block until the device can take more, so that is what it does here, using
#    the audio renderer's own buffer states.
file(READ "${SDL_SOURCE_DIR}/src/audio/switch/SDL_switchaudio.c" _audio)

set(_channels_anchor "    device->spec.format = test_format;\n")
set(_channels_edit "    device->spec.channels = SDL_min(device->spec.channels, 2);\n")
string(FIND "${_audio}" "${_channels_edit}" _channels_done)
if(_channels_done EQUAL -1)
  string(FIND "${_audio}" "${_channels_anchor}" _channels_at)
  if(_channels_at EQUAL -1)
    message(FATAL_ERROR "SwitchSdlOverlay: devkitPro's audio driver changed; OpenDevice no longer matches.")
  endif()
  string(REPLACE "${_channels_anchor}" "${_channels_anchor}${_channels_edit}" _audio "${_audio}")
endif()

set(_wait_old "static bool SWITCHAUDIO_WaitDevice(SDL_AudioDevice *device)\n{\n    return true;\n}\n")
set(_wait_new "static bool SWITCHAUDIO_WaitDevice(SDL_AudioDevice *device)
{
    // Block until one of the two wave buffers is free again.
    for (;;) {
        audrvUpdate(&device->hidden->driver);
        for (int i = 0; i < 2; i++) {
            const AudioDriverWaveBufState state = device->hidden->buffer[i].state;
            if (state == AudioDriverWaveBufState_Free || state == AudioDriverWaveBufState_Done) {
                return true;
            }
        }
        audrenWaitFrame();
    }
}
")
string(FIND "${_audio}" "${_wait_old}" _wait_at)
if(_wait_at EQUAL -1)
  string(FIND "${_audio}" "Block until one of the two wave buffers" _wait_done)
  if(_wait_done EQUAL -1)
    message(FATAL_ERROR "SwitchSdlOverlay: devkitPro's audio driver changed; WaitDevice no longer matches.")
  endif()
else()
  string(REPLACE "${_wait_old}" "${_wait_new}" _audio "${_audio}")
endif()
file(WRITE "${SDL_SOURCE_DIR}/src/audio/switch/SDL_switchaudio.c" "${_audio}")

# 3. Thread priorities. devkitPro's SDL turns every SDL_SetCurrentThreadPriority into
#    svcSetThreadPriority: 0x2B for "high", 0x3B for everything else. 0x3B is the
#    time-sliced range, below the 0x2C a libnx thread starts with, so the audio thread
#    (which asks for "time critical", not "high") and every normal thread would drop
#    in priority. The program places its threads by core (extern/aurora/lib/thread.cpp)
#    and has always left priorities as libnx made them, so the request is accepted and
#    ignored.
file(READ "${SDL_SOURCE_DIR}/src/thread/pthread/SDL_systhread.c" _thread)
string(FIND "${_thread}" "#elif SDL_PLATFORM_SWITCH\n    Result res;" _prio_at)
if(_prio_at EQUAL -1)
  string(FIND "${_thread}" "Left as libnx made it" _prio_done)
  if(_prio_done EQUAL -1)
    message(FATAL_ERROR "SwitchSdlOverlay: devkitPro's SDL_SYS_SetThreadPriority is not the one this script was written against.")
  endif()
else()
  string(SUBSTRING "${_thread}" 0 ${_prio_at} _prio_head)
  string(SUBSTRING "${_thread}" ${_prio_at} -1 _prio_tail)
  string(FIND "${_prio_tail}" "    return 0;\n" _prio_ret)
  if(_prio_ret EQUAL -1)
    message(FATAL_ERROR "SwitchSdlOverlay: cannot find the end of the Switch priority branch.")
  endif()
  string(LENGTH "    return 0;\n" _prio_ret_len)
  math(EXPR _prio_end "${_prio_ret} + ${_prio_ret_len}")
  string(SUBSTRING "${_prio_tail}" ${_prio_end} -1 _prio_rest)
  file(WRITE "${SDL_SOURCE_DIR}/src/thread/pthread/SDL_systhread.c"
    "${_prio_head}#elif SDL_PLATFORM_SWITCH\n    // Left as libnx made it: a libnx thread starts at 0x2C and the program places\n    // its threads by core, not by priority.\n    return true;\n${_prio_rest}")
endif()

# 4. The Switch block of SDL's CMakeLists asks for EGL and OpenGL after naming the
#    video sources. Ours needs neither, and asking would put EGL, glapi and
#    drm_nouveau on the final link line.
file(READ "${SDL_SOURCE_DIR}/CMakeLists.txt" _cmake)
string(FIND "${_cmake}" "elseif(NINTENDO_SWITCH)\n" _block)
if(_block EQUAL -1)
  message(FATAL_ERROR "SwitchSdlOverlay: SDL's CMakeLists has no NINTENDO_SWITCH block; "
    "this is not devkitPro's Switch branch.")
endif()
# Everything before the block stays; the lines to cut are searched for after it.
string(SUBSTRING "${_cmake}" 0 ${_block} _before)
string(SUBSTRING "${_cmake}" ${_block} -1 _from_block)
string(FIND "${_from_block}" "    CheckEGL()\n" _first)
string(FIND "${_from_block}" "-DSDL_VIDEO_STATIC_ANGLE)\n" _last)
if(_first EQUAL -1 AND _last EQUAL -1)
  message(STATUS "SwitchSdlOverlay: SDL's CMakeLists already trimmed")
elseif(_first EQUAL -1 OR _last EQUAL -1 OR _last LESS _first)
  message(FATAL_ERROR "SwitchSdlOverlay: the Switch video block of SDL's CMakeLists "
    "is not the one this script was written against; read the block and adjust it.")
else()
  string(LENGTH "-DSDL_VIDEO_STATIC_ANGLE)\n" _last_len)
  math(EXPR _tail "${_last} + ${_last_len}")
  string(SUBSTRING "${_from_block}" 0 ${_first} _head)
  string(SUBSTRING "${_from_block}" ${_tail} -1 _rest)
  file(WRITE "${SDL_SOURCE_DIR}/CMakeLists.txt"
    "${_before}${_head}    # aleks2: no GL/EGL checks, the video driver does not use them.\n${_rest}")
  message(STATUS "SwitchSdlOverlay: replaced devkitPro's video and joystick drivers")
endif()
