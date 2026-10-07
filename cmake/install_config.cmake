# SPDX-FileCopyrightText: 2026 VectorText contributors
# SPDX-License-Identifier: GPL-3.0-only
# Rebuilding must not silently overwrite the user's runtime configuration.
if(NOT EXISTS "${DEST_INI}")
    configure_file("${SOURCE_INI}" "${DEST_INI}" COPYONLY)
endif()
