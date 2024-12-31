include(FeatureSummary)

check_struct_has_member("struct stat" st_mtim "sys/types.h;sys/stat.h"
	HAVE_STRUCT_STAT_ST_MTIM LANGUAGE C)
check_struct_has_member("struct stat" st_mtimespec "sys/types.h;sys/stat.h"
	HAVE_STRUCT_STAT_ST_MTIMESPEC LANGUAGE C)
check_struct_has_member("struct stat" st_mtime_nsec sys/stat.h
	HAVE_STRUCT_STAT_MTIME_NSEC LANGUAGE C)

if(HAVE_STRUCT_STAT_ST_MTIM)
	check_struct_has_member("struct stat" st_mtim.tv_nsec sys/stat.h
		HAVE_STRUCT_STAT_ST_MTIM LANGUAGE C)
elseif(HAVE_STRUCT_STAT_ST_MTIMESPEC)
	check_struct_has_member("struct stat" st_mtimespec.tv_nsec sys/stat.h
		HAVE_STRUCT_STAT_ST_MTIMESPEC LANGUAGE C)
endif()

sanitizebool(USE_NSEC)

if((USE_NSEC STREQUAL ON OR USE_NSEC STREQUAL "") AND HAVE_STRUCT_STAT_ST_MTIM)
	set(USE_NSEC "mtim")
elseif((USE_NSEC STREQUAL ON OR USE_NSEC STREQUAL "") AND HAVE_STRUCT_STAT_ST_MTIMESPEC)
	set(USE_NSEC "mtimespec")
elseif((USE_NSEC STREQUAL ON OR USE_NSEC STREQUAL "") AND HAVE_STRUCT_STAT_MTIME_NSEC)
	set(USE_NSEC "mtime")
elseif((USE_NSEC STREQUAL ON OR USE_NSEC STREQUAL "") AND WIN32)
	set(USE_NSEC "win32")
elseif(USE_NSEC STREQUAL "")
	set(USE_NSEC OFF)
elseif(USE_NSEC STREQUAL ON)
        message(FATAL_ERROR "nanosecond support was requested but no platform support is available")
endif()

if(USE_NSEC STREQUAL "mtim")
        if(NOT HAVE_STRUCT_STAT_ST_MTIM)
                message(FATAL_ERROR "stat mtim could not be found")
        endif()

        set(GIT_NSEC 1)
        set(GIT_NSEC_MTIM 1)
        add_feature_info("Nanosecond support" ON "using mtim")
elseif(USE_NSEC STREQUAL "mtimespec")
        if(NOT HAVE_STRUCT_STAT_ST_MTIMESPEC)
                message(FATAL_ERROR "mtimespec could not be found")
        endif()

        set(GIT_NSEC 1)
        set(GIT_NSEC_MTIMESPEC 1)
        add_feature_info("Nanosecond support" ON "using mtimespec")
elseif(USE_NSEC STREQUAL "mtime")
        if(NOT HAVE_STRUCT_STAT_MTIME_NSEC)
                message(FATAL_ERROR "mtime_nsec could not be found")
        endif()

        set(GIT_NSEC 1)
        set(GIT_NSEC_MTIME 1)
        add_feature_info("Nanosecond support" ON "using mtime")
elseif(USE_NSEC STREQUAL "win32")
        if(NOT WIN32)
                message(FATAL_ERROR "Win32 API support is not available on this platform")
        endif()

        set(GIT_NSEC 1)
        set(GIT_NSEC_WIN32 1)
        add_feature_info("Nanosecond support" ON "using Win32 APIs")
elseif(USE_NSEC STREQUAL OFF)
        set(GIT_NSEC 0)
        add_feature_info("Nanosecond support" OFF "Nanosecond timestamp resolution is disabled")
else()
        message(FATAL_ERROR "unknown nanosecond option: ${USE_NSEC}")
endif()
