# aleks2 (rc18): miniz 3.0.2 refuses valid zip64 archives over 2 GiB ("invalid header or archive is corrupted").
#
# Its central-directory check adds the entry's 32-bit local header offset to the entry's size and compares with the
# archive size -- also when that offset is the zip64 marker 0xFFFFFFFF, whose real value is in the zip64 extra
# field. A texture pack written with markers for every offset past 2 GiB (Alex's 2.79 GB pack: 2425 of 5039 entries)
# then fails to open on every platform. The check is skipped for a marker; miniz reads the real offset from the
# extra field when the entry is opened (mz_zip_file_stat_internal), and that path checks it.
#
# Run at every configure (idempotent, by marker): FetchContent does not re-patch a source it already has, and the
# Switch builds configure with FETCHCONTENT_FULLY_DISCONNECTED.
#   cmake -DSOURCE_DIR=<miniz source> -P PatchMiniz.cmake
file(READ "${SOURCE_DIR}/miniz.c" _content)
if (_content MATCHES "aleks2-zip64-local-offset")
    return()
endif ()
set(_old [=[            if (comp_size != MZ_UINT32_MAX)
            {
                if (((mz_uint64)MZ_READ_LE32(p + MZ_ZIP_CDH_LOCAL_HEADER_OFS) + MZ_ZIP_LOCAL_DIR_HEADER_SIZE + comp_size) > pZip->m_archive_size)]=])
set(_new [=[            /* aleks2-zip64-local-offset: 0xFFFFFFFF is the zip64 marker, not an offset (cmake/PatchMiniz.cmake) */
            if (comp_size != MZ_UINT32_MAX && MZ_READ_LE32(p + MZ_ZIP_CDH_LOCAL_HEADER_OFS) != MZ_UINT32_MAX)
            {
                if (((mz_uint64)MZ_READ_LE32(p + MZ_ZIP_CDH_LOCAL_HEADER_OFS) + MZ_ZIP_LOCAL_DIR_HEADER_SIZE + comp_size) > pZip->m_archive_size)]=])
string(FIND "${_content}" "${_old}" _at)
if (_at LESS 0)
    message(FATAL_ERROR "PatchMiniz.cmake: the zip64 local-offset check is not where miniz 3.0.2 has it; update the patch")
endif ()
string(REPLACE "${_old}" "${_new}" _content "${_content}")
file(WRITE "${SOURCE_DIR}/miniz.c" "${_content}")
message(STATUS "aleks2: miniz patched (zip64 local header offsets past 2 GiB)")
