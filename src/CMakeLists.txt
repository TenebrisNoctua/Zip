luwow_add_library(Luwow.ZipLibrary
    NAME zip
    MODES SERIAL
    SOURCES
        Library.h
        Library.cpp
        Archive.h
        Archive.cpp
        Deflate.h
        Deflate.cpp
        Writer.h
        Writer.cpp
)
target_include_directories(Luwow.ZipLibrary PRIVATE ${ZIP_ROOT}/src)

# Paths and digests as the fs library has them
target_link_libraries(Luwow.ZipLibrary PRIVATE Luwow.FsCore)
