# check_dialog_patterns.cmake - build-time guard on the dialog rules
#
# Run via `cmake -DSEANCE_SRC_DIR=<dir> -P check_dialog_patterns.cmake` from a
# custom target, so it re-runs on every build rather than only at configure time.
#
# Why a lint and not a static_assert: the mistake this catches is *deriving from
# a JUCE class and overriding a virtual*, which is perfectly legal C++. There is
# no compile-time hook for "don't override this in a subclass of that", and the
# runtime symptom is a dialog that silently never appears - no crash, no
# warning, nothing in the log. Grepping the source is the only mechanism that
# fails early and says why.
#
# See REFERENCE.md -> "Dialogs and the Windows taskbar" for the full story.
#
# ---------------------------------------------------------------------------
# Three CMake traps, all hit while writing this file. Read before editing.
#
#  1. Do not iterate "lines" with file(STRINGS) + foreach(IN LISTS). CMake lists
#     are semicolon-separated, and file(STRINGS) does not escape the semicolons
#     in C++ source, so each "line" is really a fragment running from one `;` to
#     the next - spanning whole functions. A single such fragment happily
#     contained both a comment mentioning this rule and, hundreds of lines
#     later, the word "override".
#  2. Backslash escapes do not survive into the regex. "\\(" arrives as a plain
#     "(" (a capture group) and "\\*" arrives as a bare "*", which is not valid
#     regex at all - CMake prints a compile error to stderr and then treats the
#     match as simply false, so a broken pattern looks like a clean pass. Every
#     literal below therefore uses a bracket class - [(], [)], [*] - which needs
#     no escaping and cannot be mangled.
#  3. Patterns must not be allowed to span newlines. `[^;{]*` crosses line
#     breaks, so a paragraph of comment explaining this rule and later using the
#     word "override" matches the pattern for breaking it. The first version of
#     this script failed the build on its own documentation. Hence ${NL} is
#     excluded from every character class, and each pattern is anchored to the
#     start of a line.
#
# The anchor also does the comment filtering: a match must begin at a line whose
# first non-blank character is a letter or underscore. Declarations start with
# `class` / `int` / `virtual`; comment lines start with `/` or `*`.
# ---------------------------------------------------------------------------

if(NOT DEFINED SEANCE_SRC_DIR)
    message(FATAL_ERROR "check_dialog_patterns.cmake: SEANCE_SRC_DIR not set")
endif()

string(ASCII 10 NL)

# Overriding getDesktopWindowStyleFlags() is the sanctioned way to drop the
# taskbar flag for a DialogWindow, and dialog_helpers.cpp does exactly that
# (ToolDialogWindow). Everywhere else it is either a duplicate of that class -
# which should just call launchToolDialog() instead - or, far worse, an attempt
# to do the same to an AlertWindow, which does not work and makes the dialog
# invisible.
set(_flag_override
    "(^|${NL})[ \t]*[A-Za-z_][^;{${NL}]*getDesktopWindowStyleFlags[ \t]*[(][^)${NL}]*[)][^;{${NL}]*override")

# There is no remaining reason to subclass AlertWindow. Custom content goes
# through addCustomComponent() or launchToolDialog(); window flags are the
# look-and-feel's job. Every subclass of it that has existed in this codebase
# was an attempt at the above.
set(_alert_subclass
    "(^|${NL})[ \t]*[A-Za-z_][^;{${NL}]*public[ \t]+(juce::)?AlertWindow")

file(GLOB_RECURSE _sources "${SEANCE_SRC_DIR}/*.cpp" "${SEANCE_SRC_DIR}/*.h")

set(_violations "")

foreach(_file IN LISTS _sources)
    get_filename_component(_name "${_file}" NAME)
    file(READ "${_file}" _text)

    if(NOT _name STREQUAL "dialog_helpers.cpp" AND _text MATCHES "${_flag_override}")
        string(STRIP "${CMAKE_MATCH_0}" _hit)
        list(APPEND _violations
             "${_file}\n"
             "        ${_hit}\n"
             "      overrides getDesktopWindowStyleFlags().\n"
             "      For a custom dialog, call SoundShop::launchToolDialog() instead - it\n"
             "      already wraps that override in ToolDialogWindow.\n"
             "      For a juce::AlertWindow the override does NOT work: the peer is built\n"
             "      during construction, before the subclass vtable is live, so the override\n"
             "      is never consulted and the dialog silently never appears at all.\n"
             "      AlertWindow is handled app-wide by AppLookAndFeel - do nothing.\n\n")
    endif()

    if(_text MATCHES "${_alert_subclass}")
        string(STRIP "${CMAKE_MATCH_0}" _hit)
        list(APPEND _violations
             "${_file}\n"
             "        ${_hit}\n"
             "      derives from juce::AlertWindow.\n"
             "      Use aw.addCustomComponent() for extra controls, or\n"
             "      SoundShop::launchToolDialog() for a fully custom dialog.\n\n")
    endif()
endforeach()

if(_violations)
    string(REPLACE ";" "" _msg "${_violations}")
    message(FATAL_ERROR
        "\n"
        "Dialog rule violation (see REFERENCE.md -> 'Dialogs and the Windows taskbar'):\n"
        "\n"
        "  ${_msg}")
endif()
